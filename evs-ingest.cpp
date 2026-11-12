#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <ctime>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "evs-common.h"

// Zeitstempel-Zuordnung (PTS -> Master-Tick)
constexpr int64_t FRAME_NS = 1000000000LL / FPS;
constexpr int64_t HARD_REANCHOR_TICKS = 250; // > 5 s Abweichung = PTS-Sprung / Neustart der Quelle

// SRT: Der Ingest ist Listener, OBS ruft an (in OBS: srt://IP_DES_REPLAY_RECHNERS:20010?latency=200000).
// (RIST wurde verworfen: OBS sendet nur Main Profile und stürzt auf macOS beim Start des RIST-Ausgangs ab.)
const std::array<int, NUM_CHANNELS> SRT_PORTS = {20010, 20020, 20030, 20040, 20050, 20060};
constexpr int SRT_LATENCY_MS = 200;  // Empfangspuffer für Neuübertragungen; im LAN reichen 100–300 ms

std::atomic<bool> g_running{true};
std::atomic<uint64_t> g_master_tick{0};

// --- Kontext pro Kamera-Kanal ---
struct ChannelContext {
    int cam_id;
    int srt_port;
    // Quelle dieses REC-Wegs: "srt" (OBS ruft an), "sdi" (Blackmagic DeckLink) oder "test" (Testbild + Ton)
    std::string input = "srt";
    int sdi_dev = 0;                 // DeckLink-Gerätenummer (Anschluss), nur bei "sdi"
    std::string sdi_mode = "1080p50";
    bool warned_delta = false;       // Warnung "kein All-Intra" schon ausgegeben
    int fd = -1;
    uint64_t current_write_offset = 0;
    uint32_t generation = 0;
    uint64_t frames_received = 0;
    SharedReplayIndex* shm_index = nullptr;
    GstElement* pipeline = nullptr;

    // PTS-Anker: ordnet den Stream-PTS dem Master-Tick zu (statt Ankunftszeit zu verwenden)
    bool anchored = false;
    uint64_t base_pts = 0;
    int64_t base_tick = 0;
    int window_count = 0;
    int64_t window_min = INT64_MAX;

    // --- Ton ---
    GstElement* video_in = nullptr;  // Ziel des Demuxers für H.264
    GstElement* audio_in = nullptr;  // Ziel des Demuxers für AAC (nullptr = Ton nicht verfügbar)
    int afd = -1;                    // cam_N.aud
    // Kopie des Video-Ankers (vom Video-Thread geschrieben, vom Ton-Thread gelesen), in Samples
    std::mutex anchor_mx;
    bool a_valid = false;
    int64_t a_base_samples = 0;
    int64_t a_base_tick = 0;
    // Zustand der Ton-Zerlegung (nur vom Ton-Thread benutzt)
    std::vector<int32_t> abuf;       // interleaved Stereo, beginnt bei Sample abuf_start
    int64_t abuf_start = 0;
    bool abuf_have = false;
    int64_t a_next_t = 0;            // nächster zu schreibender Tick (0 = noch unbestimmt)
    std::atomic<uint64_t> audio_blocks{0};
};

void signal_handler(int) {
    g_running = false;
}

// --- 50Hz Master-Clock Thread (20.000.000 ns Takt) ---
// Der Tick ist die Systemzeit in 20-ms-Schritten (siehe evs-common.h). Ein Sprung der Systemuhr
// (mehr als 1 s) wird gemeldet; rückwärts läuft der Tick nie.
void master_clock_thread_func(SharedReplayIndex* shm_index) {
    uint64_t last = g_master_tick.load();

    while (g_running) {
        uint64_t target = last + 1;
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(target / FPS);
        ts.tv_nsec = static_cast<long>((target % FPS) * (1000000000ULL / FPS));
        clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, nullptr);

        uint64_t now = realtime_tick_now();
        if (now + FPS < target) {
            // Systemuhr ist deutlich zurückgesprungen: Ticks nicht rückwärts laufen lassen, kurz warten
            std::cerr << "\n[WARN] Systemuhr ist zurückgesprungen (" << (target - now) / FPS
                      << " s). Tick pausiert, bis die Uhr aufgeholt hat.\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (now > target + FPS)
            std::cerr << "\n[WARN] Systemuhr bzw. Ingest hat " << (now - target) / FPS << " s übersprungen.\n";

        uint64_t tick = std::max(now, target);
        last = tick;
        g_master_tick.store(tick, std::memory_order_release);
        shm_index->current_master_tick.store(tick, std::memory_order_release);
    }
}

// --- PTS -> Master-Tick ---
// Beim ersten Frame wird (PTS <-> Tick) verankert, danach ergibt sich der Tick aus dem PTS-Abstand.
// So ist der Tick unabhängig vom Ankunfts-Jitter (SRT-Neuübertragungen, Scheduling) und es gibt keine
// doppelt belegten / leeren Slots mehr. Die Kamera-Drift gegenüber der Master-Clock wird langsam
// nachgeführt:
//  - Kommt ein Frame >= 2 Ticks FRÜHER als erwartet, war der Anker zu spät -> sofort nachziehen.
//  - Waren alle Frames einer Sekunde >= 2 Ticks SPÄTER als erwartet, driftet die Quelle -> nachziehen.
//    (Einzelne verspätete Frames durch Retransmits lösen das bewusst nicht aus.)
//  - Abweichung > 5 s (PTS-Sprung, Kamera-Neustart) -> komplett neu verankern.
static void anchor_to(ChannelContext& c, uint64_t pts, int64_t now_tick) {
    c.anchored = true;
    c.base_pts = pts;
    c.base_tick = now_tick;
    c.window_count = 0;
    c.window_min = INT64_MAX;
}

static int64_t assign_tick(ChannelContext& c, GstClockTime pts, int64_t now_tick) {
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return now_tick; // kein PTS -> Ankunftszeit

    if (!c.anchored) {
        anchor_to(c, pts, now_tick);
        return now_tick;
    }

    int64_t dpts = static_cast<int64_t>(pts) - static_cast<int64_t>(c.base_pts);
    int64_t frames = (dpts >= 0 ? dpts + FRAME_NS / 2 : dpts - FRAME_NS / 2) / FRAME_NS; // gerundet
    int64_t expected = c.base_tick + frames;
    int64_t delta = now_tick - expected;

    if (delta > HARD_REANCHOR_TICKS || delta < -HARD_REANCHOR_TICKS) {
        c.shm_index->stats[c.cam_id].reanchors.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "\n[WARN] REC" << (c.cam_id + 1) << ": PTS-Sprung (" << delta
                  << " Ticks), verankere neu\n";
        anchor_to(c, pts, now_tick);
        return now_tick;
    }

    if (delta < -1) {            // Frame kam deutlich früher als erwartet -> Anker nachziehen
        c.base_tick += delta;
        expected += delta;
        delta = 0;
        c.shm_index->stats[c.cam_id].reanchors.fetch_add(1, std::memory_order_relaxed);
    }

    c.window_min = std::min(c.window_min, delta);
    if (++c.window_count >= static_cast<int>(FPS)) {
        if (c.window_min >= 2) { // dauerhaft verspätet -> Drift
            c.base_tick += c.window_min;
            expected += c.window_min;
            c.shm_index->stats[c.cam_id].reanchors.fetch_add(1, std::memory_order_relaxed);
        }
        c.window_min = INT64_MAX;
        c.window_count = 0;
    }
    return expected;
}

// --- GStreamer Callback: Wird für jeden empfangenen H.264 Frame aufgerufen ---
static GstFlowReturn on_new_sample(GstAppSink* appsink, gpointer user_data) {
    auto* ctx = static_cast<ChannelContext*>(user_data);

    GstSample* sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer) {
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            uint32_t frame_size = static_cast<uint32_t>(map.size);

            // Ringbuffer Wrap-Around prüfen (Wenn Ende der Datei erreicht -> zurück auf 0, neue Generation)
            if (ctx->current_write_offset + frame_size > MAX_FILE_SIZE_BYTES) {
                ctx->current_write_offset = 0;
                ctx->generation = (ctx->generation + 1) & GEN_MASK;
            }

            // Schreibposition VOR dem Schreiben veröffentlichen: Der Playout erkennt damit, welche
            // alten Frames gerade überschrieben werden (seq_cst, damit die Prüfung nach dem pread() greift).
            ctx->shm_index->write_pos[ctx->cam_id].store(
                (static_cast<uint64_t>(ctx->generation) << GEN_SHIFT) | ctx->current_write_offset);

            // Frame per pwrite() direkt an die Zieladresse der NVMe-Datei schreiben
            ssize_t written = pwrite(ctx->fd, map.data, frame_size, ctx->current_write_offset);
            if (written == static_cast<ssize_t>(frame_size)) {
                int64_t arrival_tick = static_cast<int64_t>(g_master_tick.load(std::memory_order_acquire));
                if (arrival_tick < 1) arrival_tick = 1;

                int64_t tick = assign_tick(*ctx, GST_BUFFER_PTS(buffer), arrival_tick);

                // Anker für die Ton-Zuordnung bereitstellen (PTS -> Sample-Position)
                if (ctx->anchored && GST_BUFFER_PTS_IS_VALID(buffer)) {
                    std::lock_guard<std::mutex> lk(ctx->anchor_mx);
                    ctx->a_valid = true;
                    ctx->a_base_samples = static_cast<int64_t>(ctx->base_pts) * 6 / 125000;  // ns -> 48 kHz
                    ctx->a_base_tick = ctx->base_tick;
                }

                // Kamera-Latenzkorrektur (positiv = später, negativ = früher)
                int64_t synced_tick = tick + ctx->shm_index->camera_latency_offset[ctx->cam_id];
                if (synced_tick < 1) synced_tick = tick;

                size_t slot_idx = static_cast<uint64_t>(synced_tick) % TOTAL_SLOTS;
                bool is_key = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
                if (!is_key && !ctx->warned_delta) {
                    ctx->warned_delta = true;
                    std::cerr << "\n[WARN] REC" << (ctx->cam_id + 1)
                              << ": Quelle liefert keine reinen I-Frames – Wiedergabe/Shuttle braucht All-Intra!\n";
                }

                // In Shared-Memory-Index eintragen (Seqlock: erst ungültig, Felder, dann Commit)
                FrameSlot& slot = ctx->shm_index->slots[ctx->cam_id][slot_idx];
                slot.master_tick.store(0, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                slot.pts_ns.store(GST_BUFFER_PTS(buffer), std::memory_order_relaxed);
                slot.file_offset.store(ctx->current_write_offset, std::memory_order_relaxed);
                slot.frame_size.store(frame_size, std::memory_order_relaxed);
                slot.generation.store(ctx->generation, std::memory_order_relaxed);
                slot.is_keyframe.store(is_key ? 1 : 0, std::memory_order_relaxed);
                slot.master_tick.store(static_cast<uint64_t>(synced_tick), std::memory_order_release);

                ctx->current_write_offset += frame_size;
                ctx->frames_received++;
                ctx->shm_index->stats[ctx->cam_id].frames_received.store(ctx->frames_received, std::memory_order_relaxed);
            }
            gst_buffer_unmap(buffer, &map);
        }
    }

    gst_sample_unref(sample);
    return g_running ? GST_FLOW_OK : GST_FLOW_EOS;
}

// --- Ton: AAC -> PCM-Blöcke pro Master-Tick ---
static int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int64_t ceil_div(int64_t a, int64_t b) { return floor_div(a + b - 1, b); }

// Schneidet aus dem angesammelten Ton alle vollständigen 20-ms-Blöcke aus und schreibt sie in die .aud-Datei.
// Block für Tick t beginnt bei Sample A(t) = base_samples + (t - base_tick) * 960 (gleicher Anker wie das Bild).
static void emit_audio_blocks(ChannelContext& c) {
    int64_t base_s, base_t;
    {
        std::lock_guard<std::mutex> lk(c.anchor_mx);
        if (!c.a_valid) {  // Bild noch nicht verankert: nur die letzte Sekunde vorhalten
            size_t max_vals = static_cast<size_t>(AUDIO_RATE) * AUDIO_CHANNELS;
            if (c.abuf.size() > max_vals) {
                size_t drop = c.abuf.size() - max_vals;
                c.abuf.erase(c.abuf.begin(), c.abuf.begin() + drop);
                c.abuf_start += static_cast<int64_t>(drop / AUDIO_CHANNELS);
            }
            return;
        }
        base_s = c.a_base_samples;
        base_t = c.a_base_tick;
    }
    const int64_t SPF = AUDIO_SAMPLES_PER_FRAME;
    auto A = [&](int64_t t) { return base_s + (t - base_t) * SPF; };

    if (c.a_next_t == 0) c.a_next_t = base_t + ceil_div(c.abuf_start - base_s, SPF);

    for (;;) {
        int64_t buf_end = c.abuf_start + static_cast<int64_t>(c.abuf.size() / AUDIO_CHANNELS);
        int64_t start = A(c.a_next_t);
        if (start < c.abuf_start) {  // Anker hat sich verschoben, Ton von früher ist schon verworfen
            c.a_next_t += ceil_div(c.abuf_start - start, SPF);
            start = A(c.a_next_t);
        }
        if (start > buf_end + 5 * static_cast<int64_t>(AUDIO_RATE)) {  // Anker weit voraus (Sprung): neu beginnen
            c.abuf.clear();
            c.abuf_start = buf_end;
            c.a_next_t = 0;
            return;
        }
        if (start + SPF > buf_end) break;  // Block noch nicht vollständig

        int64_t tick = c.a_next_t + c.shm_index->camera_latency_offset[c.cam_id];
        if (tick < 1) tick = c.a_next_t;
        if (tick >= 1 && c.afd >= 0) {
            uint8_t blk[AUDIO_SLOT_BYTES];
            uint64_t t64 = static_cast<uint64_t>(tick);
            std::memcpy(blk, &t64, sizeof(t64));
            std::memcpy(blk + sizeof(t64), &c.abuf[static_cast<size_t>(start - c.abuf_start) * AUDIO_CHANNELS],
                        AUDIO_PAYLOAD_BYTES);
            off_t off = static_cast<off_t>((t64 % TOTAL_SLOTS) * AUDIO_SLOT_BYTES);
            if (pwrite(c.afd, blk, AUDIO_SLOT_BYTES, off) == static_cast<ssize_t>(AUDIO_SLOT_BYTES))
                c.audio_blocks.fetch_add(1, std::memory_order_relaxed);
        }
        c.a_next_t++;
    }

    // Verbrauchten Ton verwerfen
    int64_t buf_end = c.abuf_start + static_cast<int64_t>(c.abuf.size() / AUDIO_CHANNELS);
    int64_t keep_from = std::min(A(c.a_next_t), buf_end);
    if (keep_from > c.abuf_start) {
        c.abuf.erase(c.abuf.begin(), c.abuf.begin() + (keep_from - c.abuf_start) * AUDIO_CHANNELS);
        c.abuf_start = keep_from;
    }
}

// Wird für jeden dekodierten Audio-Puffer aufgerufen (S32LE, 48 kHz, beliebige Kanalzahl).
// Es werden nur Kanal 1/2 übernommen; Mono wird auf beide Seiten gelegt.
static GstFlowReturn on_new_audio(GstAppSink* appsink, gpointer user_data) {
    auto* c = static_cast<ChannelContext*>(user_data);
    GstSample* sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps* caps = gst_sample_get_caps(sample);
    int in_ch = 2;
    if (caps) gst_structure_get_int(gst_caps_get_structure(caps, 0), "channels", &in_ch);

    GstMapInfo map;
    if (buffer && in_ch >= 1 && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        size_t n = map.size / (sizeof(int32_t) * static_cast<size_t>(in_ch));
        const int32_t* in = reinterpret_cast<const int32_t*>(map.data);

        int64_t buf_end = c->abuf_start + static_cast<int64_t>(c->abuf.size() / AUDIO_CHANNELS);
        GstClockTime pts = GST_BUFFER_PTS(buffer);
        int64_t s0 = GST_CLOCK_TIME_IS_VALID(pts) ? static_cast<int64_t>(pts) * 6 / 125000 : buf_end;

        if (!c->abuf_have) {
            c->abuf.clear();
            c->abuf_start = s0;
            c->abuf_have = true;
            c->a_next_t = 0;
        } else {
            int64_t diff = s0 - buf_end;  // kleine Abweichungen (PTS-Rundung) ignorieren
            if (diff > 96) {
                if (diff <= static_cast<int64_t>(AUDIO_RATE)) {  // kurze Lücke: mit Stille füllen
                    c->abuf.insert(c->abuf.end(), static_cast<size_t>(diff) * AUDIO_CHANNELS, 0);
                } else {                                         // große Lücke / Sprung: neu beginnen
                    c->abuf.clear();
                    c->abuf_start = s0;
                    c->a_next_t = 0;
                }
            } else if (diff < -96) {                             // Rücksprung: neu beginnen
                c->abuf.clear();
                c->abuf_start = s0;
                c->a_next_t = 0;
            }
        }

        size_t old = c->abuf.size();
        c->abuf.resize(old + n * AUDIO_CHANNELS);
        for (size_t i = 0; i < n; ++i) {
            int32_t l = in[i * in_ch];
            int32_t r = in_ch > 1 ? in[i * in_ch + 1] : l;
            c->abuf[old + i * 2] = l;
            c->abuf[old + i * 2 + 1] = r;
        }
        gst_buffer_unmap(buffer, &map);
        emit_audio_blocks(*c);
    }
    gst_sample_unref(sample);
    return g_running ? GST_FLOW_OK : GST_FLOW_EOS;
}

// --- Dynamisches Verknüpfen des MPEG-TS Demuxers ---
static void on_demux_pad_added(GstElement*, GstPad* new_pad, gpointer user_data) {
    auto* c = static_cast<ChannelContext*>(user_data);

    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps) gst_caps_unref(caps);
        return;
    }
    const gchar* name = gst_structure_get_name(gst_caps_get_structure(caps, 0));

    GstElement* target = nullptr;
    if (g_str_has_prefix(name, "video/x-h264")) {
        target = c->video_in;
    } else if (g_str_has_prefix(name, "audio/mpeg")) {
        target = c->audio_in;  // AAC; nullptr, falls die Audio-Elemente fehlen
    } else {
        std::cerr << "[WARN] REC" << (c->cam_id + 1) << ": Stream '" << name << "' wird nicht unterstützt (nur H.264 + AAC)\n";
    }

    if (target) {
        GstPad* sink_pad = gst_element_get_static_pad(target, "sink");
        if (!gst_pad_is_linked(sink_pad)) gst_pad_link(new_pad, sink_pad);
        gst_object_unref(sink_pad);
    }
    gst_caps_unref(caps);
}

// ---------------------------------------------------------------------------------------------
// Live-Quellen ohne Netzwerk: SDI (Blackmagic DeckLink) und Testquelle.
// Das Bild kommt unkomprimiert und wird hier zu All-Intra-H.264 (jedes Bild ein I-Frame) kodiert,
// danach läuft alles wie bei SRT: Ring-Datei, Index, Ton-Ring. Ton: eingebettet, Kanal 1/2.
// ---------------------------------------------------------------------------------------------
static bool has_element(const char* name) {
    GstElementFactory* f = gst_element_factory_find(name);
    if (f) gst_object_unref(f);
    return f != nullptr;
}

// Setzt eine Eigenschaft (auch Aufzählungen, per Text) nur, wenn das Element sie kennt
static bool set_arg_if_exists(GstElement* e, const std::string& prop, const std::string& val) {
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(e), prop.c_str())) return false;
    gst_util_set_object_arg(G_OBJECT(e), prop.c_str(), val.c_str());
    return true;
}

typedef std::vector<std::pair<std::string, std::string>> Props;

// All-Intra-Encoder: NVIDIA NVENC, Intel Quick Sync, sonst x264 (Software, sehr rechenintensiv).
// Auswahl erzwingen mit der Umgebungsvariable EVS_HW=nv|va|sw.
static std::string pick_intra_encoder(int kbps, std::string& label, Props& props) {
    const std::string pref = hw_preference();
    const std::string br = std::to_string(kbps);
    if ((pref.empty() || pref == "nv") && has_element("nvh264enc")) {
        label = "NVIDIA NVENC";
        props = {{"bitrate", br}, {"gop-size", "1"}, {"bframes", "0"}, {"rc-mode", "cbr"}, {"zerolatency", "true"}};
        return "nvh264enc name=venc";
    }
    if (pref != "sw" && pref != "nv" && has_element("vah264enc")) {
        label = "Intel Quick Sync (va)";
        props = {{"bitrate", br}, {"key-int-max", "1"}, {"b-frames", "0"}, {"rate-control", "cbr"}};
        return "vah264enc name=venc";
    }
    if (pref != "sw" && pref != "nv" && has_element("vaapih264enc")) {
        label = "Intel Quick Sync (vaapi)";
        props = {{"bitrate", br}, {"keyframe-period", "1"}, {"max-bframes", "0"}, {"rate-control", "cbr"}};
        return "vaapih264enc name=venc";
    }
    label = "Software (x264) – sehr rechenintensiv";
    props = {{"bitrate", br}, {"key-int-max", "1"}, {"bframes", "0"}, {"tune", "zerolatency"},
             {"speed-preset", "ultrafast"}};
    return "x264enc name=venc";
}

constexpr int INTRA_KBPS = 90000;  // wie die OBS-Streams (~92 Mbit/s); Ring-Größe in evs-common.h beachten

static bool init_live_pipeline(ChannelContext& ctx) {
    const std::string cam = "REC" + std::to_string(ctx.cam_id + 1);
    std::string vsrc, asrc;

    if (ctx.input == "sdi") {
        if (!has_element("decklinkvideosrc") || !has_element("decklinkaudiosrc")) {
            std::cerr << "[Fehler] " << cam << ": GStreamer-Plugin für DeckLink fehlt "
                         "(prüfen: gst-inspect-1.0 decklinkvideosrc; Blackmagic Desktop Video installiert?)\n";
            return false;
        }
        if (ctx.sdi_mode.find('i') != std::string::npos && ctx.sdi_mode.find("p") == std::string::npos)
            std::cerr << "[WARN] " << cam << ": Zeilensprung-Modus " << ctx.sdi_mode
                      << " wird nicht entflochten – nur progressive Modi (z.B. 1080p50) werden unterstützt.\n";
        vsrc = "decklinkvideosrc name=vsrc device-number=" + std::to_string(ctx.sdi_dev) + " mode=" + ctx.sdi_mode;
        asrc = "decklinkaudiosrc name=asrc device-number=" + std::to_string(ctx.sdi_dev) + " channels=2";
    } else {  // "test"
        vsrc = "videotestsrc name=vsrc is-live=true pattern=ball ! "
               "video/x-raw,width=1920,height=1080,framerate=50/1";
        asrc = "audiotestsrc name=asrc is-live=true wave=sine volume=0.2 freq=" +
               std::to_string(440 + 110 * ctx.cam_id) + " ! audio/x-raw,rate=48000,channels=2";
    }

    std::string label;
    Props props;
    const std::string enc = pick_intra_encoder(INTRA_KBPS, label, props);

    // Warteschlange zwischen Karte und Encoder: höchstens 25 unkomprimierte Bilder (~100 MB), Überlauf = Bild verwerfen
    std::string desc =
        vsrc + " ! queue max-size-buffers=25 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "videoconvert ! video/x-raw,format=NV12 ! " + enc +
        " ! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au ! "
        "appsink name=sink";
    if (ctx.afd >= 0)
        desc += " " + asrc +
                " ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=2000000000 ! audioconvert ! "
                "audioresample ! audio/x-raw,format=S32LE,rate=48000,channels=2,layout=interleaved ! "
                "appsink name=asink";

    GError* err = nullptr;
    ctx.pipeline = gst_parse_launch(desc.c_str(), &err);
    if (err || !ctx.pipeline) {
        std::cerr << "[Fehler] " << cam << ": Pipeline: " << (err ? err->message : "unbekannt") << "\n";
        if (err) g_error_free(err);
        if (ctx.pipeline) { gst_object_unref(ctx.pipeline); ctx.pipeline = nullptr; }
        return false;
    }

    if (GstElement* venc = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "venc")) {
        for (auto& p : props) set_arg_if_exists(venc, p.first, p.second);
        gst_object_unref(venc);
    }

    GstElement* vsink = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "sink");
    if (!vsink) return false;
    g_object_set(G_OBJECT(vsink), "emit-signals", FALSE, "sync", FALSE, "async", FALSE,
                 "max-buffers", 8, "drop", TRUE, nullptr);
    GstAppSinkCallbacks vcb = {};
    vcb.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(vsink), &vcb, &ctx, nullptr);
    gst_object_unref(vsink);

    if (GstElement* asink = gst_bin_get_by_name(GST_BIN(ctx.pipeline), "asink")) {
        g_object_set(G_OBJECT(asink), "emit-signals", FALSE, "sync", FALSE, "async", FALSE,
                     "max-buffers", 200, "drop", FALSE, nullptr);
        GstAppSinkCallbacks acb = {};
        acb.new_sample = on_new_audio;
        gst_app_sink_set_callbacks(GST_APP_SINK(asink), &acb, &ctx, nullptr);
        gst_object_unref(asink);
    }

    std::cout << "[INIT] " << cam << ": " << (ctx.input == "sdi" ? "SDI Anschluss " + std::to_string(ctx.sdi_dev) +
                 " (" + ctx.sdi_mode + ")" : std::string("Testquelle"))
              << ", All-Intra " << INTRA_KBPS / 1000 << " Mbit/s, Encoder: " << label << "\n";

    if (gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[Fehler] " << cam << ": Quelle lässt sich nicht starten"
                  << (ctx.input == "sdi"
                          ? " – DeckLink-Anschluss " + std::to_string(ctx.sdi_dev) +
                                " nicht gefunden oder belegt? (Desktop Video installiert, Anschluss in Desktop "
                                "Video Setup als Eingang gestellt, Modus " + ctx.sdi_mode + " passend?)"
                          : std::string(""))
                  << "\n";
        gst_element_set_state(ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(ctx.pipeline);
        ctx.pipeline = nullptr;
        return false;
    }
    return true;
}

// --- Aufbau der GStreamer SRT-Pipeline für einen Kanal ---
bool init_channel_pipeline(ChannelContext& ctx) {
    mkdir(STORAGE_DIR, 0777);
    std::string filepath = std::string(STORAGE_DIR) + "/cam_" + std::to_string(ctx.cam_id) + ".raw";
    
    ctx.fd = open(filepath.c_str(), O_CREAT | O_RDWR, 0666);
    if (ctx.fd < 0) {
        std::cerr << "[Fehler] Konnte Ringbuffer-Datei nicht öffnen: " << filepath << "\n";
        return false;
    }

    // Datei physisch auf der SSD vorallozieren (verhindert Fragmentierung im Live-Betrieb)
    posix_fallocate(ctx.fd, 0, MAX_FILE_SIZE_BYTES);

    // Ton-Ring: bei jedem Start leeren (ftruncate 0), damit keine Tick-Nummern aus einem früheren
    // Lauf als gültig gelesen werden (Ticks beginnen bei jedem Start wieder bei 1).
    std::string apath = std::string(STORAGE_DIR) + "/cam_" + std::to_string(ctx.cam_id) + ".aud";
    ctx.afd = open(apath.c_str(), O_CREAT | O_RDWR, 0666);
    if (ctx.afd < 0 || ftruncate(ctx.afd, 0) != 0 || posix_fallocate(ctx.afd, 0, AUDIO_FILE_BYTES) != 0) {
        std::cerr << "[WARN] REC" << (ctx.cam_id + 1) << ": Ton-Datei nicht nutzbar (" << apath << "), nur Bild\n";
        if (ctx.afd >= 0) { close(ctx.afd); ctx.afd = -1; }
    }

    if (ctx.input != "srt") return init_live_pipeline(ctx);

    ctx.pipeline = gst_pipeline_new(("srt-cam-" + std::to_string(ctx.cam_id)).c_str());
    GstElement* srtsrc    = gst_element_factory_make("srtsrc", "src");
    GstElement* tsfilter  = gst_element_factory_make("capsfilter", "tscaps");
    GstElement* demux     = gst_element_factory_make("tsdemux", "demux");
    GstElement* vqueue    = gst_element_factory_make("queue", "vqueue");
    GstElement* parse     = gst_element_factory_make("h264parse", "parse");
    GstElement* capsfilter= gst_element_factory_make("capsfilter", "caps");
    GstElement* appsink   = gst_element_factory_make("appsink", "sink");

    if (!ctx.pipeline || !srtsrc || !tsfilter || !demux || !vqueue || !parse || !capsfilter || !appsink) {
        std::cerr << "[Fehler] GStreamer-Elemente konnten nicht erstellt werden (fehlt gstreamer1.0-plugins-bad?).\n";
        return false;
    }

    // SRT-Listener: wartet auf OBS. keep-listening: Bricht OBS die Verbindung ab (Neustart des
    // Ausgangs, Netzwerk), wartet der Ingest auf die nächste Verbindung, statt den Kanal zu beenden.
    std::string uri = "srt://0.0.0.0:" + std::to_string(ctx.srt_port);
    g_object_set(G_OBJECT(srtsrc),
                 "uri", uri.c_str(),
                 "mode", 2,  // listener
                 "localaddress", "0.0.0.0",
                 "localport", static_cast<guint>(ctx.srt_port),  // im Listener-Modus maßgeblich (nicht die URI)
                 "latency", SRT_LATENCY_MS,
                 "keep-listening", TRUE,
                 nullptr);
    // srtsrc liefert Rohdaten ohne Formatangabe -> als MPEG-TS kennzeichnen
    GstCaps* tscaps = gst_caps_from_string("video/mpegts, systemstream=(boolean)true, packetsize=(int)188");
    g_object_set(G_OBJECT(tsfilter), "caps", tscaps, nullptr);
    gst_caps_unref(tscaps);

    // SPS/PPS vor jedes I-Frame setzen, damit der Player an jedem Frame sofort starten kann!
    g_object_set(G_OBJECT(parse), "config-interval", -1, nullptr);

    // Erzwinge kompletten Frame pro Buffer im Byte-Stream-Format (Annex-B)
    GstCaps* caps = gst_caps_from_string("video/x-h264, stream-format=(string)byte-stream, alignment=(string)au");
    g_object_set(G_OBJECT(capsfilter), "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Eigene Queue für das Bild: Der Demuxer hat nur EINEN Thread für Bild und Ton.
    // Ohne Queue würde ein langsames pwrite() den Ton aufhalten und umgekehrt.
    g_object_set(G_OBJECT(vqueue), "max-size-buffers", 0, "max-size-bytes", 0,
                 "max-size-time", static_cast<guint64>(2 * GST_SECOND), nullptr);

    // AppSink für minimalen Overhead konfigurieren
    // async=FALSE: Live-Pipeline, kein Preroll. Sonst wartet die Pipeline mit zwei Sinks
    // (Bild + Ton) im PAUSED-Zustand, bis BEIDE einen Puffer haben, und blockiert den Demuxer.
    g_object_set(G_OBJECT(appsink),
                 "emit-signals", FALSE,
                 "sync", FALSE,
                 "async", FALSE,
                 "max-buffers", 5,
                 "drop", TRUE,
                 nullptr);

    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, &ctx, nullptr);

    gst_bin_add_many(GST_BIN(ctx.pipeline), srtsrc, tsfilter, demux, vqueue, parse, capsfilter, appsink, nullptr);

    gst_element_link_many(srtsrc, tsfilter, demux, nullptr);
    gst_element_link_many(vqueue, parse, capsfilter, appsink, nullptr);
    ctx.video_in = vqueue;

    // Ton-Zweig: queue -> aacparse -> avdec_aac -> audioconvert -> audioresample -> S32LE/48k -> appsink
    // (Kanalzahl bleibt wie im Stream; Kanal 1/2 werden in on_new_audio herausgenommen.)
    if (ctx.afd >= 0) {
        GstElement* aqueue = gst_element_factory_make("queue", "aqueue");
        GstElement* aparse = gst_element_factory_make("aacparse", "aparse");
        GstElement* adec   = gst_element_factory_make("avdec_aac", "adec");
        GstElement* aconv  = gst_element_factory_make("audioconvert", "aconv");
        GstElement* ares   = gst_element_factory_make("audioresample", "ares");
        GstElement* acaps  = gst_element_factory_make("capsfilter", "acaps");
        GstElement* asink  = gst_element_factory_make("appsink", "asink");
        if (aqueue && aparse && adec && aconv && ares && acaps && asink) {
            g_object_set(G_OBJECT(aqueue), "max-size-buffers", 0, "max-size-bytes", 0,
                         "max-size-time", static_cast<guint64>(2 * GST_SECOND), nullptr);
            GstCaps* ac = gst_caps_from_string(
                "audio/x-raw, format=(string)S32LE, rate=(int)48000, layout=(string)interleaved");
            g_object_set(G_OBJECT(acaps), "caps", ac, nullptr);
            gst_caps_unref(ac);
            g_object_set(G_OBJECT(asink), "emit-signals", FALSE, "sync", FALSE, "async", FALSE,
                         "max-buffers", 200, "drop", FALSE, nullptr);
            GstAppSinkCallbacks acb = {};
            acb.new_sample = on_new_audio;
            gst_app_sink_set_callbacks(GST_APP_SINK(asink), &acb, &ctx, nullptr);

            gst_bin_add_many(GST_BIN(ctx.pipeline), aqueue, aparse, adec, aconv, ares, acaps, asink, nullptr);
            if (gst_element_link_many(aqueue, aparse, adec, aconv, ares, acaps, asink, nullptr)) {
                ctx.audio_in = aqueue;
            } else {
                std::cerr << "[WARN] REC" << (ctx.cam_id + 1) << ": Ton-Zweig konnte nicht verbunden werden, nur Bild\n";
            }
        } else {
            std::cerr << "[WARN] REC" << (ctx.cam_id + 1)
                      << ": Ton-Elemente fehlen (gstreamer1.0-libav installiert?), nur Bild\n";
            for (GstElement* e : {aqueue, aparse, adec, aconv, ares, acaps, asink})
                if (e) gst_object_unref(e);
        }
    }

    g_signal_connect(demux, "pad-added", G_CALLBACK(on_demux_pad_added), &ctx);

    gst_element_set_state(ctx.pipeline, GST_STATE_PLAYING);
    return true;
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);
    signal(SIGINT, signal_handler);

    // 1. Shared Memory für die Frame-Indextabelle anlegen (~43 MB in /dev/shm)
    int shm_fd = shm_open(SHM_INDEX_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0 || ftruncate(shm_fd, sizeof(SharedReplayIndex)) < 0) {
        std::cerr << "[Fehler] Konnte Shared Memory Index nicht initialisieren.\n";
        return 1;
    }

    auto* shm_index = static_cast<SharedReplayIndex*>(
        mmap(nullptr, sizeof(SharedReplayIndex), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0)
    );
    std::memset(static_cast<void*>(shm_index), 0, sizeof(SharedReplayIndex));

    // Anzahl der REC-Wege: evs-ingest 4   oder   evs-ingest 6   (Standard 6)
    // Quellen:  --input srt        alle per SRT (Standard)
    //           --input sdi        alle per SDI, REC n = DeckLink-Anschluss n-1
    //           --input sdi,sdi,srt,srt,sdi:6,test    pro REC-Weg einzeln ("sdi:K" = Anschluss K, 0-basiert)
    //           --sdi-mode 1080p50  Videomodus der SDI-Eingänge (Standard 1080p50)
    // (auch per Umgebungsvariable EVS_INPUT)
    size_t num_rec = NUM_CHANNELS;
    std::string input_spec = std::getenv("EVS_INPUT") ? std::getenv("EVS_INPUT") : "srt";
    std::string sdi_mode = "1080p50";
    for (int a = 1; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--input" && a + 1 < argc) { input_spec = argv[++a]; continue; }
        if (arg == "--sdi-mode" && a + 1 < argc) { sdi_mode = argv[++a]; continue; }
        int v = std::atoi(argv[a]);
        if (v >= 1 && v <= static_cast<int>(NUM_CHANNELS)) num_rec = static_cast<size_t>(v);
    }

    // Quellen pro REC-Weg auflösen
    std::vector<std::string> in_kind(NUM_CHANNELS, "srt");
    std::vector<int> in_dev(NUM_CHANNELS, 0);
    {
        std::vector<std::string> tok;
        size_t p = 0;
        while (true) {
            size_t q = input_spec.find(',', p);
            tok.push_back(input_spec.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (tok.size() != 1 && tok.size() != num_rec) {
            std::cerr << "[Fehler] --input braucht eine Angabe für alle oder " << num_rec
                      << " Angaben (eine pro REC), hier " << tok.size() << ".\n";
            return 1;
        }
        for (size_t i = 0; i < num_rec; ++i) {
            const std::string& t = tok.size() == 1 ? tok[0] : tok[i];
            if (t == "srt") { in_kind[i] = "srt"; }
            else if (t == "test") { in_kind[i] = "test"; }
            else if (t == "sdi") { in_kind[i] = "sdi"; in_dev[i] = static_cast<int>(i); }
            else if (t.rfind("sdi:", 0) == 0) { in_kind[i] = "sdi"; in_dev[i] = std::atoi(t.c_str() + 4); }
            else {
                std::cerr << "[Fehler] Unbekannte Quelle '" << t << "' (erlaubt: srt, sdi, sdi:N, test)\n";
                return 1;
            }
        }
    }
    shm_index->active_channels.store(static_cast<uint32_t>(num_rec));
    std::cout << "[INIT] " << num_rec << " REC-Wege\n";

    // 2. 50Hz Master-Clock starten (Tick = Systemzeit, Tageszeit-Timecode)
    g_master_tick.store(realtime_tick_now());
    shm_index->current_master_tick.store(g_master_tick.load());
    {
        char tcb[16];
        tick_to_tc(g_master_tick.load(), tcb, sizeof(tcb));
        std::cout << "[INIT] Zeitquelle: Systemuhr (Tageszeit " << tcb
                  << "). Uhr per chrony/NTP synchron halten!\n";
    }
    std::thread clock_thread(master_clock_thread_func, shm_index);

    // 3. Die SRT-Kanäle initialisieren und starten
    std::array<ChannelContext, NUM_CHANNELS> channels;
    for (size_t i = 0; i < num_rec; ++i) {
        channels[i].cam_id = static_cast<int>(i);
        channels[i].srt_port = SRT_PORTS[i];
        channels[i].shm_index = shm_index;
        channels[i].input = in_kind[i];
        channels[i].sdi_dev = in_dev[i];
        channels[i].sdi_mode = sdi_mode;

        if (!init_channel_pipeline(channels[i])) {
            g_running = false;
            break;
        }
        if (in_kind[i] == "srt")
            std::cout << "[INIT] REC " << (i + 1) << ": SRT-Listener auf Port " << SRT_PORTS[i]
                      << " (Latenz " << SRT_LATENCY_MS << " ms)\n";
    }

    // 4. Live-Status-Telemetrie in der Konsole
    std::cout << "\n--- EVS SRT INGEST AKTIV (Strg+C zum Beenden) ---\n";
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t tick = g_master_tick.load();

        // Fehler der Pipelines melden (z.B. DeckLink-Gerät nicht gefunden, Verbindung verloren)
        for (size_t i = 0; i < num_rec; ++i) {
            if (!channels[i].pipeline) continue;
            GstBus* bus = gst_element_get_bus(channels[i].pipeline);
            while (GstMessage* m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
                GError* e = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(m, &e, &dbg);
                std::cerr << "\n[FEHLER] REC" << (i + 1) << ": " << (e ? e->message : "unbekannt") << "\n";
                if (e) g_error_free(e);
                g_free(dbg);
                gst_message_unref(m);
            }
            gst_object_unref(bus);
        }

        // Master-Tick als Tageszeit-Timecode (HH:MM:SS:FF)
        char tcb[16];
        tick_to_tc(tick, tcb, sizeof(tcb));

        printf("\r[TC %s] Frames Ingested | ", tcb);
        for (size_t i = 0; i < num_rec; ++i) {
            double mb_pos = static_cast<double>(channels[i].current_write_offset) / (1024.0 * 1024.0);
            printf("REC%zu: %lu (%.0fMB, G%u, A%lu, T%lu)  ", i + 1, channels[i].frames_received, mb_pos,
                   channels[i].generation, shm_index->stats[i].reanchors.load(std::memory_order_relaxed),
                   channels[i].audio_blocks.load(std::memory_order_relaxed));
        }
        fflush(stdout);
    }

    // 5. Sauberes Herunterfahren
    std::cout << "\nStoppe Pipelines und schließe Ringbuffer...\n";
    clock_thread.join();
    for (auto& ch : channels) {
        if (ch.pipeline) {
            gst_element_set_state(ch.pipeline, GST_STATE_NULL);
            gst_object_unref(ch.pipeline);
        }
        if (ch.fd >= 0) close(ch.fd);
        if (ch.afd >= 0) close(ch.afd);
    }
    munmap(shm_index, sizeof(SharedReplayIndex));
    close(shm_fd);
    return 0;
}

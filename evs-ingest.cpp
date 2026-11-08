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
void master_clock_thread_func(SharedReplayIndex* shm_index) {
    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    while (g_running) {
        next_tick.tv_nsec += 20000000; // +20 ms (50 fps)
        if (next_tick.tv_nsec >= 1000000000) {
            next_tick.tv_sec += 1;
            next_tick.tv_nsec -= 1000000000;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, nullptr);

        uint64_t tick = ++g_master_tick;
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
    size_t num_rec = NUM_CHANNELS;
    for (int a = 1; a < argc; ++a) {
        int v = std::atoi(argv[a]);
        if (v >= 1 && v <= static_cast<int>(NUM_CHANNELS)) num_rec = static_cast<size_t>(v);
    }
    shm_index->active_channels.store(static_cast<uint32_t>(num_rec));
    std::cout << "[INIT] " << num_rec << " REC-Wege\n";

    // 2. 50Hz Master-Clock starten
    std::thread clock_thread(master_clock_thread_func, shm_index);

    // 3. Die SRT-Kanäle initialisieren und starten
    std::array<ChannelContext, NUM_CHANNELS> channels;
    for (size_t i = 0; i < num_rec; ++i) {
        channels[i].cam_id = static_cast<int>(i);
        channels[i].srt_port = SRT_PORTS[i];
        channels[i].shm_index = shm_index;

        if (!init_channel_pipeline(channels[i])) {
            g_running = false;
            break;
        }
        std::cout << "[INIT] REC " << (i + 1) << ": SRT-Listener auf Port " << SRT_PORTS[i]
                  << " (Latenz " << SRT_LATENCY_MS << " ms)\n";
    }

    // 4. Live-Status-Telemetrie in der Konsole
    std::cout << "\n--- EVS SRT INGEST AKTIV (Strg+C zum Beenden) ---\n";
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint64_t tick = g_master_tick.load();
        
        // Formatiere Master-Tick als Timecode (MM:SS:FF)
        uint64_t total_sec = tick / FPS;
        uint64_t frames = tick % FPS;
        uint64_t mins = (total_sec / 60) % 60;
        uint64_t secs = total_sec % 60;

        printf("\r[TC %02lu:%02lu:%02lu] Frames Ingested | ", mins, secs, frames);
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

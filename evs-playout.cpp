// evs-playout.cpp — PGM/PVW-Wiedergabe aus dem Ring, aus gespeicherten Clips und aus Listen.
//
// Quellen einer Engine (PGM oder PVW):
//   RING  – Live-Ring des Ingest (Playhead = Ring-Tick)
//   CLIP  – ein gespeicherter Clip (Playhead = Frame im Clip, stoppt am OUT)
//   LIST  – eine der 10 Listen: Clips laufen nacheinander mit hartem Schnitt (Highlight-Playout),
//           PVW zeigt dabei den nächsten Eintrag.
//
// Bedienung: Tastatur im Terminal oder Textbefehle per UDP an 127.0.0.1:20200 (Clip-Fenster evs-control,
// später die Controller-Bridge). Beide laufen über dieselbe Befehlsfunktion handle_command().
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <memory>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cctype>
#include <ctime>
#include <sstream>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "evs-common.h"
#include "evs-clips.h"

constexpr uint64_t MAX_FALLBACK_FRAMES = 3;
constexpr double TAKE_DEFAULT_SPEED = 1.0;  // Tempo nach TAKE, wenn PVW stand

std::atomic<bool> g_running{true};
// Ton bei Zeitlupe/Reverse: true = Varispeed (wie ein Bandgerät), false = nur bei 100 % hörbar
std::atomic<bool> g_audio_varispeed{true};

// Schützt Engine-Quellen (Modus, Clip, Liste, Marken), den Clip-Katalog und die Listen.
// Der 50-Hz-Takt hält ihn pro Takt kurz; Befehle (Tastatur/UDP) ebenso.
std::mutex g_mx;

// --- Clips ---
struct Clip {
    ClipMeta meta;
    std::atomic<uint32_t> copied{0};  // so viele Frames liegen schon (alle Kameras) im Clip-Speicher
    bool ring_fallback = false;       // in DIESER Sitzung gespeichert: noch nicht kopierte Frames aus dem Ring
    ClipFiles files;
};
using ClipPtr = std::shared_ptr<Clip>;

std::map<int, ClipPtr> g_clips;                          // Katalog (unter g_mx)
std::array<std::vector<ListEntry>, NUM_LISTS + 1> g_lists; // 1..10 (unter g_mx)
int g_next_clip_id = 1;  // nur noch für alte Clips ohne EVS-Nummer (Fallback)

// EVS-Nummerierung: Clip-Nummer = Seite*100 + Bank*10 + Position, jede Ziffer 1..9 (z. B. 112).
std::atomic<int> g_page{1};
std::atomic<int> g_bank{1};

// Kopier-Aufträge (Ring -> Clip-Speicher)
std::mutex g_copy_mx;
std::condition_variable g_copy_cv;
std::deque<ClipPtr> g_copy_queue;

// Kleiner Zwischenspeicher für Tonblöcke (pro Engine), damit pro Tick nur 1-3 pread() nötig sind
struct AudioCache {
    static constexpr int N = 4;
    int cam[N] = {-1, -1, -1, -1};
    int src[N] = {-1, -1, -1, -1};     // 0 = Ring, sonst Clip-ID
    int64_t idx[N] = {0, 0, 0, 0};
    bool ok[N] = {false, false, false, false};
    int32_t data[N][AUDIO_SAMPLES_PER_FRAME * AUDIO_CHANNELS];
    int next = 0;
};

// --- Player-Zustand für PGM und PVW ---
struct PlayoutEngine {
    std::string name;
    std::atomic<int> active_cam{0};       // Kamera 0 bis 5 (Cam 1-6)
    std::atomic<double> speed{1.0};       // 1.0 = 100%, 0.5 = 50% Slomo, 0.0 = Freeze, -1.0 = Reverse
    std::atomic<double> playhead{0};      // RING: Ring-Tick; CLIP/LIST: Frame im Clip

    // unter g_mx:
    uint32_t mode = MODE_RING;
    ClipPtr clip;                         // CLIP/LIST: aktueller Clip
    int list_no = 0;                      // LIST: 1..10
    int list_pos = 0;                     // LIST: Eintrag (0-basiert)
    uint64_t mark_in = 0, mark_out = 0;   // Quell-Ticks

    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* audiosrc = nullptr;
    bool hw_chain = false;                 // Ausgabe läuft über Quick Sync (bei Fehler -> Software)
    int out_mode = 3, out_port = 0, out_dev = 0;
    bool out_speakers = false;
    uint64_t out_tick = 0;  // zählt jeden 50-Hz-Ausgabetakt; gemeinsamer PTS für Bild und Ton
    AudioCache acache;
    std::vector<uint8_t> framebuf;
};

PlayoutEngine g_pgm, g_pvw;
std::atomic<bool> g_gang{false};
SharedReplayIndex* g_shm = nullptr;
size_t g_nch = NUM_CHANNELS;           // aktive REC-Wege (4 oder 6), vom Ingest vorgegeben
std::array<int, NUM_CHANNELS> g_fds;   // Ring-Videodateien
std::array<int, NUM_CHANNELS> g_afds;  // Ring-Tondateien

void signal_handler(int) {
    g_running = false;
}

// Zustand für Monitor und Clip-Fenster im Shared Memory
SharedPlayoutState* g_state = nullptr;

static void bump_catalog() {
    if (g_state) g_state->catalog_rev.fetch_add(1, std::memory_order_release);
}

// Länge des aktuellen Clips in Frames (0 = kein Clip)
static uint32_t clip_frames(const PlayoutEngine& e) {
    return e.clip ? e.clip->meta.frames : 0;
}

// Aktueller Frame im Clip (gerundet, begrenzt)
static uint32_t clip_frame_now(const PlayoutEngine& e) {
    uint32_t n = clip_frames(e);
    if (n == 0) return 0;
    long long f = std::llround(e.playhead.load());
    if (f < 0) f = 0;
    if (f > static_cast<long long>(n) - 1) f = n - 1;
    return static_cast<uint32_t>(f);
}

// Quell-Tick der aktuellen Position (bei Clips: der Ring-Tick, aus dem der Frame stammt)
static uint64_t source_tick(const PlayoutEngine& e) {
    if (e.mode == MODE_RING) {
        long long t = std::llround(e.playhead.load());
        return t > 0 ? static_cast<uint64_t>(t) : 0;
    }
    return e.clip ? e.clip->meta.in_tick + clip_frame_now(e) : 0;
}

static void publish_engine(PlayoutEngineState& out, const PlayoutEngine& in) {
    out.active_cam.store(in.active_cam.load(), std::memory_order_relaxed);
    out.speed.store(in.speed.load(), std::memory_order_relaxed);
    out.playhead_tick.store(static_cast<double>(source_tick(in)), std::memory_order_relaxed);
    out.mode.store(in.mode, std::memory_order_relaxed);
    out.clip_id.store(in.clip ? in.clip->meta.id : 0, std::memory_order_relaxed);
    out.clip_frame.store(static_cast<int32_t>(clip_frame_now(in)), std::memory_order_relaxed);
    out.list_no.store(in.mode == MODE_LIST ? in.list_no : 0, std::memory_order_relaxed);
    out.list_pos.store(in.list_pos, std::memory_order_relaxed);
    out.list_len.store(in.mode == MODE_LIST && in.list_no >= 1 && in.list_no <= NUM_LISTS
                           ? static_cast<int32_t>(g_lists[in.list_no].size()) : 0,
                       std::memory_order_relaxed);
    out.mark_in.store(in.mark_in, std::memory_order_relaxed);
    out.mark_out.store(in.mark_out, std::memory_order_relaxed);
}

// --- GStreamer Playout-Pipeline initialisieren ---
// mode 0 = Lokale Fenster-Ausgabe (autovideosink)
// mode 1 = SRT-Rückweg (nur PGM; Slomo ist Listener, OBS holt ab)
// mode 2 = Blackmagic DeckLink SDI Output
// mode 3 = nur Monitor (kein Videofenster)
static bool has_element(const char* f) {
    GstElementFactory* fac = gst_element_factory_find(f);
    if (fac) gst_object_unref(fac);
    return fac != nullptr;
}

// x264 ohne B-Frames, ein Referenzbild: robust für Empfänger mit Hardwaredekodierung (z.B. OBS auf dem Mac)
static std::string pick_h264_encoder_sw(int kbps) {
    return "x264enc name=venc tune=zerolatency speed-preset=superfast bitrate=" + std::to_string(kbps) +
           " key-int-max=50 bframes=0 ref=1";
}

// Setzt eine Ganzzahl-Eigenschaft nur, wenn das Element sie kennt (Namen unterscheiden sich je nach Version)
static bool set_uint_if_exists(GstElement* e, const char* prop, guint v) {
    GParamSpec* ps = g_object_class_find_property(G_OBJECT_GET_CLASS(e), prop);
    if (!ps) return false;
    if (G_PARAM_SPEC_VALUE_TYPE(ps) == G_TYPE_INT) g_object_set(e, prop, static_cast<gint>(v), nullptr);
    else g_object_set(e, prop, v, nullptr);
    return true;
}

// Rückweg-Encoder auf einen einfachen Stream einstellen: keine B-Frames, ein Referenzbild
static void simplify_hw_encoder(GstElement* pipeline) {
    GstElement* enc = gst_bin_get_by_name(GST_BIN(pipeline), "venc");
    if (!enc) return;
    // va: b-frames/ref-frames, vaapi: max-bframes/refs, x264: bframes/ref
    bool b = set_uint_if_exists(enc, "b-frames", 0) | set_uint_if_exists(enc, "max-bframes", 0) |
             set_uint_if_exists(enc, "bframes", 0);
    bool r = set_uint_if_exists(enc, "ref-frames", 1) | set_uint_if_exists(enc, "refs", 1) |
             set_uint_if_exists(enc, "ref", 1);
    std::cout << "[Playout] Rückweg-Encoder: " << (b ? "ohne B-Frames" : "B-Frames nicht einstellbar") << ", "
              << (r ? "1 Referenzbild" : "Referenzbilder nicht einstellbar") << "\n";
    gst_object_unref(enc);
}

// Encoder für den SRT-Rückweg: Hardware (Intel Quick Sync über VA-API), wenn vorhanden, sonst x264.
// Der Rückweg braucht kein All-Intra: normale GOP von 1 s.
static std::string pick_h264_encoder(int kbps) {
    auto has = has_element;
    const std::string br = std::to_string(kbps);
    if (has("vah264enc"))    return "vah264enc name=venc bitrate=" + br + " key-int-max=50";
    if (has("vaapih264enc")) return "vaapih264enc name=venc bitrate=" + br + " keyframe-period=50";
    return pick_h264_encoder_sw(kbps);
}


static void teardown_pipeline(PlayoutEngine& eng) {
    if (eng.pipeline) gst_element_set_state(eng.pipeline, GST_STATE_NULL);
    if (eng.appsrc) gst_object_unref(eng.appsrc);
    if (eng.audiosrc) gst_object_unref(eng.audiosrc);
    if (eng.pipeline) gst_object_unref(eng.pipeline);
    eng.pipeline = eng.appsrc = eng.audiosrc = nullptr;
}

// Komplette Kette für den Rückweg von H.264 (All-Intra aus dem Ring) zu H.264 (GOP) für SRT.
// Bevorzugt bleibt alles auf der Grafikeinheit: dekodieren, umwandeln, kodieren – ohne Umweg über die CPU.
static std::string srt_video_chain(bool allow_hw, bool& hw_used, std::string& label) {
    const int kbps = 25000;
    if (allow_hw && has_element("vah264dec") && has_element("vapostproc") && has_element("vah264enc")) {
        hw_used = true;
        label = "Quick Sync (va): dekodieren + kodieren auf der Grafikeinheit";
        return "h264parse ! vah264dec ! vapostproc ! video/x-raw(memory:VAMemory),format=NV12 ! "
               "vah264enc name=venc bitrate=" + std::to_string(kbps) + " key-int-max=50";
    }
    if (allow_hw && has_element("vaapih264dec") && has_element("vaapipostproc") && has_element("vaapih264enc")) {
        hw_used = true;
        label = "Quick Sync (vaapi): dekodieren + kodieren auf der Grafikeinheit";
        return "h264parse ! vaapih264dec ! vaapipostproc format=nv12 ! vaapih264enc name=venc bitrate=" +
               std::to_string(kbps) + " keyframe-period=50";
    }
    hw_used = false;
    std::string enc = allow_hw ? pick_h264_encoder(kbps) : pick_h264_encoder_sw(kbps);
    label = "Software-Dekoder, Encoder " + enc.substr(0, enc.find(' '));
    return "h264parse ! decodebin ! videoconvert ! video/x-raw,format=NV12 ! " + enc;
}

bool init_playout_pipeline(PlayoutEngine& eng, int output_mode, int srt_out_port, int decklink_dev,
                           bool audio_to_speakers, bool allow_hw = true) {
    std::string pipe_desc;
    eng.out_mode = output_mode;
    eng.out_port = srt_out_port;
    eng.out_dev = decklink_dev;
    eng.out_speakers = audio_to_speakers;
    eng.hw_chain = false;
    const std::string asrc = " appsrc name=asrc ! audioconvert ! audioresample ! ";

    // parse + decodebin wählt unter Linux Mint automatisch VA-API / NVDEC Hardware-Decoding,
    // falls verfügbar, und fällt sonst auf avdec_h264 zurück.
    if (output_mode == 0) {
        pipe_desc = "appsrc name=src ! h264parse ! decodebin ! videoconvert ! "
                    "textoverlay name=osd valignment=top halignment=left font-desc=\"Sans Bold 18\" ! "
                    "autovideosink sync=false" +
                    asrc + (audio_to_speakers ? "autoaudiosink sync=false" : "fakesink sync=false");
    } else if (output_mode == 1 && audio_to_speakers) {
        // SRT-Rückweg, nur PGM: Der Slomo-Rechner ist Listener auf srt_out_port, OBS holt den Stream als
        // Caller ab (Medienquelle srt://IP-des-Slomo:20100). Ohne Gegenstelle laufen die Daten ins Leere,
        // die Wiedergabe wird dadurch nicht aufgehalten. Ton als AAC 256 kbit/s im selben Transportstrom.
        std::string label;
        const std::string chain = srt_video_chain(allow_hw, eng.hw_chain, label);
        std::cout << "[Playout] PGM-Ausgang SRT-Listener Port " << srt_out_port << ", " << label << "\n";
        pipe_desc = "mpegtsmux name=mux alignment=7 ! srtsink mode=listener localaddress=0.0.0.0 localport=" +
                    std::to_string(srt_out_port) + " latency=200 wait-for-connection=false sync=false"
                    " appsrc name=src ! " + chain +
                    " ! h264parse config-interval=-1 ! queue ! mux." +
                    asrc + "avenc_aac bitrate=256000 ! aacparse ! queue ! mux.";
    } else if (output_mode == 1) {
        // PVW geht nicht raus (nur im Monitor zu sehen): nichts dekodieren
        pipe_desc = "appsrc name=src ! fakesink sync=false" + asrc + "fakesink sync=false";
    } else if (output_mode == 3) {
        // Nur Monitor: kein eigenes Videofenster (Bild zeigt evs-monitor), nichts wird dekodiert.
        // Der PGM-Ton geht auf die Lautsprecher, damit man beim Abspielen etwas hört.
        pipe_desc = "appsrc name=src ! fakesink sync=false" +
                    asrc + (audio_to_speakers ? "autoaudiosink sync=false" : "fakesink sync=false");
    } else {
        // Blackmagic DeckLink Hardware-Ausgang (1080p50 = Mode 1080p50)
        pipe_desc = "appsrc name=src ! h264parse ! decodebin ! videoconvert ! "
                    "decklinkvideosink device-number=" + std::to_string(decklink_dev) + " mode=1080p50 sync=false" +
                    asrc + "audio/x-raw,format=S32LE,channels=2 ! decklinkaudiosink device-number=" +
                    std::to_string(decklink_dev);  // UNGETESTET (keine Karte im Test), siehe Hinweis DeckLink-Takt
    }

    GError* err = nullptr;
    eng.pipeline = gst_parse_launch(pipe_desc.c_str(), &err);
    if (err || !eng.pipeline) {
        std::cerr << "[Fehler] Pipeline " << eng.name << ": " << (err ? err->message : "Unbekannt") << "\n";
        if (err) g_error_free(err);
        if (eng.pipeline) { gst_object_unref(eng.pipeline); eng.pipeline = nullptr; }
        if (eng.hw_chain) {  // Quick-Sync-Kette lässt sich nicht aufbauen: Software
            std::cerr << "[Warnung] " << eng.name << " läuft in Software\n";
            return init_playout_pipeline(eng, output_mode, srt_out_port, decklink_dev, audio_to_speakers, false);
        }
        return false;
    }
    if (output_mode == 1) simplify_hw_encoder(eng.pipeline);

    eng.appsrc = gst_bin_get_by_name(GST_BIN(eng.pipeline), "src");
    GstCaps* caps = gst_caps_from_string("video/x-h264, stream-format=(string)byte-stream, alignment=(string)au, framerate=50/1");
    g_object_set(G_OBJECT(eng.appsrc),
                 "caps", caps,
                 "stream-type", 0, // GST_APP_STREAM_TYPE_STREAM
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 nullptr);
    gst_caps_unref(caps);

    eng.audiosrc = gst_bin_get_by_name(GST_BIN(eng.pipeline), "asrc");
    GstCaps* acaps = gst_caps_from_string(
        "audio/x-raw, format=(string)S32LE, rate=(int)48000, channels=(int)2, layout=(string)interleaved");
    g_object_set(G_OBJECT(eng.audiosrc),
                 "caps", acaps,
                 "stream-type", 0,
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 nullptr);
    gst_caps_unref(acaps);

    if (gst_element_set_state(eng.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE && eng.hw_chain) {
        std::cerr << "[Warnung] Quick-Sync-Kette startet nicht – " << eng.name << " läuft in Software\n";
        teardown_pipeline(eng);
        return init_playout_pipeline(eng, output_mode, srt_out_port, decklink_dev, audio_to_speakers, false);
    }
    return true;
}

// Meldet die Quick-Sync-Kette im Betrieb einen Fehler (z.B. Format nicht unterstützt), auf Software umbauen.
// Aufruf aus dem 50-Hz-Takt (unter g_mx), etwa einmal pro Sekunde.
static void check_hw_errors(PlayoutEngine& eng) {
    if (!eng.pipeline || !eng.hw_chain) return;
    GstBus* bus = gst_element_get_bus(eng.pipeline);
    GstMessage* m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!m) return;
    GError* e = nullptr;
    gst_message_parse_error(m, &e, nullptr);
    std::fprintf(stderr, "\n[Warnung] %s Quick Sync: %s – schalte auf Software um\n", eng.name.c_str(),
                 e ? e->message : "Fehler");
    if (e) g_error_free(e);
    gst_message_unref(m);
    teardown_pipeline(eng);
    init_playout_pipeline(eng, eng.out_mode, eng.out_port, eng.out_dev, eng.out_speakers, false);
}

// --- Frames lesen ---

// Ring-Frame für 'tick' (bei Fehlen bis zu max_back Frames davor). Prüft Seqlock und Überschreiben.
static bool read_ring_frame(int cam, uint64_t tick, uint64_t max_back, std::vector<uint8_t>& out) {
    for (uint64_t back = 0; back <= max_back && back < tick; ++back) {
        SlotCopy s;
        if (read_slot(g_shm, cam, tick - back, s) && s.frame_size > 0 && data_intact(g_shm, cam, s)) {
            out.resize(s.frame_size);
            ssize_t n = pread(g_fds[cam], out.data(), s.frame_size, static_cast<off_t>(s.file_offset));
            // Nach dem Lesen erneut prüfen: Hat der Ingest die Region währenddessen überschrieben, verwerfen.
            return n == static_cast<ssize_t>(s.frame_size) && data_intact(g_shm, cam, s);
        }
    }
    return false;
}

// Frame i eines Clips: aus dem Clip-Speicher, oder (noch nicht kopiert) aus dem Ring
static bool read_clip_frame(const Clip& c, int cam, uint32_t i, std::vector<uint8_t>& out) {
    if (i < c.copied.load(std::memory_order_acquire)) return c.files.read_frame(cam, i, out);
    if (c.ring_fallback) return read_ring_frame(cam, c.meta.in_tick + i, 0, out);
    return false;
}

static bool read_engine_frame(PlayoutEngine& eng, std::vector<uint8_t>& out) {
    int cam = eng.active_cam.load(std::memory_order_relaxed);
    if (eng.mode == MODE_RING) {
        uint64_t t = source_tick(eng);
        return t > 0 && read_ring_frame(cam, t, MAX_FALLBACK_FRAMES, out);
    }
    if (!eng.clip) return false;
    uint32_t f = clip_frame_now(eng);
    for (uint32_t back = 0; back <= MAX_FALLBACK_FRAMES && back <= f; ++back)
        if (read_clip_frame(*eng.clip, cam, f - back, out)) return true;
    return false;
}

void feed_frame_to_engine(PlayoutEngine& eng) {
    if (!read_engine_frame(eng, eng.framebuf)) return;  // nichts Gültiges -> Bild bleibt stehen

    GstBuffer* gst_buf = gst_buffer_new_allocate(nullptr, eng.framebuf.size(), nullptr);
    gst_buffer_fill(gst_buf, 0, eng.framebuf.data(), eng.framebuf.size());
    // Synthetischer, streng monoton steigender 50Hz-PTS (Ausgabetakt), gemeinsam mit dem Ton
    GstClockTime pts = eng.out_tick * (GST_SECOND / FPS);
    GST_BUFFER_PTS(gst_buf) = pts;
    GST_BUFFER_DTS(gst_buf) = pts;
    GST_BUFFER_DURATION(gst_buf) = GST_SECOND / FPS;

    // Optionales OSD im Fenster-Modus
    GstElement* osd = gst_bin_get_by_name(GST_BIN(eng.pipeline), "osd");
    if (osd) {
        char osd_text[160];
        if (eng.mode == MODE_RING)
            std::snprintf(osd_text, sizeof(osd_text), "%s | REC %d | %.0f%%", eng.name.c_str(),
                          eng.active_cam.load() + 1, eng.speed.load() * 100.0);
        else
            std::snprintf(osd_text, sizeof(osd_text), "%s | CLIP %d | REC %d | %.0f%%", eng.name.c_str(),
                          eng.clip ? eng.clip->meta.id : 0, eng.active_cam.load() + 1, eng.speed.load() * 100.0);
        g_object_set(G_OBJECT(osd), "text", osd_text, nullptr);
        gst_object_unref(osd);
    }
    gst_app_src_push_buffer(GST_APP_SRC(eng.appsrc), gst_buf);
}

// --- Ton ---
// Tonblock 'idx' der aktuellen Quelle: RING -> idx = Ring-Tick; CLIP/LIST -> idx = Frame im Clip.
// nullptr = kein Ton (Stille).
static const int32_t* get_audio_block(PlayoutEngine& eng, int cam, int64_t idx) {
    const int src = (eng.mode == MODE_RING || !eng.clip) ? 0 : eng.clip->meta.id;
    AudioCache& c = eng.acache;
    for (int i = 0; i < AudioCache::N; ++i)
        if (c.cam[i] == cam && c.src[i] == src && c.idx[i] == idx) return c.ok[i] ? c.data[i] : nullptr;
    int i = c.next;
    c.next = (c.next + 1) % AudioCache::N;
    c.cam[i] = cam;
    c.src[i] = src;
    c.idx[i] = idx;
    bool ok = false;
    if (src == 0) {
        ok = g_afds[cam] >= 0 && idx >= 1 && read_audio_block(g_afds[cam], static_cast<uint64_t>(idx), c.data[i]);
    } else if (idx >= 0 && idx < static_cast<int64_t>(eng.clip->meta.frames)) {
        const Clip& cl = *eng.clip;
        uint32_t f = static_cast<uint32_t>(idx);
        if (f < cl.copied.load(std::memory_order_acquire)) ok = cl.files.read_audio(cam, f, c.data[i]);
        else if (cl.ring_fallback && g_afds[cam] >= 0)
            ok = read_audio_block(g_afds[cam], cl.meta.in_tick + f, c.data[i]);
    }
    c.ok[i] = ok;
    return ok ? c.data[i] : nullptr;
}

// Ein Stereo-Sample an der (ganzzahligen) Sample-Position s; Sample s gehört zu Block s / 960.
static inline void sample_at(PlayoutEngine& eng, int cam, int64_t s, double& l, double& r) {
    if (s < 0) { l = r = 0; return; }
    int64_t blk = s / AUDIO_SAMPLES_PER_FRAME;
    size_t idx = static_cast<size_t>(s % AUDIO_SAMPLES_PER_FRAME);
    const int32_t* b = get_audio_block(eng, cam, blk);
    if (!b) { l = r = 0; return; }
    l = b[idx * 2];
    r = b[idx * 2 + 1];
}

// Erzeugt die 960 Ausgabe-Samples für diesen Takt und pusht sie (bei Freeze / fehlendem Ton: Stille).
// Die Tonposition folgt dem Playhead: Block B = Samples [B*960, B*960+960). Pro Ausgabe-Sample rückt die
// Leseposition um 'speed' Samples vor -> 100 % bitgenau, Zeitlupe/Reverse als Varispeed (linear interpoliert).
void feed_audio_to_engine(PlayoutEngine& eng) {
    if (!eng.audiosrc) return;
    constexpr size_t SPF = AUDIO_SAMPLES_PER_FRAME;

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, AUDIO_PAYLOAD_BYTES, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) { gst_buffer_unref(buf); return; }
    auto* out = reinterpret_cast<int32_t*>(map.data);

    int cam = eng.active_cam.load(std::memory_order_relaxed);
    double speed = eng.speed.load(std::memory_order_relaxed);
    double ph = eng.playhead.load(std::memory_order_relaxed);
    bool normal = std::fabs(speed - 1.0) < 1e-9;
    bool mute = std::fabs(speed) < 0.01 || (!normal && !g_audio_varispeed.load(std::memory_order_relaxed));

    if (mute) {
        std::memset(out, 0, AUDIO_PAYLOAD_BYTES);
    } else {
        double start = ph * static_cast<double>(SPF);
        for (size_t i = 0; i < SPF; ++i) {
            double pos = start + static_cast<double>(i) * speed;
            double fl = std::floor(pos);
            int64_t s0 = static_cast<int64_t>(fl);
            double fr = pos - fl;
            double l0, r0;
            sample_at(eng, cam, s0, l0, r0);
            if (fr > 1e-9) {
                double l1, r1;
                sample_at(eng, cam, s0 + 1, l1, r1);
                l0 += (l1 - l0) * fr;
                r0 += (r1 - r0) * fr;
            }
            out[i * 2]     = static_cast<int32_t>(std::lround(l0));
            out[i * 2 + 1] = static_cast<int32_t>(std::lround(r0));
        }
    }
    gst_buffer_unmap(buf, &map);

    GstClockTime pts = eng.out_tick * (GST_SECOND / FPS);
    GST_BUFFER_PTS(buf) = pts;
    GST_BUFFER_DURATION(buf) = GST_SECOND / FPS;
    gst_app_src_push_buffer(GST_APP_SRC(eng.audiosrc), buf);
}

// --- Quellen wechseln (alle unter g_mx) ---

static void set_ring(PlayoutEngine& e, double tick, double speed) {
    e.mode = MODE_RING;
    e.clip.reset();
    e.list_no = 0;
    e.list_pos = 0;
    e.playhead.store(tick);
    e.speed.store(speed);
}

static void set_clip(PlayoutEngine& e, const ClipPtr& c, double speed) {
    e.mode = MODE_CLIP;
    e.clip = c;
    e.list_no = 0;
    e.list_pos = 0;
    e.active_cam.store(c->meta.cam);
    e.playhead.store(0.0);
    e.speed.store(speed);
}

static ClipPtr find_clip(int id) {
    auto it = g_clips.find(id);
    return it == g_clips.end() ? nullptr : it->second;
}

// Ersten gültigen Listeneintrag ab 'pos' laden. Einträge, deren Clip gelöscht ist, werden übersprungen.
static bool load_list_item(PlayoutEngine& e, int list, int pos, bool running) {
    if (list < 1 || list > NUM_LISTS) return false;
    const auto& l = g_lists[list];
    for (int p = std::max(0, pos); p < static_cast<int>(l.size()); ++p) {
        ClipPtr c = find_clip(l[p].clip_id);
        if (!c) continue;
        e.mode = MODE_LIST;
        e.clip = c;
        e.list_no = list;
        e.list_pos = p;
        e.active_cam.store(c->meta.cam);
        e.playhead.store(0.0);
        e.speed.store(running ? l[p].speed : 0.0);
        return true;
    }
    return false;
}

// Läuft PGM eine Liste ab, zeigt PVW den nächsten Eintrag (an seinem Anfang, stehend)
static void cue_list_preview() {
    if (g_pgm.mode != MODE_LIST) return;
    const auto& l = g_lists[g_pgm.list_no];
    for (int p = g_pgm.list_pos + 1; p < static_cast<int>(l.size()); ++p) {
        ClipPtr c = find_clip(l[p].clip_id);
        if (!c) continue;
        set_clip(g_pvw, c, 0.0);
        return;
    }
}

// Tageszeit eines Ring-Ticks (für die Anzeige "TC IN" der Clips)
static std::string wallclock_tc_of_tick(uint64_t tick, uint64_t /*live*/) {
    char b[32];
    tick_to_tc(tick, b, sizeof(b));  // der Tick IST die Tageszeit
    return b;
}

// --- Clip speichern: Dateien anlegen, Katalog eintragen, Kopierauftrag ---
static std::string save_clip(PlayoutEngine& src, const std::string& name) {
    uint64_t in = src.mark_in, out = src.mark_out;
    uint64_t pos = source_tick(src);
    if (in == 0) return "FEHLER: kein IN gesetzt";
    if (out == 0 || out <= in) {
        if (pos > in) out = pos;  // OUT fehlt: aktuelle Position als OUT
        else return "FEHLER: kein gültiges OUT (OUT muss nach IN liegen)";
    }
    uint64_t live = g_shm->current_master_tick.load();
    if (live + TOTAL_SLOTS < out || in + TOTAL_SLOTS < live + 50)
        return "FEHLER: IN liegt nicht mehr im Ring";

    // Nächste freie Position (1..9) auf der aktuellen Seite und Bank
    int new_id = 0;
    for (int pos = 1; pos <= 9; ++pos) {
        int cand = g_page.load() * 100 + g_bank.load() * 10 + pos;
        if (!g_clips.count(cand)) { new_id = cand; break; }
    }
    if (new_id == 0) {
        char fb[96];
        std::snprintf(fb, sizeof(fb), "FEHLER: Seite %d Bank %d ist voll – andere Bank/Seite wählen", g_page.load(),
                      g_bank.load());
        return fb;
    }
    auto c = std::make_shared<Clip>();
    c->meta.id = new_id;
    c->meta.name = name.empty() ? ("Clip " + std::to_string(c->meta.id)) : name;
    c->meta.in_tick = in;
    c->meta.out_tick = out;
    c->meta.frames = static_cast<uint32_t>(out - in + 1);
    c->meta.cam = src.active_cam.load();
    c->meta.tc_in = wallclock_tc_of_tick(in, live);
    c->meta.created = time(nullptr);
    c->ring_fallback = true;

    ensure_clip_dirs();
    mkdir(clip_dir(c->meta.id).c_str(), 0775);
    for (int cam = 0; cam < static_cast<int>(g_nch); ++cam)
        for (const char* ext : {".h264", ".idx", ".pcm"}) {
            int fd = ::open(clip_file(c->meta.id, cam, ext).c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);
            if (fd < 0) return "FEHLER: Clip-Dateien nicht anlegbar (" + clip_dir(c->meta.id) + ")";
            ::close(fd);
        }
    if (!save_clip_meta(c->meta)) return "FEHLER: clip.ini nicht schreibbar";
    c->files.open(c->meta.id);

    g_clips[c->meta.id] = c;
    src.mark_in = src.mark_out = 0;
    bump_catalog();
    {
        std::lock_guard<std::mutex> lk(g_copy_mx);
        g_copy_queue.push_back(c);
    }
    g_copy_cv.notify_one();
    char b[128];
    std::snprintf(b, sizeof(b), "OK Clip %d gespeichert (%.1f s, %s) REC %d", c->meta.id,
                  c->meta.frames / double(FPS), c->meta.tc_in.c_str(), c->meta.cam + 1);
    return b;
}

// Kopier-Thread: holt die Frames aller 6 Kameras (Bild + Ton) aus dem Ring in den Clip-Speicher.
// Läuft ohne g_mx; die Wiedergabe liest bis dahin aus dem Ring.
static void copy_thread() {
    std::vector<uint8_t> buf;
    std::vector<uint8_t> abuf(AUDIO_PAYLOAD_BYTES);
    while (g_running) {
        ClipPtr c;
        {
            std::unique_lock<std::mutex> lk(g_copy_mx);
            g_copy_cv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_copy_queue.empty() || !g_running; });
            if (g_copy_queue.empty()) continue;
            c = g_copy_queue.front();
            g_copy_queue.pop_front();
        }
        const int id = c->meta.id;
        int vfd[NUM_CHANNELS], ifd[NUM_CHANNELS], pfd[NUM_CHANNELS];
        uint64_t voff[NUM_CHANNELS] = {};
        for (int cam = 0; cam < static_cast<int>(NUM_CHANNELS); ++cam) vfd[cam] = ifd[cam] = pfd[cam] = -1;
        for (int cam = 0; cam < static_cast<int>(g_nch); ++cam) {
            vfd[cam] = ::open(clip_file(id, cam, ".h264").c_str(), O_WRONLY | O_APPEND);
            ifd[cam] = ::open(clip_file(id, cam, ".idx").c_str(), O_WRONLY | O_APPEND);
            pfd[cam] = ::open(clip_file(id, cam, ".pcm").c_str(), O_WRONLY | O_APPEND);
        }
        bool aborted = false;
        for (uint32_t i = 0; i < c->meta.frames && g_running && !aborted; ++i) {
            const uint64_t tick = c->meta.in_tick + i;
            // Liegt der Frame noch in der Zukunft (OUT nahe Live)? Dann warten, bis er geschrieben ist.
            while (g_running && g_shm->current_master_tick.load() < tick + 3) {
                if (g_shm->current_master_tick.load() + 1000 < c->meta.in_tick) { aborted = true; break; }  // Ingest neu gestartet
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (aborted || !g_running) break;
            for (int cam = 0; cam < static_cast<int>(g_nch); ++cam) {
                ClipIdxEntry e{voff[cam], 0, 0};
                if (vfd[cam] >= 0 && read_ring_frame(cam, tick, 0, buf) &&
                    write(vfd[cam], buf.data(), buf.size()) == static_cast<ssize_t>(buf.size())) {
                    e.size = static_cast<uint32_t>(buf.size());
                    voff[cam] += buf.size();
                }
                bool aok = g_afds[cam] >= 0 && read_audio_block(g_afds[cam], tick, abuf.data());
                if (!aok) std::memset(abuf.data(), 0, AUDIO_PAYLOAD_BYTES);
                if (pfd[cam] >= 0 && write(pfd[cam], abuf.data(), AUDIO_PAYLOAD_BYTES) == static_cast<ssize_t>(AUDIO_PAYLOAD_BYTES) && aok)
                    e.flags |= 1u;
                if (ifd[cam] >= 0 && write(ifd[cam], &e, sizeof(e)) != sizeof(e)) aborted = true;
            }
            c->copied.store(i + 1, std::memory_order_release);
        }
        for (int cam = 0; cam < static_cast<int>(NUM_CHANNELS); ++cam)
            for (int fd : {vfd[cam], ifd[cam], pfd[cam]}) if (fd >= 0) ::close(fd);
        if (aborted) std::fprintf(stderr, "\n[Clip %d] Kopieren abgebrochen nach %u Frames\n", id, c->copied.load());
        {
            std::lock_guard<std::mutex> lk(g_mx);
            bump_catalog();
        }
    }
}

// --- Befehle (Tastatur und UDP) ---

static PlayoutEngine* engine_by_name(const std::string& s) {
    if (s == "PGM") return &g_pgm;
    if (s == "PVW") return &g_pvw;
    return nullptr;
}

static std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

static bool list_ok(int l) { return l >= 1 && l <= NUM_LISTS; }

static std::string save_list_and_bump(int l) {
    if (!save_list(l, g_lists[l])) return "FEHLER: Liste nicht schreibbar";
    bump_catalog();
    return "OK";
}

// Führt einen Befehl aus (nimmt g_mx). Rückgabe: "OK ..." oder "FEHLER: ..."
static std::string handle_command_impl(const std::string& line);

// Führt einen Befehl aus. Schlägt er fehl, geht eine kurze Meldung an alle Anzeigen (Clip-Fenster).
std::string handle_command(const std::string& line) {
    std::string r = handle_command_impl(line);
    if (g_state && r.rfind("FEHLER", 0) == 0) {
        std::string t = r.size() > 8 ? r.substr(8) : r;
        if (r.find("voll") != std::string::npos) {
            char b[64];
            std::snprintf(b, sizeof(b), "BANK VOLL  (Seite %d, Bank %d)", g_page.load(), g_bank.load());
            t = b;
        }
        std::strncpy(g_state->alert_text, t.c_str(), sizeof(g_state->alert_text) - 1);
        g_state->alert_text[sizeof(g_state->alert_text) - 1] = 0;
        g_state->alert_seq.fetch_add(1);
    }
    return r;
}

static std::string handle_command_impl(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    cmd = upper(cmd);
    if (cmd.empty()) return "";

    std::lock_guard<std::mutex> lk(g_mx);
    const uint64_t live = g_shm->current_master_tick.load();

    auto engine_arg = [&](PlayoutEngine* def) -> PlayoutEngine* {
        std::string w;
        std::streampos p = in.tellg();
        if (in >> w) {
            if (PlayoutEngine* e = engine_by_name(upper(w))) return e;
            in.clear();
            in.seekg(p);
        }
        return def;
    };

    if (cmd == "CAM") {
        PlayoutEngine* e = engine_arg(&g_pvw);
        int n = 0;
        in >> n;
        if (n < 1 || n > static_cast<int>(g_nch)) return "FEHLER: REC 1-" + std::to_string(g_nch);
        e->active_cam.store(n - 1);
        return "OK";
    }
    if (cmd == "SPEED") {
        PlayoutEngine* e = engine_arg(&g_pgm);
        double v = 0;
        if (!(in >> v) || v < -4.0 || v > 4.0) return "FEHLER: Tempo -4 .. 4";
        e->speed.store(v);
        return "OK";
    }
    if (cmd == "PLAY") {
        PlayoutEngine* e = engine_arg(&g_pgm);
        double v = 1.0;
        if (e->mode == MODE_LIST && list_ok(e->list_no) && e->list_pos < static_cast<int>(g_lists[e->list_no].size()))
            v = g_lists[e->list_no][e->list_pos].speed;
        e->speed.store(v);
        return "OK";
    }
    if (cmd == "STOP") {
        engine_arg(&g_pgm)->speed.store(0.0);
        return "OK";
    }
    if (cmd == "STEP") {  // bildweise (Jog): STEP [PGM|PVW] <Frames>
        PlayoutEngine* e = engine_arg(&g_pvw);
        int n = 0;
        in >> n;
        e->speed.store(0.0);
        double p = e->playhead.load() + n;
        if (e->mode == MODE_RING) {
            double lo = live > TOTAL_SLOTS - 50 ? double(live - TOTAL_SLOTS + 50) : 1.0;
            p = std::clamp(p, lo, double(live > 2 ? live - 2 : 1));
        } else {
            p = std::clamp(p, 0.0, double(clip_frames(*e) > 0 ? clip_frames(*e) - 1 : 0));
        }
        e->playhead.store(std::round(p));
        return "OK";
    }
    if (cmd == "TAKE") {
        if (g_pgm.mode == MODE_LIST) {  // Liste läuft: TAKE springt sofort zum nächsten Eintrag
            if (!load_list_item(g_pgm, g_pgm.list_no, g_pgm.list_pos + 1, true)) return "FEHLER: Listenende";
            cue_list_preview();
            return "OK";
        }
        std::swap(g_pgm.mode, g_pvw.mode);
        std::swap(g_pgm.clip, g_pvw.clip);
        std::swap(g_pgm.list_no, g_pvw.list_no);
        std::swap(g_pgm.list_pos, g_pvw.list_pos);
        int cam = g_pgm.active_cam.load();
        g_pgm.active_cam.store(g_pvw.active_cam.load());
        g_pvw.active_cam.store(cam);
        double ph = g_pgm.playhead.load();
        g_pgm.playhead.store(g_pvw.playhead.load());
        g_pvw.playhead.store(ph);
        double pvw_speed = g_pvw.speed.load();
        g_pvw.speed.store(0.0);
        g_pgm.speed.store(std::fabs(pvw_speed) > 0.001 ? pvw_speed : TAKE_DEFAULT_SPEED);
        if (g_pgm.mode == MODE_LIST) cue_list_preview();
        return "OK";
    }
    if (cmd == "GANG") {
        g_gang.store(!g_gang.load());
        return g_gang.load() ? "OK GANG an" : "OK GANG aus";
    }
    if (cmd == "LIVE") {  // LIVE [PGM|PVW]: zurück an die Live-Kante, 100 %
        PlayoutEngine* e = engine_arg(&g_pgm);
        if (live > 5) set_ring(*e, double(live - 3), 1.0);
        return "OK";
    }
    if (cmd == "JUMP") {  // JUMP [PVW] <Sekunden>: Live minus Sekunden, stehend (Cue)
        PlayoutEngine* e = engine_arg(&g_pvw);
        double sec = 5;
        in >> sec;
        uint64_t back = static_cast<uint64_t>(std::max(0.0, sec) * FPS);
        set_ring(*e, double(live > back + 1 ? live - back : 1), 0.0);
        return "OK";
    }
    if (cmd == "MARKIN" || cmd == "MARKOUT") {
        PlayoutEngine* e = engine_arg(&g_pvw);
        uint64_t t = source_tick(*e);
        if (cmd == "MARKIN") e->mark_in = t; else e->mark_out = t;
        return "OK";
    }
    if (cmd == "CLEARMARKS") {
        PlayoutEngine* e = engine_arg(&g_pvw);
        e->mark_in = e->mark_out = 0;
        return "OK";
    }
    if (cmd == "GOTOIN" || cmd == "GOTOOUT") {
        PlayoutEngine* e = engine_arg(&g_pvw);
        uint64_t t = cmd == "GOTOIN" ? e->mark_in : e->mark_out;
        if (t == 0) return "FEHLER: Marke nicht gesetzt";
        if (e->mode == MODE_RING) {
            e->playhead.store(double(t));
        } else if (e->clip && t >= e->clip->meta.in_tick && t <= e->clip->meta.out_tick) {
            e->playhead.store(double(t - e->clip->meta.in_tick));
        } else {
            set_ring(*e, double(t), 0.0);
        }
        e->speed.store(0.0);
        return "OK";
    }
    if (cmd == "SAVECLIP") {  // SAVECLIP [PGM|PVW] [Name …]
        PlayoutEngine* e = engine_arg(&g_pvw);
        std::string name;
        std::getline(in, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        return save_clip(*e, name);
    }
    if (cmd == "RECALL") {  // RECALL <ID> [PGM|PVW]: Clip am Anfang laden, stehend
        int id = 0;
        in >> id;
        PlayoutEngine* e = engine_arg(&g_pvw);
        ClipPtr c = find_clip(id);
        if (!c) return "FEHLER: Clip " + std::to_string(id) + " gibt es nicht";
        set_clip(*e, c, 0.0);
        // optional: Kamerawinkel – SAVED = Winkel beim Speichern, 1..6 = REC
        std::string w;
        if (in >> w) {
            if (upper(w) == "SAVED") e->active_cam.store(std::clamp(c->meta.cam, 0, int(g_nch) - 1));
            else {
                int n = std::atoi(w.c_str());
                if (n >= 1 && n <= static_cast<int>(g_nch)) e->active_cam.store(n - 1);
            }
        }
        return "OK";
    }
    if (cmd == "PAGE" || cmd == "BANK") {  // PAGE <1..9> | + | -
        std::atomic<int>& v = (cmd == "PAGE") ? g_page : g_bank;
        std::string w;
        in >> w;
        int n = v.load();
        if (w == "+") n = n % 9 + 1;
        else if (w == "-") n = (n + 7) % 9 + 1;
        else if (std::atoi(w.c_str()) >= 1 && std::atoi(w.c_str()) <= 9) n = std::atoi(w.c_str());
        else return "FEHLER: 1..9, + oder -";
        v.store(n);
        char b[64];
        std::snprintf(b, sizeof(b), "OK Seite %d Bank %d", g_page.load(), g_bank.load());
        return b;
    }
    if (cmd == "STATUS") {  // Zustand fürs Controller-Display (eine Zeile, key=value)
        auto describe = [&](PlayoutEngine& e, const char* p, std::string& out) {
            const char* m = e.mode == MODE_CLIP ? "CLIP" : e.mode == MODE_LIST ? "LIST" : "RING";
            char b[160];
            std::snprintf(b, sizeof(b), " %scam=%d %sspeed=%d %smode=%s %sclip=%d", p, e.active_cam.load() + 1, p,
                          static_cast<int>(std::lround(e.speed.load() * 100.0)), p, m, p, e.clip ? e.clip->meta.id : 0);
            out += b;
        };
        std::string r = "STATUS";
        char b[96];
        int free_pos = 0;
        for (int pos = 1; pos <= 9 && !free_pos; ++pos)
            if (!g_clips.count(g_page.load() * 100 + g_bank.load() * 10 + pos)) free_pos = pos;
        std::snprintf(b, sizeof(b), " page=%d bank=%d free=%d", g_page.load(), g_bank.load(), free_pos);
        r += b;
        describe(g_pvw, "pvw", r);
        describe(g_pgm, "pgm", r);
        int ln = g_pvw.list_no;
        std::snprintf(b, sizeof(b), " pvwlist=%d pgmlist=%d", ln, g_pgm.list_no);
        r += b;
        std::snprintf(b, sizeof(b), " marks=%d%d", g_pvw.mark_in != 0, g_pvw.mark_out != 0);
        r += b;
        return r;
    }
    if (cmd == "RENAME") {  // RENAME <ID> <Name …>
        int id = 0;
        in >> id;
        std::string name;
        std::getline(in, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        ClipPtr c = find_clip(id);
        if (!c || name.empty()) return "FEHLER: RENAME <ID> <Name>";
        c->meta.name = clean_name(name);
        save_clip_meta(c->meta);
        bump_catalog();
        return "OK";
    }
    if (cmd == "DELCLIP") {  // Clip löschen (und aus allen Listen entfernen)
        int id = 0;
        in >> id;
        ClipPtr c = find_clip(id);
        if (!c) return "FEHLER: Clip gibt es nicht";
        if (g_pgm.clip == c) return "FEHLER: Clip läuft gerade auf PGM";
        if (c->copied.load() < c->meta.frames && c->ring_fallback) return "FEHLER: Clip wird noch kopiert";
        if (g_pvw.clip == c) set_ring(g_pvw, double(live > 250 ? live - 250 : 1), 0.0);
        g_clips.erase(id);
        for (int l = 1; l <= NUM_LISTS; ++l) {
            auto& v = g_lists[l];
            size_t before = v.size();
            v.erase(std::remove_if(v.begin(), v.end(), [&](const ListEntry& e) { return e.clip_id == id; }), v.end());
            if (v.size() != before) save_list(l, v);
        }
        c->files.close();
        delete_clip_files(id);
        bump_catalog();
        return "OK";
    }
    if (cmd == "LISTADD") {  // LISTADD <Liste> <Clip-ID> [Position 1..] [Tempo]
        int l = 0, id = 0, pos = 0;
        double sp = 1.0;
        in >> l >> id;
        if (!(in >> pos)) pos = 0;
        if (!(in >> sp)) sp = 1.0;
        if (!list_ok(l) || !find_clip(id)) return "FEHLER: LISTADD <Liste 1-10> <Clip-ID>";
        auto& v = g_lists[l];
        size_t at = (pos >= 1 && pos <= static_cast<int>(v.size())) ? size_t(pos - 1) : v.size();
        v.insert(v.begin() + static_cast<long>(at), ListEntry{id, sp});
        return save_list_and_bump(l);
    }
    if (cmd == "LISTDEL") {  // LISTDEL <Liste> <Position>
        int l = 0, pos = 0;
        in >> l >> pos;
        if (!list_ok(l) || pos < 1 || pos > static_cast<int>(g_lists[l].size())) return "FEHLER: Position";
        if (g_pgm.mode == MODE_LIST && g_pgm.list_no == l) return "FEHLER: Liste läuft gerade auf PGM";
        g_lists[l].erase(g_lists[l].begin() + (pos - 1));
        return save_list_and_bump(l);
    }
    if (cmd == "LISTMOVE") {  // LISTMOVE <Liste> <von> <nach>  (Reihenfolge ändern)
        int l = 0, a = 0, b = 0;
        in >> l >> a >> b;
        if (!list_ok(l)) return "FEHLER: Liste";
        auto& v = g_lists[l];
        int n = static_cast<int>(v.size());
        if (a < 1 || a > n || b < 1 || b > n) return "FEHLER: Position";
        if (g_pgm.mode == MODE_LIST && g_pgm.list_no == l) return "FEHLER: Liste läuft gerade auf PGM";
        ListEntry e = v[a - 1];
        v.erase(v.begin() + (a - 1));
        v.insert(v.begin() + (b - 1), e);
        return save_list_and_bump(l);
    }
    if (cmd == "LISTXFER") {  // LISTXFER <Liste> <Position> <Zielliste> [MOVE|COPY]
        int l = 0, pos = 0, t = 0;
        std::string how = "MOVE";
        in >> l >> pos >> t;
        in >> how;
        how = upper(how);
        if (!list_ok(l) || !list_ok(t) || pos < 1 || pos > static_cast<int>(g_lists[l].size()))
            return "FEHLER: LISTXFER <Liste> <Position> <Zielliste> [MOVE|COPY]";
        if (how == "MOVE" && g_pgm.mode == MODE_LIST && g_pgm.list_no == l) return "FEHLER: Liste läuft gerade auf PGM";
        ListEntry e = g_lists[l][pos - 1];
        g_lists[t].push_back(e);
        if (how == "MOVE" && t != l) g_lists[l].erase(g_lists[l].begin() + (pos - 1));
        save_list(l, g_lists[l]);
        return save_list_and_bump(t);
    }
    if (cmd == "LISTSPEED") {  // LISTSPEED <Liste> <Position> <Tempo>
        int l = 0, pos = 0;
        double sp = 1.0;
        in >> l >> pos >> sp;
        if (!list_ok(l) || pos < 1 || pos > static_cast<int>(g_lists[l].size()) || sp < 0.0 || sp > 4.0)
            return "FEHLER: LISTSPEED <Liste> <Position> <Tempo 0..4>";
        g_lists[l][pos - 1].speed = sp;
        return save_list_and_bump(l);
    }
    if (cmd == "LISTCLEAR") {
        int l = 0;
        in >> l;
        if (!list_ok(l)) return "FEHLER: Liste";
        if (g_pgm.mode == MODE_LIST && g_pgm.list_no == l) return "FEHLER: Liste läuft gerade auf PGM";
        g_lists[l].clear();
        return save_list_and_bump(l);
    }
    if (cmd == "LISTCUE") {  // LISTCUE <Liste> [Position]: Liste auf PGM bereitstellen (stehend), PLAY startet
        int l = 0, pos = 1;
        in >> l;
        if (!(in >> pos)) pos = 1;
        if (!list_ok(l)) return "FEHLER: Liste";
        if (!load_list_item(g_pgm, l, pos - 1, false)) return "FEHLER: Liste ist leer";
        cue_list_preview();
        return "OK";
    }
    if (cmd == "AUDIO") {
        g_audio_varispeed.store(!g_audio_varispeed.load());
        if (g_state) g_state->audio_varispeed.store(g_audio_varispeed.load() ? 1 : 0);
        return g_audio_varispeed.load() ? "OK Ton Varispeed" : "OK Ton nur bei 100 %";
    }
    return "FEHLER: unbekannter Befehl '" + cmd + "'";
}

// UDP-Befehlseingang (nur 127.0.0.1). Antwort geht an den Absender zurück.
static void control_thread() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(CONTROL_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::fprintf(stderr, "[Warnung] Befehlsport %d belegt – Clip-Fenster/Controller nicht verfügbar\n", CONTROL_PORT);
        ::close(s);
        return;
    }
    char buf[2048];
    while (g_running) {
        pollfd pf{s, POLLIN, 0};
        if (poll(&pf, 1, 200) <= 0) continue;
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        std::istringstream lines(buf);
        std::string line, reply;
        while (std::getline(lines, line)) {
            if (line.empty()) continue;
            reply = handle_command(line);
        }
        if (!reply.empty()) sendto(s, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), fl);
    }
    ::close(s);
}

// --- 50Hz Playout-Taktgeber für PGM & PVW ---
void playout_loop_thread() {
    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    while (g_running) {
        next_tick.tv_nsec += 20000000; // 20 ms = 50 Hz
        if (next_tick.tv_nsec >= 1000000000) {
            next_tick.tv_sec += 1;
            next_tick.tv_nsec -= 1000000000;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, nullptr);

        uint64_t live_head = g_shm->current_master_tick.load(std::memory_order_acquire);
        if (live_head < 10) {  // Warten bis Ingest angelaufen ist
            if (g_state) g_state->heartbeat.fetch_add(1, std::memory_order_release);
            continue;
        }

        std::lock_guard<std::mutex> lk(g_mx);
        uint64_t min_valid_tick = (live_head > TOTAL_SLOTS - 50) ? (live_head - TOTAL_SLOTS + 50) : 1;
        uint64_t max_valid_tick = live_head - 2; // 2 Frames Sicherheitsabstand zum Schreibkopf

        auto update_playhead = [&](PlayoutEngine& eng) {
            const double sp = eng.speed.load();
            double next_pos = eng.playhead.load() + sp;
            if (eng.mode == MODE_RING) {
                if (next_pos > static_cast<double>(max_valid_tick)) {
                    next_pos = static_cast<double>(max_valid_tick);
                    if (sp > 1.0) eng.speed.store(1.0); // Am Live-Punkt auf 100% bremsen
                }
                if (next_pos < static_cast<double>(min_valid_tick)) {
                    next_pos = static_cast<double>(min_valid_tick);
                    eng.speed.store(0.0); // Am Puffer-Anfang auf Freeze gehen
                }
                eng.playhead.store(next_pos);
                return;
            }
            const double last = clip_frames(eng) > 0 ? double(clip_frames(eng) - 1) : 0.0;
            if (next_pos > last + 0.5) {
                // Clip zu Ende: in der Liste nahtlos (harter Schnitt) zum nächsten Eintrag, sonst am OUT stehen
                if (eng.mode == MODE_LIST && sp > 0 && load_list_item(eng, eng.list_no, eng.list_pos + 1, true)) {
                    if (&eng == &g_pgm) cue_list_preview();
                    return;
                }
                next_pos = last;
                eng.speed.store(0.0);
            }
            if (next_pos < 0.0) {
                next_pos = 0.0;
                eng.speed.store(0.0);
            }
            eng.playhead.store(next_pos);
        };

        update_playhead(g_pgm);

        if (g_gang.load() && g_pgm.mode == MODE_RING && g_pvw.mode == MODE_RING) {
            // Im EVS Gang-Modus laufen PGM und PVW exakt synchron auf demselben Frame
            g_pvw.playhead.store(g_pgm.playhead.load());
            g_pvw.speed.store(g_pgm.speed.load());
        } else {
            update_playhead(g_pvw);
        }

        for (PlayoutEngine* eng : {&g_pgm, &g_pvw}) {
            if (eng->out_tick % FPS == 0) check_hw_errors(*eng);
            if (!eng->pipeline) continue;
            feed_frame_to_engine(*eng);
            feed_audio_to_engine(*eng);
            eng->out_tick++;
        }

        if (g_state) {
            publish_engine(g_state->pgm, g_pgm);
            publish_engine(g_state->pvw, g_pvw);
            g_state->gang_mode.store(g_gang.load() ? 1 : 0, std::memory_order_relaxed);
            g_state->audio_varispeed.store(g_audio_varispeed.load() ? 1 : 0, std::memory_order_relaxed);
            // Heartbeat ERST nach dem Veröffentlichen: Der Monitor holt sein PGM/PVW-Bild genau dann,
            // wenn der Heartbeat weiterzählt, und sieht so immer den gerade ausgegebenen Frame.
            g_state->heartbeat.fetch_add(1, std::memory_order_release);
        }
    }
}

// --- Hilfsfunktion für verzögerungsfreie Tastatureingabe im Terminal ---
void set_raw_terminal(bool enable) {
    static struct termios oldt, newt;
    if (enable) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    }
}

// Tastatur -> Befehl
static std::string key_to_command(char c) {
    switch (c) {
        case '1': case '2': case '3': case '4': case '5': case '6':
            return std::string("CAM PVW ") + c;
        case '!': return "CAM PGM 1";
        case '"': case '@': return "CAM PGM 2";
        case '#': return "CAM PGM 3";
        case '$': return "CAM PGM 4";
        case '%': return "CAM PGM 5";
        case '&': case '^': return "CAM PGM 6";
        case ' ': return "TAKE";
        case 'g': return "GANG";
        case 'j': return "SPEED PVW -1";
        case 'k': return "SPEED PVW 0";
        case 'l': return "SPEED PVW 1";
        case ',': return "STEP PVW -1";
        case '.': return "STEP PVW 1";
        case 's': return "SPEED PGM 0.25";
        case 'd': return "SPEED PGM 0.5";
        case 'f': return "SPEED PGM 0.75";
        case 'p': return "SPEED PGM 1";
        case 'x': return "STOP PGM";
        case 'b': return "JUMP PVW 5";
        case 'i': return "MARKIN PVW";
        case 'u': return "MARKOUT PVW";
        case 'o': return "GOTOIN PVW";
        case 'c': return "SAVECLIP PVW";
        case 'e': return "LIVE PGM";
        case 'a': return "AUDIO";
        default: return "";
    }
}

static const char* mode_name(const PlayoutEngine& e) {
    return e.mode == MODE_RING ? "Ring" : e.mode == MODE_CLIP ? "Clip" : "Liste";
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);
    signal(SIGINT, signal_handler);

    // 1. Shared Memory vom Ingest-Prozess öffnen
    int shm_fd = shm_open(SHM_INDEX_NAME, O_RDONLY, 0666);
    if (shm_fd < 0) {
        std::cerr << "[Fehler] Frame-Index (" << SHM_INDEX_NAME << ") nicht gefunden. Läuft evs-ingest?\n";
        return 1;
    }
    void* ip = mmap(nullptr, sizeof(SharedReplayIndex), PROT_READ, MAP_SHARED, shm_fd, 0);
    if (ip == MAP_FAILED) {
        std::cerr << "[Fehler] Frame-Index nicht lesbar (Version passt nicht? alle Programme neu bauen)\n";
        return 1;
    }
    g_shm = static_cast<SharedReplayIndex*>(ip);
    {
        uint32_t n = g_shm->active_channels.load();
        g_nch = (n >= 1 && n <= NUM_CHANNELS) ? n : NUM_CHANNELS;
        std::cout << "[Playout] " << g_nch << " REC-Wege\n";
    }

    // 1b. Zustands-Bereich für Monitor und Clip-Fenster anlegen (wird bei Neustart wiederverwendet)
    int state_fd = shm_open(SHM_STATE_NAME, O_CREAT | O_RDWR, 0666);
    if (state_fd >= 0 && ftruncate(state_fd, sizeof(SharedPlayoutState)) == 0) {
        void* p = mmap(nullptr, sizeof(SharedPlayoutState), PROT_READ | PROT_WRITE, MAP_SHARED, state_fd, 0);
        if (p != MAP_FAILED) {
            std::memset(p, 0, sizeof(SharedPlayoutState));
            g_state = static_cast<SharedPlayoutState*>(p);
            g_state->audio_varispeed.store(1);
        }
    }
    if (!g_state) std::cerr << "[Warnung] Zustands-Bereich fuer den Monitor nicht verfuegbar.\n";

    // 2. Die Ringbuffer-Dateien der aktiven Wege im Read-Only-Modus öffnen
    g_fds.fill(-1);
    g_afds.fill(-1);
    for (size_t i = 0; i < g_nch; ++i) {
        std::string path = std::string(STORAGE_DIR) + "/cam_" + std::to_string(i) + ".raw";
        g_fds[i] = open(path.c_str(), O_RDONLY);
        if (g_fds[i] < 0) {
            std::cerr << "[Fehler] Konnte Datei nicht öffnen: " << path << "\n";
            return 1;
        }
    }
    // 2b. Ton-Dateien (optional: fehlt eine, ist der Ton dieser Kamera stumm)
    for (size_t i = 0; i < g_nch; ++i) {
        std::string path = std::string(STORAGE_DIR) + "/cam_" + std::to_string(i) + ".aud";
        g_afds[i] = open(path.c_str(), O_RDONLY);
        if (g_afds[i] < 0) std::cerr << "[Warnung] Kein Ton für REC " << (i + 1) << " (" << path << ")\n";
    }

    // 2c. Gespeicherte Clips und Listen laden
    ensure_clip_dirs();
    for (const ClipMeta& m : load_all_clips()) {
        auto c = std::make_shared<Clip>();
        c->meta = m;
        c->files.open(m.id);
        uint32_t avail = m.frames;
        for (int cam = 0; cam < static_cast<int>(NUM_CHANNELS); ++cam)
            if (c->files.has_cam(cam)) avail = std::min(avail, c->files.frames_available(cam));
        c->copied.store(avail);
        c->ring_fallback = false;  // aus früherer Sitzung: Ring-Ticks passen nicht mehr
        g_clips[m.id] = c;
        g_next_clip_id = std::max(g_next_clip_id, m.id + 1);
    }
    for (int l = 1; l <= NUM_LISTS; ++l) g_lists[l] = load_list(l);
    std::cout << "[Clips] " << g_clips.size() << " Clips geladen, nächste Nummer " << g_next_clip_id << "\n";

    // 3. PGM und PVW Engines initialisieren (Startet 5 Sekunden hinter Live)
    g_pgm.name = "PGM";
    g_pvw.name = "PVW";
    uint64_t start_tick = (g_shm->current_master_tick.load() > 250) ? (g_shm->current_master_tick.load() - 250) : 1;
    g_pgm.playhead.store(static_cast<double>(start_tick));
    g_pvw.playhead.store(static_cast<double>(start_tick));
    g_pvw.active_cam.store(1); // PVW startet auf Kamera 2

    // Output-Mode: 0 = Fenster, 1 = SRT-Rückweg (nur PGM, Listener auf Port 20100 oder Angabe),
    //              2 = DeckLink SDI, 3 = nur Monitor (keine Videofenster, PGM-Ton auf Lautsprecher)
    int output_mode = (argc > 1) ? std::atoi(argv[1]) : 3;
    int srt_port = (argc > 2) ? std::atoi(argv[2]) : 20100;
    if (output_mode < 0 || output_mode > 3 || srt_port <= 0 || srt_port > 65535) {
        std::cerr << "Aufruf: evs-playout [0|1|2|3] [SRT-Port]\n"
                     "  0=Fenster  1=SRT-Rückweg PGM (Listener, Standard-Port 20100)  2=DeckLink  3=nur Monitor (Standard)\n";
        return 1;
    }
    if (!init_playout_pipeline(g_pgm, output_mode, srt_port, 0, true) ||
        !init_playout_pipeline(g_pvw, output_mode, srt_port + 10, 1, false)) {
        return 1;
    }

    std::thread loop_thread(playout_loop_thread);
    std::thread copier(copy_thread);
    std::thread control(control_thread);

    // 4. Interaktive Live-Steuerung (Tastatur zum Testen; Clips/Listen bequemer im Fenster evs-control)
    std::cout << "\n================ EVS PLAYOUT CONTROLLER ================\n"
              << " [1-6]        : REC auf PVW wählen (1-6)\n"
              << " [Shift+1..6] : REC direkt auf PGM schalten\n"
              << " [SPACE]      : TAKE (PVW -> PGM; läuft eine Liste: nächster Eintrag)\n"
              << " [g]          : GANG-MODE an/aus (PGM & PVW Timecode koppeln)\n"
              << " [j / k / l]  : PVW Rückwärts (-100%) / Stand (0%) / Play (100%)\n"
              << " [, / .]      : PVW einen Frame zurück / vor\n"
              << " [s/d/f/p/x]  : PGM Tempo 25% / 50% / 75% / 100% / Stopp\n"
              << " [b]          : PVW 5 Sekunden hinter Live (Cue)\n"
              << " [i / u / o]  : PVW IN setzen / OUT setzen / zum IN springen\n"
              << " [c]          : Clip aus IN/OUT speichern (alle REC-Wege)\n"
              << " [e]          : PGM zurück zu LIVE (E-to-E)\n"
              << " [a]          : Ton bei Zeitlupe: Varispeed / nur bei 100 % (umschalten)\n"
              << " [q]          : Beenden\n"
              << " Befehle auch per UDP an 127.0.0.1:" << CONTROL_PORT << " (Clip-Fenster evs-control)\n"
              << "========================================================\n";

    const bool tty = isatty(STDIN_FILENO);
    if (tty) set_raw_terminal(true);
    std::string last_reply;
    while (g_running) {
        pollfd pf{STDIN_FILENO, POLLIN, 0};
        if (poll(&pf, 1, 500) <= 0) continue;
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            if (!tty) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
            continue;
        }
        if (c == 'q') { g_running = false; break; }
        std::string cmd = key_to_command(c);
        if (cmd.empty()) continue;
        last_reply = handle_command(cmd);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            std::printf("\r[STATUS] %s | GANG %s | PGM: %s Cam%d %.0f%% | PVW: %s Cam%d %.0f%% IN:%s OUT:%s | %s          ",
                        g_audio_varispeed.load() ? "Varispeed" : "nur 100%", g_gang.load() ? "an " : "aus",
                        mode_name(g_pgm), g_pgm.active_cam.load() + 1, g_pgm.speed.load() * 100.0,
                        mode_name(g_pvw), g_pvw.active_cam.load() + 1, g_pvw.speed.load() * 100.0,
                        g_pvw.mark_in ? "ja" : "-", g_pvw.mark_out ? "ja" : "-", last_reply.c_str());
        }
        std::fflush(stdout);
    }
    if (tty) set_raw_terminal(false);

    std::cout << "\nBeende Playout-Engine...\n";
    g_copy_cv.notify_all();
    loop_thread.join();
    copier.join();
    control.join();
    for (PlayoutEngine* e : {&g_pgm, &g_pvw}) {
        teardown_pipeline(*e);
        e->clip.reset();
    }
    g_clips.clear();
    for (int fd : g_fds) if (fd >= 0) close(fd);
    for (int fd : g_afds) if (fd >= 0) close(fd);
    munmap(g_shm, sizeof(SharedReplayIndex));
    close(shm_fd);
    if (g_state) munmap(g_state, sizeof(SharedPlayoutState));
    if (state_fd >= 0) close(state_fd);
    return 0;
}

// evs-common.h — gemeinsame Konstanten und Shared-Memory-Strukturen für
// evs-ingest (schreibt), evs-playout (liest, schreibt Zustand) und evs-monitor (liest nur).
// Wird eine Struktur hier geändert, müssen ALLE drei Programme neu gebaut werden.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

// --- System-Konstanten ---
constexpr size_t NUM_CHANNELS = 6;
constexpr size_t FPS = 50;
constexpr size_t BUFFER_SECONDS = 3600;               // 1 Stunde
constexpr size_t TOTAL_SLOTS = FPS * BUFFER_SECONDS;  // 180.000 Frames pro Kanal (Index ~43 MB gesamt)

// Dimensionierung des Datei-Rings pro Kanal:
// Die Datei muss BUFFER_SECONDS bei der MAXIMALEN Stream-Bitrate fassen, sonst wird Material
// überschrieben, das der Index noch als gültig führt. Hier: 120 Mbit/s + 25 % Reserve (~67,5 GB
// pro Kanal, ~405 GB gesamt bei einer Stunde). Wert an die tatsächliche OBS-Encoder-Bitrate anpassen!
constexpr uint64_t ASSUMED_MAX_MBIT_PER_S = 120;
constexpr uint64_t MAX_FILE_SIZE_BYTES =
    (BUFFER_SECONDS * ASSUMED_MAX_MBIT_PER_S * 125000ULL) / 4 * 5;

// write_pos[cam] = (Generation << GEN_SHIFT) | Schreib-Offset. Die Generation zählt die Datei-Umläufe.
constexpr unsigned GEN_SHIFT = 40;
constexpr uint64_t OFFSET_MASK = (1ULL << GEN_SHIFT) - 1;
constexpr uint32_t GEN_MASK = 0xFFFFFF;
static_assert(MAX_FILE_SIZE_BYTES < (1ULL << GEN_SHIFT), "Datei zu groß für 40-Bit-Offset");

// Sicherheitsabstand zur Schreibposition (größer als jeder einzelne Frame)
constexpr uint64_t OVERWRITE_MARGIN_BYTES = 2ULL * 1024 * 1024;

constexpr const char* SHM_INDEX_NAME = "/evs_replay_index";   // Frame-Index, geschrieben vom Ingest
constexpr const char* SHM_STATE_NAME = "/evs_playout_state";  // Zustand PGM/PVW, geschrieben vom Playout
constexpr const char* STORAGE_DIR = "./replay_storage";

// --- Ton (Stereo, aus dem AAC der RIST-Streams dekodiert) ---
// Pro Kamera eine eigene Ring-Datei cam_N.aud mit FESTEN Blöcken: ein Block pro Master-Tick,
// Position = (Tick % TOTAL_SLOTS) * AUDIO_SLOT_BYTES. Blockaufbau:
//   [uint64 Tick][960 Samples x 2 Kanäle, int32 little-endian, interleaved]
// Der Tick im Header ist der Gültigkeits-Marker (0 / anderer Tick = kein Ton für diesen Tick).
// Größe: ~1,4 GB pro Kamera bei einer Stunde.
constexpr uint32_t AUDIO_RATE = 48000;
constexpr uint32_t AUDIO_CHANNELS = 2;
constexpr uint32_t AUDIO_SAMPLES_PER_FRAME = AUDIO_RATE / FPS;  // 960
constexpr size_t AUDIO_PAYLOAD_BYTES = AUDIO_SAMPLES_PER_FRAME * AUDIO_CHANNELS * sizeof(int32_t);  // 7680
constexpr size_t AUDIO_SLOT_BYTES = sizeof(uint64_t) + AUDIO_PAYLOAD_BYTES;
constexpr uint64_t AUDIO_FILE_BYTES = static_cast<uint64_t>(TOTAL_SLOTS) * AUDIO_SLOT_BYTES;

// --- Echtzeit als Zeitquelle ---
// Ein Tick ist ein 20-ms-Schritt der Systemuhr (CLOCK_REALTIME): tick = Sekunden seit 1970 * 50 + Frame.
// Dadurch ist der Timecode überall die Tageszeit (HH:MM:SS:FF, lokale Zeit) und über Neustarts hinweg
// eindeutig. Die Systemuhr sollte per chrony/NTP/PTP diszipliniert sein (nur Gleiten, kein Springen).
inline uint64_t realtime_tick_now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * FPS + static_cast<uint64_t>(ts.tv_nsec) / (1000000000ULL / FPS);
}

// Tick -> "HH:MM:SS:FF" (lokale Zeit)
inline void tick_to_tc(uint64_t tick, char* buf, size_t n) {
    time_t sec = static_cast<time_t>(tick / FPS);
    struct tm lt;
    localtime_r(&sec, &lt);
    std::snprintf(buf, n, "%02d:%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec, static_cast<int>(tick % FPS));
}

// --- Frame-Index (Ingest -> Playout/Monitor) ---
// Seqlock-Prinzip: master_tick ist der Commit-Marker.
//   Writer: master_tick=0 -> Felder schreiben -> master_tick=Tick (release)
//   Reader: master_tick lesen -> Felder lesen -> master_tick erneut lesen, bei Abweichung verwerfen.
// master_tick == 0 bedeutet "leer / wird gerade beschrieben" (Ticks beginnen bei 1).
struct FrameSlot {
    std::atomic<uint64_t> master_tick;  // Globaler 50Hz Synchron-Zeitstempel (Commit-Marker)
    std::atomic<uint64_t> pts_ns;       // Originaler Stream-PTS
    std::atomic<uint64_t> file_offset;  // Exakte Byte-Position in cam_X.raw
    std::atomic<uint32_t> frame_size;   // Größe des H.264 Frames in Bytes
    std::atomic<uint32_t> generation;   // Datei-Umlauf, in dem der Frame geschrieben wurde
    std::atomic<uint32_t> is_keyframe;  // 1 bei I-Frame (All-Intra = immer 1)
};

// Zähler pro Kanal, nur für Anzeige/Monitoring
struct ChannelStats {
    std::atomic<uint64_t> frames_received;  // seit Start des Ingest
    std::atomic<uint64_t> reanchors;        // wie oft der PTS-Anker nachgezogen wurde
};

struct SharedReplayIndex {
    std::atomic<uint64_t> current_master_tick;
    std::atomic<uint32_t> active_channels;  // 4 oder 6: so viele REC-Wege hat der Ingest gestartet (0 = noch nicht gesetzt)
    std::atomic<uint64_t> write_pos[NUM_CHANNELS];  // (Generation << 40) | Offset des zuletzt begonnenen Writes
    int32_t camera_latency_offset[NUM_CHANNELS];
    ChannelStats stats[NUM_CHANNELS];
    FrameSlot slots[NUM_CHANNELS][TOTAL_SLOTS];
};

// --- Zustand des Playout (Playout -> Monitor) ---
static_assert(std::atomic<double>::is_always_lock_free, "atomic<double> muss lock-free sein");

// Quelle einer Engine
enum EngineMode : uint32_t { MODE_RING = 0, MODE_CLIP = 1, MODE_LIST = 2 };

struct PlayoutEngineState {
    std::atomic<int32_t> active_cam;     // 0..5
    std::atomic<double> speed;           // 1.0 = 100%
    std::atomic<double> playhead_tick;   // Quell-Tick (auch bei Clips: Ring-Tick, aus dem der Frame stammt)
    std::atomic<uint32_t> mode;          // EngineMode
    std::atomic<int32_t> clip_id;        // 0 = kein Clip (Ring)
    std::atomic<int32_t> clip_frame;     // Frame-Nummer im Clip (0 .. Länge-1)
    std::atomic<int32_t> list_no;        // 1..10 im Listenmodus, sonst 0
    std::atomic<int32_t> list_pos;       // Eintrag in der Liste (0-basiert)
    std::atomic<int32_t> list_len;
    std::atomic<uint64_t> mark_in;       // gesetzte IN/OUT-Marken (Quell-Ticks, 0 = nicht gesetzt)
    std::atomic<uint64_t> mark_out;
};

struct SharedPlayoutState {
    std::atomic<uint64_t> heartbeat;     // zählt bei jedem 50-Hz-Zyklus hoch; steht er, läuft das Playout nicht
    std::atomic<uint32_t> gang_mode;
    PlayoutEngineState pgm;
    PlayoutEngineState pvw;
    std::atomic<uint64_t> catalog_rev;   // zählt hoch, wenn sich Clips oder Listen ändern (Fenster lädt neu)
    std::atomic<uint32_t> audio_varispeed;
    std::atomic<uint32_t> alert_seq;     // zählt hoch, wenn ein Befehl fehlschlägt (Clip-Fenster zeigt dann ein rotes Banner)
    char alert_text[64];                 // kurzer Text dazu (UTF-8, 0-terminiert)
};

// --- Clips und Listen ---
constexpr const char* CLIP_DIR = "./clips";   // ein Unterordner pro Clip (clip_0001, ...)
constexpr const char* LIST_DIR = "./lists";   // list_01.txt ... list_10.txt
constexpr int NUM_LISTS = 10;
constexpr int MAX_CLIP_ID = 9999;
// Steuerbefehle an das Playout (Textzeilen per UDP, nur lokal): vom Clip-Fenster und der Controller-Bridge
constexpr int CONTROL_PORT = 20200;

// --- Lesezugriff auf Slots (Playout und Monitor) ---

// Konsistente Kopie eines Slots
struct SlotCopy {
    uint64_t tick = 0;
    uint64_t pts_ns = 0;
    uint64_t file_offset = 0;
    uint32_t frame_size = 0;
    uint32_t generation = 0;
    bool is_keyframe = false;
};

// Liest den Slot für want_tick. Liefert false, wenn der Slot leer ist, einen anderen Tick enthält
// (z.B. Frame-Drop oder bereits überschrieben) oder während des Lesens vom Ingest beschrieben wurde.
inline bool read_slot(const SharedReplayIndex* shm, int cam, uint64_t want_tick, SlotCopy& out) {
    const FrameSlot& s = shm->slots[cam][want_tick % TOTAL_SLOTS];

    uint64_t t1 = s.master_tick.load(std::memory_order_acquire);
    if (t1 != want_tick) return false;  // 0 (leer/in Arbeit) oder anderer Tick

    out.pts_ns      = s.pts_ns.load(std::memory_order_relaxed);
    out.file_offset = s.file_offset.load(std::memory_order_relaxed);
    out.frame_size  = s.frame_size.load(std::memory_order_relaxed);
    out.generation  = s.generation.load(std::memory_order_relaxed);
    out.is_keyframe = s.is_keyframe.load(std::memory_order_relaxed) != 0;

    std::atomic_thread_fence(std::memory_order_acquire);
    uint64_t t2 = s.master_tick.load(std::memory_order_relaxed);
    if (t1 != t2) return false;  // Ingest hat den Slot währenddessen umgeschrieben

    out.tick = t1;
    return true;
}

// Prüft, ob die Frame-Daten in der Datei noch NICHT vom Ring überschrieben wurden.
//  - gleiche Generation: Daten liegen vor der Schreibposition -> intakt
//  - Vorgänger-Generation: intakt, solange der Frame hinter der Schreibposition (+Sicherheitsabstand) liegt
//  - älter: überschrieben
inline bool data_intact(const SharedReplayIndex* shm, int cam, const SlotCopy& s) {
    uint64_t wp = shm->write_pos[cam].load();  // seq_cst, Gegenstück zum Ingest
    uint32_t cur_gen = static_cast<uint32_t>(wp >> GEN_SHIFT);
    uint64_t cur_off = wp & OFFSET_MASK;

    uint32_t diff = (cur_gen - s.generation) & GEN_MASK;
    if (diff == 0) return true;
    if (diff == 1) return s.file_offset >= cur_off + OVERWRITE_MARGIN_BYTES;
    return false;
}

// Liest den Tonblock für want_tick aus der .aud-Datei (fd). out muss AUDIO_PAYLOAD_BYTES groß sein.
// Liefert false, wenn für diesen Tick kein Ton vorliegt (leer, Lücke oder bereits überschrieben).
inline bool read_audio_block(int fd, uint64_t want_tick, void* out) {
    uint8_t buf[AUDIO_SLOT_BYTES];
    off_t off = static_cast<off_t>((want_tick % TOTAL_SLOTS) * AUDIO_SLOT_BYTES);
    if (pread(fd, buf, AUDIO_SLOT_BYTES, off) != static_cast<ssize_t>(AUDIO_SLOT_BYTES)) return false;
    uint64_t t;
    std::memcpy(&t, buf, sizeof(t));
    if (t != want_tick) return false;
    std::memcpy(out, buf + sizeof(t), AUDIO_PAYLOAD_BYTES);
    return true;
}

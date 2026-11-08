// evs-clips.h — Clip-Speicher und Listen (gemeinsam für evs-playout, evs-monitor, evs-control)
//
// Clip-Speicher (dauerhaft, unabhängig vom 1-Stunden-Ring):
//   clips/clip_0007/clip.ini     Metadaten (Textdatei, key=value)
//   clips/clip_0007/cam_N.h264   H.264-Frames der Kamera N hintereinander
//   clips/clip_0007/cam_N.idx    pro Frame 16 Byte: Offset, Größe, Flags (Bit 0 = Ton vorhanden)
//   clips/clip_0007/cam_N.pcm    pro Frame ein Tonblock (960 Samples Stereo int32) — fest, auch bei Stille
// Ein Clip enthält ALLE 6 Kamerawinkel derselben Zeitspanne (wie bei EVS). Frame i entspricht dem
// Ring-Tick in_tick + i. Die .idx-Datei wächst beim Kopieren Frame für Frame; ihre Größe zeigt, wie
// viele Frames schon kopiert sind (der Rest kommt bis dahin noch aus dem Ring).
//
// Listen:  lists/list_01.txt … list_10.txt, eine Zeile pro Eintrag:  <Clip-ID> <Tempo>
#pragma once

#include "evs-common.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct ClipIdxEntry {
    uint64_t offset;
    uint32_t size;    // 0 = Frame fehlte im Ring (Wiedergabe zeigt den vorherigen)
    uint32_t flags;   // Bit 0: Ton vorhanden
};
static_assert(sizeof(ClipIdxEntry) == 16, "ClipIdxEntry muss 16 Byte groß sein");

struct ClipMeta {
    int id = 0;
    std::string name;
    uint64_t in_tick = 0;    // erster Frame (Ring-Tick)
    uint64_t out_tick = 0;   // letzter Frame (einschließlich)
    uint32_t frames = 0;     // out - in + 1
    int cam = 0;             // Hauptkamera beim Speichern (0..5)
    std::string tc_in;       // Tageszeit des IN-Punkts "HH:MM:SS:FF"
    int64_t created = 0;     // Unix-Zeit des Speicherns
};

struct ListEntry {
    int clip_id = 0;
    double speed = 1.0;      // Tempo dieses Eintrags beim Abspielen der Liste
};

inline void ensure_clip_dirs() {
    mkdir(CLIP_DIR, 0775);
    mkdir(LIST_DIR, 0775);
}

inline std::string clip_dir(int id) {
    char b[96];
    std::snprintf(b, sizeof(b), "%s/clip_%04d", CLIP_DIR, id);
    return b;
}

inline std::string clip_file(int id, int cam, const char* ext) {
    return clip_dir(id) + "/cam_" + std::to_string(cam) + ext;
}

inline std::string list_path(int n) {
    char b[96];
    std::snprintf(b, sizeof(b), "%s/list_%02d.txt", LIST_DIR, n);
    return b;
}

// Schreibt erst in eine Hilfsdatei und benennt dann um: Leser sehen nie eine halbe Datei.
inline bool write_file_atomic(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline std::string clean_name(std::string n) {
    for (char& c : n) if (c == '\n' || c == '\r') c = ' ';
    return n;
}

inline bool save_clip_meta(const ClipMeta& m) {
    std::string t;
    t += "id=" + std::to_string(m.id) + "\n";
    t += "name=" + clean_name(m.name) + "\n";
    t += "in_tick=" + std::to_string(m.in_tick) + "\n";
    t += "out_tick=" + std::to_string(m.out_tick) + "\n";
    t += "frames=" + std::to_string(m.frames) + "\n";
    t += "cam=" + std::to_string(m.cam) + "\n";
    t += "tc_in=" + m.tc_in + "\n";
    t += "created=" + std::to_string(m.created) + "\n";
    return write_file_atomic(clip_dir(m.id) + "/clip.ini", t);
}

inline bool load_clip_meta(int id, ClipMeta& m) {
    FILE* f = std::fopen((clip_dir(id) + "/clip.ini").c_str(), "r");
    if (!f) return false;
    m = ClipMeta{};
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == "id") m.id = std::atoi(v.c_str());
        else if (k == "name") m.name = v;
        else if (k == "in_tick") m.in_tick = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "out_tick") m.out_tick = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "frames") m.frames = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "cam") m.cam = std::atoi(v.c_str());
        else if (k == "tc_in") m.tc_in = v;
        else if (k == "created") m.created = std::strtoll(v.c_str(), nullptr, 10);
    }
    std::fclose(f);
    return m.id == id && m.frames > 0;
}

// Alle Clips im Speicher, nach ID sortiert
inline std::vector<ClipMeta> load_all_clips() {
    std::vector<ClipMeta> out;
    DIR* d = opendir(CLIP_DIR);
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        int id = 0;
        if (std::sscanf(e->d_name, "clip_%d", &id) == 1 && id > 0) {
            ClipMeta m;
            if (load_clip_meta(id, m)) out.push_back(m);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const ClipMeta& a, const ClipMeta& b) { return a.id < b.id; });
    return out;
}

inline void delete_clip_files(int id) {
    for (int c = 0; c < static_cast<int>(NUM_CHANNELS); ++c)
        for (const char* ext : {".h264", ".idx", ".pcm"}) unlink(clip_file(id, c, ext).c_str());
    unlink((clip_dir(id) + "/clip.ini").c_str());
    rmdir(clip_dir(id).c_str());
}

inline std::vector<ListEntry> load_list(int n) {
    std::vector<ListEntry> out;
    FILE* f = std::fopen(list_path(n).c_str(), "r");
    if (!f) return out;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        ListEntry e;
        if (line[0] == '#') continue;
        if (std::sscanf(line, "%d %lf", &e.clip_id, &e.speed) >= 1 && e.clip_id > 0) out.push_back(e);
    }
    std::fclose(f);
    return out;
}

inline bool save_list(int n, const std::vector<ListEntry>& l) {
    std::string t = "# Liste " + std::to_string(n) + ": <Clip-ID> <Tempo>\n";
    char b[64];
    for (const auto& e : l) {
        std::snprintf(b, sizeof(b), "%d %.3f\n", e.clip_id, e.speed);
        t += b;
    }
    return write_file_atomic(list_path(n), t);
}

// Lesezugriff auf die Mediendateien eines Clips (Playout und Monitor)
class ClipFiles {
public:
    ClipFiles() = default;
    ClipFiles(const ClipFiles&) = delete;
    ClipFiles& operator=(const ClipFiles&) = delete;
    ~ClipFiles() { close(); }

    bool open(int id) {
        close();
        id_ = id;
        bool any = false;
        for (int c = 0; c < static_cast<int>(NUM_CHANNELS); ++c) {
            h264_[c] = ::open(clip_file(id, c, ".h264").c_str(), O_RDONLY);
            idx_[c] = ::open(clip_file(id, c, ".idx").c_str(), O_RDONLY);
            pcm_[c] = ::open(clip_file(id, c, ".pcm").c_str(), O_RDONLY);
            any = any || (h264_[c] >= 0 && idx_[c] >= 0);
        }
        return any;
    }

    void close() {
        for (int c = 0; c < static_cast<int>(NUM_CHANNELS); ++c)
            for (int* fd : {&h264_[c], &idx_[c], &pcm_[c]})
                if (*fd >= 0) { ::close(*fd); *fd = -1; }
        id_ = 0;
    }

    int id() const { return id_; }
    // Gibt es für diese Kamera Dateien im Clip? (Clips aus einem 4-Wege-Betrieb haben nur REC 1-4)
    bool has_cam(int cam) const { return cam >= 0 && cam < static_cast<int>(NUM_CHANNELS) && idx_[cam] >= 0 && h264_[cam] >= 0; }

    // Wie viele Frames dieser Kamera sind schon kopiert?
    uint32_t frames_available(int cam) const {
        if (cam < 0 || cam >= static_cast<int>(NUM_CHANNELS) || idx_[cam] < 0) return 0;
        struct stat st;
        if (fstat(idx_[cam], &st) != 0) return 0;
        return static_cast<uint32_t>(st.st_size / static_cast<off_t>(sizeof(ClipIdxEntry)));
    }

    bool entry(int cam, uint32_t i, ClipIdxEntry& e) const {
        if (cam < 0 || cam >= static_cast<int>(NUM_CHANNELS) || idx_[cam] < 0) return false;
        return pread(idx_[cam], &e, sizeof(e), static_cast<off_t>(i) * sizeof(e)) == sizeof(e);
    }

    // Frame i lesen; false, wenn er (noch) nicht kopiert ist oder im Ring gefehlt hatte
    bool read_frame(int cam, uint32_t i, std::vector<uint8_t>& out) const {
        ClipIdxEntry e;
        if (!entry(cam, i, e) || e.size == 0 || h264_[cam] < 0) return false;
        out.resize(e.size);
        return pread(h264_[cam], out.data(), e.size, static_cast<off_t>(e.offset)) == static_cast<ssize_t>(e.size);
    }

    // Tonblock i lesen (AUDIO_PAYLOAD_BYTES); false, wenn kein Ton vorhanden
    bool read_audio(int cam, uint32_t i, void* out) const {
        ClipIdxEntry e;
        if (!entry(cam, i, e) || !(e.flags & 1u) || pcm_[cam] < 0) return false;
        return pread(pcm_[cam], out, AUDIO_PAYLOAD_BYTES, static_cast<off_t>(i) * AUDIO_PAYLOAD_BYTES) ==
               static_cast<ssize_t>(AUDIO_PAYLOAD_BYTES);
    }

private:
    int id_ = 0;
    int h264_[NUM_CHANNELS] = {-1, -1, -1, -1, -1, -1};
    int idx_[NUM_CHANNELS] = {-1, -1, -1, -1, -1, -1};
    int pcm_[NUM_CHANNELS] = {-1, -1, -1, -1, -1, -1};
};

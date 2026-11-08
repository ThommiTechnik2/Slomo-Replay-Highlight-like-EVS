// evs-monitor.cpp — Qt6-Monitor für das EVS-Replay-System (nur lesend)
//
// Festes Raster:   oben     PGM         |  PVW          (groß)
//                  unten    REC1  REC2  REC3
//                           REC4  REC5  REC6
//
// Der Monitor liest den Frame-Index (/dev/shm/evs_replay_index), den Zustand des Playouts
// (/dev/shm/evs_playout_state) und die Ring-Dateien. Er schreibt nirgends und beeinflusst
// Ingest und Playout nicht. Läuft er nicht oder stürzt er ab, merkt die Sendung nichts.
//
// Vorschaubilder: jedes Frame ist ein I-Frame, es wird einzeln dekodiert (appsrc -> avdec_h264 ->
// verkleinert -> appsink) und als QImage in die Kachel gemalt.

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QStringList>
#include <QElapsedTimer>
#include <QFont>
#include <QResizeEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMetaObject>
#include <QPainter>
#include <QString>
#include <QTimer>
#include <QFontMetrics>
#include <QWidget>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evs-common.h"
#include "evs-clips.h"
#include <memory>

// --- Einstellungen (fest) ---
// Größe der dekodierten Vorschau (Breite*3 Bytes pro Zeile muss durch 4 teilbar sein -> kein Zeilen-Padding)
constexpr int CAM_W = 640;                  // Kameras (klein, 3x2 unten)
constexpr int CAM_H = 360;
constexpr int BIG_W = 960;                  // PGM / PVW (groß, oben)
constexpr int BIG_H = 540;
// Der Monitor fragt alle 4 ms nach, ob Ingest (Master-Tick) oder Playout (Heartbeat) einen Takt
// weitergeschaltet haben, und holt neue Bilder genau dann. So läuft die Anzeige im Takt der Quelle
// statt in einem eigenen, leicht abweichenden Takt (das erzeugte doppelte/ausgelassene Bilder).
constexpr int TIMER_MS = 4;
// Bildraten (über Kommandozeile änderbar: --cam-fps / --big-fps; erlaubt 50, 25, 10, 5)
// Jede Kachel dekodiert in einem eigenen GStreamer-Thread; die Last verteilt sich auf alle Kerne.
static QStringList g_names;                 // Kamera-Namen (--names "Mitte;16er Hoch;...")
static int g_cam_fps = 25;                  // Kameras
static int g_big_fps = 50;                  // PGM / PVW
constexpr uint64_t LIVE_MARGIN_TICKS = 3;   // Kameravorschau zeigt Live-Kante minus 3 Frames
constexpr uint64_t GAP_WINDOW_TICKS = 60 * FPS;  // Lücken der letzten 60 s
constexpr uint32_t MAX_SANE_FRAME_BYTES = 32u * 1024 * 1024;

// ---------------------------------------------------------------------------------------------
// Dekodierung der Vorschaubilder: Hardware (Intel Quick Sync über VA-API) dekodiert UND verkleinert,
// die CPU bekommt nur noch das kleine Bild. Ohne passende Hardware: Software (avdec_h264).
// Erzwingen von Software:  evs-monitor --no-hw
// ---------------------------------------------------------------------------------------------
enum class DecodePath { Software, VA, VAAPI };
static DecodePath g_decode = DecodePath::Software;

static bool has_element(const char* name) {
    GstElementFactory* f = gst_element_factory_find(name);
    if (f) gst_object_unref(f);
    return f != nullptr;
}

static void choose_decode_path(bool allow_hw) {
    if (allow_hw && has_element("vah264dec") && has_element("vapostproc")) g_decode = DecodePath::VA;
    else if (allow_hw && has_element("vaapih264dec") && has_element("vaapipostproc")) g_decode = DecodePath::VAAPI;
    else g_decode = DecodePath::Software;
}

static const char* decode_name(DecodePath p) {
    return p == DecodePath::VA ? "Quick Sync (va)" : p == DecodePath::VAAPI ? "Quick Sync (vaapi)" : "Software";
}

// Decoder-Kette von h264parse bis zum RGB-Bild in der gewünschten Größe
static std::string decode_chain(DecodePath p, int w, int h) {
    const std::string size = "width=" + std::to_string(w) + ",height=" + std::to_string(h);
    switch (p) {
        case DecodePath::VA:
            return "h264parse ! vah264dec ! vapostproc ! video/x-raw," + size +
                   " ! videoconvert ! video/x-raw,format=RGB," + size;
        case DecodePath::VAAPI:
            return "h264parse ! vaapih264dec ! vaapipostproc width=" + std::to_string(w) + " height=" +
                   std::to_string(h) + " ! videoconvert ! video/x-raw,format=RGB," + size;
        default:
            return "h264parse ! avdec_h264 max-threads=1 ! videoscale ! videoconvert ! video/x-raw,format=RGB," + size;
    }
}

// ---------------------------------------------------------------------------------------------
// Kachel: zeigt ein Vorschaubild, Titel oben, Statuszeile unten, farbiger Rahmen (Tally)
// ---------------------------------------------------------------------------------------------
class Tile : public QWidget {
public:
    Tile(int decodeW, int decodeH, QWidget* parent = nullptr) : QWidget(parent), w_(decodeW), h_(decodeH) {
        setMinimumSize(240, 135);
        setAutoFillBackground(false);

        build(g_decode);
    }

    ~Tile() override { teardown(); }

    // Einen H.264-Frame zum Dekodieren übergeben (GUI-Thread)
    void pushFrame(const uint8_t* data, size_t size) {
        if (!appsrc_) return;
        checkErrors();
        if (!appsrc_) return;
        GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
        gst_buffer_fill(buf, 0, data, size);
        GstClockTime pts = pts_counter_++ * (GST_SECOND / FPS);
        GST_BUFFER_PTS(buf) = pts;
        GST_BUFFER_DTS(buf) = pts;
        GST_BUFFER_DURATION(buf) = GST_SECOND / FPS;
        gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);  // übernimmt die Referenz
    }

private:
    void build(DecodePath path) {
        path_ = path;
        const std::string desc =
            "appsrc name=src is-live=true format=time block=false max-bytes=4000000 ! " +
            decode_chain(path, w_, h_) + " ! appsink name=sink sync=false max-buffers=2 drop=true emit-signals=false";
        GError* err = nullptr;
        pipeline_ = gst_parse_launch(desc.c_str(), &err);
        if (err || !pipeline_) {
            std::fprintf(stderr, "[Fehler] Vorschau-Pipeline (%s): %s\n", decode_name(path),
                         err ? err->message : "unbekannt");
            if (err) g_error_free(err);
            if (pipeline_) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
            if (path != DecodePath::Software) build(DecodePath::Software);
            return;
        }

        appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        GstCaps* caps = gst_caps_from_string(
            "video/x-h264, stream-format=(string)byte-stream, alignment=(string)au");
        g_object_set(G_OBJECT(appsrc_), "caps", caps, nullptr);
        gst_caps_unref(caps);
        // Läuft der Decoder hinterher, lieber alte Frames verwerfen als Verzögerung aufbauen
        gst_util_set_object_arg(G_OBJECT(appsrc_), "leaky-type", "downstream");

        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        GstAppSinkCallbacks cb = {};
        cb.new_sample = &Tile::onNewSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, this, nullptr);
        gst_object_unref(sink);

        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE &&
            path != DecodePath::Software) {
            std::fprintf(stderr, "[Warnung] %s startet nicht – Vorschau dekodiert in Software\n", decode_name(path));
            teardown();
            build(DecodePath::Software);
        }
    }

    void teardown() {
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);  // stoppt auch die Callback-Threads
            if (appsrc_) gst_object_unref(appsrc_);
            gst_object_unref(pipeline_);
        }
        pipeline_ = nullptr;
        appsrc_ = nullptr;
    }

    // Meldet die Hardware-Kette einen Fehler (z.B. Treiber kann das Format nicht), auf Software umbauen
    void checkErrors() {
        if (!pipeline_ || path_ == DecodePath::Software) return;
        GstBus* bus = gst_element_get_bus(pipeline_);
        GstMessage* m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (!m) return;
        GError* e = nullptr;
        gst_message_parse_error(m, &e, nullptr);
        std::fprintf(stderr, "[Warnung] %s: %s – Vorschau dekodiert ab jetzt in Software\n", decode_name(path_),
                     e ? e->message : "Fehler");
        if (e) g_error_free(e);
        gst_message_unref(m);
        teardown();
        build(DecodePath::Software);
    }

public:
    bool hasPipeline() const { return pipeline_ != nullptr; }

    void setImage(const QImage& img) { image_ = img; update(); }
    void clearImage() { if (!image_.isNull()) { image_ = QImage(); update(); } }

    // EVS-Stil (Kamerakacheln): kleine, dünne Schrift ohne Kasten mit Schatten, Titel oben links,
    // Zeit unten links, Name unten mittig, Punkt oben rechts (Aufnahme-Status)
    void setEvsStyle(bool on) { evsStyle_ = on; }
    void setRec(int state) { if (state != rec_) { rec_ = state; update(); } }  // 0 kein Signal, 1 ok, 2 Warnung

    void setNameOnTop(bool top) { nameTop_ = top; }  // PGM/PVW: Name oben (unten ist die lange Statuszeile)

    void setName(const QString& n) {
        if (n == name_) return;
        name_ = n;
        update();
    }

    void setInfo(const QString& title, const QString& detail, const QColor& border, bool warn) {
        if (title == title_ && detail == detail_ && border == border_ && warn == warn_) return;
        title_ = title; detail_ = detail; border_ = border; warn_ = warn;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(18, 18, 18));

        // Das Bild füllt die ganze Kachel, die Beschriftung liegt IM Bild (wie bei EVS LSM-VIA)
        const int m = 4;  // Platz für den Tally-Rahmen
        const QRect area = rect().adjusted(m, m, -m, -m);
        QRect pic = area;

        if (!image_.isNull()) {
            QSize s = image_.size().scaled(area.size(), Qt::KeepAspectRatio);
            pic = QRect(QPoint(area.x() + (area.width() - s.width()) / 2,
                               area.y() + (area.height() - s.height()) / 2), s);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(pic, image_);
        } else {
            p.setPen(QColor(110, 110, 110));
            p.drawText(area, Qt::AlignCenter, QStringLiteral("kein Bild"));
        }

        if (evsStyle_) {
            paintEvsOverlay(p, pic);
        } else {
        // Titel oben links, Statuszeile unten links; halbtransparenter Hintergrund, damit sie auf
        // jedem Bild lesbar bleiben. Schriftgröße wächst mit der Kachel.
        const int pad = 6;
        const int maxTextW = std::max(40, pic.width() - 2 * pad - 12);

        QFont ft = font();
        ft.setBold(true);
        ft.setPixelSize(std::clamp(height() / 13, 12, 30));
        QFontMetrics fmt(ft);
        const QString title = fmt.elidedText(title_, Qt::ElideRight, maxTextW);
        const QRect tr(pic.left() + pad, pic.top() + pad, fmt.horizontalAdvance(title) + 12, fmt.height() + 4);
        p.fillRect(tr, QColor(0, 0, 0, 150));
        p.setFont(ft);
        p.setPen(Qt::white);
        p.drawText(tr, Qt::AlignCenter, title);

        int detailW = 0;  // Breite der Statuszeile unten links (der Name darf sie nicht überdecken)
        if (!detail_.isEmpty()) {
            QFont fd = font();
            int px = std::clamp(height() / 19, 10, 18);
            fd.setPixelSize(px);
            // Passt die Zeile nicht, Schrift verkleinern (bis 8 px), erst danach abschneiden
            while (px > 8 && QFontMetrics(fd).horizontalAdvance(detail_) > maxTextW) fd.setPixelSize(--px);
            QFontMetrics fmd(fd);
            const QString detail = fmd.elidedText(detail_, Qt::ElideRight, maxTextW);
            const QRect dr(pic.left() + pad, pic.bottom() - pad - fmd.height() - 4,
                           fmd.horizontalAdvance(detail) + 12, fmd.height() + 4);
            p.fillRect(dr, QColor(0, 0, 0, 150));
            p.setFont(fd);
            p.setPen(warn_ ? QColor(255, 140, 100) : QColor(220, 220, 220));
            p.drawText(dr, Qt::AlignCenter, detail);
            detailW = dr.width() + pad;
        }

        // Name der Kamera (Parameter --names): bei REC-Kacheln mittig unten, bei PGM/PVW mittig oben.
        // Unten entfällt er, wenn eine lange Warnzeile den Platz braucht.
        if (!name_.isEmpty()) {
            const int side = nameTop_ ? tr.width() : detailW;
            const int avail = pic.width() - 2 * (side + pad) - 12;
            if (avail > 40) {
                const QString nm = fmt.elidedText(name_, Qt::ElideRight, avail);
                const int w = fmt.horizontalAdvance(nm) + 16;
                const int y = nameTop_ ? pic.top() + pad : pic.bottom() - pad - fmt.height() - 4;
                const QRect nr(pic.left() + (pic.width() - w) / 2, y, w, fmt.height() + 4);
                p.fillRect(nr, QColor(0, 0, 0, 150));
                p.setFont(ft);
                p.setPen(QColor(255, 240, 160));
                p.drawText(nr, Qt::AlignCenter, nm);
            }
        }

        }

        if (border_.isValid()) {
            p.setPen(QPen(border_, 4));
            p.setBrush(Qt::NoBrush);
            p.drawRect(rect().adjusted(2, 2, -2, -2));
        }
    }

private:
    // Text mit dunklem Schatten (lesbar auf hellen und dunklen Bildern, ohne Kasten)
    static void shadowText(QPainter& p, const QRect& r, int flags, const QString& t, const QColor& c) {
        p.setPen(QColor(0, 0, 0, 230));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) p.drawText(r.translated(dx, dy), flags, t);
        p.setPen(c);
        p.drawText(r, flags, t);
    }

    void paintEvsOverlay(QPainter& p, const QRect& pic) {
        const int pad = 8;
        QFont f = font();
        f.setPixelSize(std::clamp(height() / 22, 11, 18));
        f.setBold(false);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int fl = Qt::AlignLeft | Qt::AlignVCenter;

        // Titel oben links
        const QRect tr(pic.left() + pad, pic.top() + pad, std::max(60, fm.horizontalAdvance(title_) + 4), fm.height());
        shadowText(p, tr, fl, title_, QColor(255, 255, 255));

        // Aufnahme-Punkt oben rechts: rot = Signal ok, orange = Warnung, grau = kein Signal
        const int d = std::max(8, fm.height() * 2 / 3);
        const QColor dot = rec_ == 1 ? QColor(235, 40, 40) : rec_ == 2 ? QColor(255, 150, 40) : QColor(110, 110, 110);
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QRect(pic.right() - pad - d, pic.top() + pad + (fm.height() - d) / 2, d, d));

        // Zeit (bei Problemen mit Warnung) unten links
        const int maxW = pic.width() - 2 * pad;
        const QString detail = fm.elidedText(detail_, Qt::ElideRight, maxW);
        const int dw = fm.horizontalAdvance(detail) + 4;
        const QRect dr(pic.left() + pad, pic.bottom() - pad - fm.height(), dw, fm.height());
        shadowText(p, dr, fl, detail, warn_ ? QColor(255, 150, 110) : QColor(235, 235, 235));

        // Name unten mittig; entfällt, wenn eine lange Warnzeile den Platz braucht
        if (!name_.isEmpty()) {
            const int avail = pic.width() - 2 * (dw + pad) - 12;
            if (avail > 30) {
                const QString nm = fm.elidedText(name_, Qt::ElideRight, avail);
                const QRect nr(pic.left() + (pic.width() - avail) / 2, pic.bottom() - pad - fm.height(), avail, fm.height());
                shadowText(p, nr, Qt::AlignHCenter | Qt::AlignVCenter, nm, QColor(255, 255, 255));
            }
        }
    }

    // Wird im GStreamer-Thread aufgerufen: Bild kopieren und per Queue an den GUI-Thread geben
    DecodePath path_ = DecodePath::Software;

    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer user) {
        Tile* self = static_cast<Tile*>(user);
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_OK;

        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
            if (map.size >= static_cast<size_t>(self->w_) * self->h_ * 3) {
                QImage img = QImage(map.data, self->w_, self->h_, self->w_ * 3, QImage::Format_RGB888).copy();
                QMetaObject::invokeMethod(self, [self, img]() { self->setImage(img); },
                                          Qt::QueuedConnection);
            }
            gst_buffer_unmap(buf, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    int w_, h_;  // Auflösung der dekodierten Vorschau
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    uint64_t pts_counter_ = 0;

    QImage image_;
    QString title_, detail_, name_;
    bool nameTop_ = false;
    bool evsStyle_ = false;
    int rec_ = 0;
    QColor border_;
    bool warn_ = false;
};

// ---------------------------------------------------------------------------------------------
// Hauptfenster
// ---------------------------------------------------------------------------------------------
class Monitor : public QWidget {
public:
    Monitor() {
        setWindowTitle(QStringLiteral("Replay Monitor"));
        setStyleSheet(QStringLiteral("background-color: #101010;"));

        // Kacheln werden in resizeEvent() platziert (layoutTiles), damit jede exakt 16:9 ist.
        for (size_t i = 0; i < NUM_CHANNELS; ++i) cams_[i] = new Tile(CAM_W, CAM_H, this);
        pgm_ = new Tile(BIG_W, BIG_H, this);
        pvw_ = new Tile(BIG_W, BIG_H, this);
        pgm_->setNameOnTop(true);
        pvw_->setNameOnTop(true);
        for (size_t i = 0; i < NUM_CHANNELS; ++i) {
            cams_[i]->setName(g_names.value(static_cast<int>(i)));
            cams_[i]->setEvsStyle(true);
        }
        logOpen();

        fds_.fill(-1);
        resize(1600, 1050);  // passt zum Block aus 16:9-Kacheln (bei 16:9-Bildschirm: Ränder links/rechts)

        auto* timer = new QTimer(this);
        QObject::connect(timer, &QTimer::timeout, this, [this]() { tick(); });
        timer->setTimerType(Qt::PreciseTimer);
        timer->start(TIMER_MS);
        clock_.start();
    }

    ~Monitor() override {
        for (int fd : fds_) if (fd >= 0) ::close(fd);
        if (log_) { logLine("=== Monitor beendet ==="); std::fclose(log_); }
        if (idx_) munmap(const_cast<SharedReplayIndex*>(idx_), sizeof(SharedReplayIndex));
        if (st_) munmap(const_cast<SharedPlayoutState*>(st_), sizeof(SharedPlayoutState));
    }

protected:
    void resizeEvent(QResizeEvent*) override { layoutTiles(); }

    // Aufteilung:  oben PGM | PVW (je halbe Breite), darunter die REC-Kacheln:
    //   6 Wege: 2 Reihen mit je 3 Kacheln;  4 Wege: 1 Reihe mit 4 Kacheln.
    // Jede Kachel ist exakt 16:9. Der ganze Block wird so groß wie möglich gemacht und mittig
    // ins Fenster gesetzt; passt das Fenster nicht genau, bleiben schwarze Ränder außen.
    void layoutTiles() {
        const int M = 6;  // Außenrand
        const int G = 6;  // Abstand zwischen den Kacheln
        const double W = width() - 2 * M;
        const double H = height() - 2 * M;
        const int n = static_cast<int>(nch_);
        const int cols = n <= 4 ? std::max(1, n) : 3;
        const int rows = (n + cols - 1) / cols;
        // Kamerabreite cw: Breite = cols*cw + (cols-1)G;  PGM-Breite bw = (Breite - G) / 2
        // Höhe = bw*9/16 + rows*cw*9/16 + rows*G
        double cw_w = (W - (cols - 1) * G) / cols;
        double cw_h = (H - 9.0 / 32.0 * (cols - 2) * G - rows * G) / (9.0 / 32.0 * cols + 9.0 / 16.0 * rows);
        int cw = std::max(16, static_cast<int>(std::floor(std::min(cw_w, cw_h))));
        int ch = cw * 9 / 16;
        int totalW = cols * cw + (cols - 1) * G;
        int bw = (totalW - G) / 2;
        int bh = bw * 9 / 16;
        int totalH = bh + G + rows * ch + (rows - 1) * G;
        int x0 = M + (static_cast<int>(W) - totalW) / 2;
        int y0 = M + (static_cast<int>(H) - totalH) / 2;

        pgm_->setGeometry(x0, y0, bw, bh);
        pvw_->setGeometry(x0 + totalW - bw, y0, bw, bh);
        for (size_t i = 0; i < NUM_CHANNELS; ++i) {
            if (i >= nch_) { cams_[i]->hide(); continue; }
            int r = static_cast<int>(i) / cols, c = static_cast<int>(i) % cols;
            cams_[i]->setGeometry(x0 + c * (cw + G), y0 + bh + G + r * (ch + G), cw, ch);
            cams_[i]->show();
        }
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_F11) {
            if (isFullScreen()) showNormal(); else showFullScreen();
        } else if (e->key() == Qt::Key_Escape && isFullScreen()) {
            showNormal();
        } else {
            QWidget::keyPressEvent(e);
        }
    }

private:
    // --- Shared Memory / Dateien öffnen (wird bei jedem Takt versucht, bis alles da ist) ---
    template <typename T>
    static const T* mapShm(const char* name) {
        int fd = shm_open(name, O_RDONLY, 0);
        if (fd < 0) return nullptr;
        struct stat sb;
        const T* ptr = nullptr;
        // Größe prüfen: wird der Bereich gerade erst angelegt, wäre ein Zugriff ein Absturz (SIGBUS)
        if (fstat(fd, &sb) == 0 && static_cast<size_t>(sb.st_size) >= sizeof(T)) {
            void* p = mmap(nullptr, sizeof(T), PROT_READ, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) ptr = static_cast<const T*>(p);
        }
        ::close(fd);
        return ptr;
    }

    void ensureOpen() {
        if (!idx_) idx_ = mapShm<SharedReplayIndex>(SHM_INDEX_NAME);
        if (!st_) st_ = mapShm<SharedPlayoutState>(SHM_STATE_NAME);
        if (idx_) {  // Zahl der REC-Wege vom Ingest übernehmen (4 oder 6) und Layout anpassen
            uint32_t n = idx_->active_channels.load(std::memory_order_relaxed);
            if (n >= 1 && n <= NUM_CHANNELS && n != nch_) { nch_ = n; layoutTiles(); }
        }
        for (size_t i = 0; i < nch_; ++i) {
            if (fds_[i] >= 0) continue;
            std::string path = std::string(STORAGE_DIR) + "/cam_" + std::to_string(i) + ".raw";
            fds_[i] = open(path.c_str(), O_RDONLY);
        }
    }

    // Frame für (cam, tick) holen; fehlt der Tick, bis zu 3 Frames zurückgehen.
    // Gleiche Absicherung wie im Playout: Seqlock-konsistent und in der Datei noch intakt.
    bool fetchFrame(int cam, uint64_t tick, uint64_t& usedTick) {
        if (cam < 0 || cam >= static_cast<int>(NUM_CHANNELS) || fds_[cam] < 0) return false;
        for (uint64_t back = 0; back <= 3 && back < tick; ++back) {
            SlotCopy s;
            if (!read_slot(idx_, cam, tick - back, s)) continue;
            if (s.frame_size == 0 || s.frame_size > MAX_SANE_FRAME_BYTES) continue;
            if (!data_intact(idx_, cam, s)) continue;

            buf_.resize(s.frame_size);
            ssize_t n = pread(fds_[cam], buf_.data(), s.frame_size, static_cast<off_t>(s.file_offset));
            if (n != static_cast<ssize_t>(s.frame_size) || !data_intact(idx_, cam, s)) return false;
            usedTick = s.tick;
            return true;
        }
        return false;
    }

    static QString timecode(uint64_t tick) {
        uint64_t total_sec = tick / FPS;
        return QString::asprintf("%02llu:%02llu:%02llu",
                                 static_cast<unsigned long long>((total_sec / 60) % 60),
                                 static_cast<unsigned long long>(total_sec % 60),
                                 static_cast<unsigned long long>(tick % FPS));
    }

    // --- Log: alle Messwerte (fps, Lücken, Ring, Anker) landen in monitor-log.txt im Arbeitsordner ---
    void logOpen() {
        log_ = std::fopen("monitor-log.txt", "a");
        if (log_) {
            logLine("=== Monitor gestartet ===");
            std::setvbuf(log_, nullptr, _IOLBF, 0);
        }
    }
    void logLine(const QString& text) {
        if (!log_) return;
        std::fprintf(log_, "[%s] %s\n", QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss").toUtf8().constData(),
                     text.toUtf8().constData());
    }
    // Nach jeder Statistik: Ereignisse sofort, Gesamtübersicht jede Minute
    void logStats() {
        if (!log_) return;
        // Der erste Messdurchlauf hat keine gültige fps-Rate; ab dem zweiten werden die Startwerte übernommen
        if (++logCalls_ < 2) return;
        bool first = logCalls_ == 2;
        bool summary = first || !logClock_.isValid() || logClock_.elapsed() >= 60000;
        if (summary) logClock_.restart();
        for (size_t c = 0; c < nch_; ++c) {
            const uint64_t anchors = idx_->stats[c].reanchors.load(std::memory_order_relaxed);
            if (first) { prevGapsTotal_[c] = gapsTotal_[c]; prevAnchors_[c] = anchors; }
            const QString name = g_names.value(static_cast<int>(c));
            const QString who = QString("REC%1%2").arg(c + 1).arg(name.isEmpty() ? QString() : " (" + name + ")");
            if (gapsTotal_[c] != prevGapsTotal_[c])
                logLine(QString("%1: Lücken +%2 (gesamt %3), %4 fps")
                            .arg(who).arg(gapsTotal_[c] - prevGapsTotal_[c]).arg(gapsTotal_[c]).arg(fps_[c], 0, 'f', 1));
            if (anchors != prevAnchors_[c])
                logLine(QString("%1: Anker +%2 (gesamt %3)").arg(who).arg(anchors - prevAnchors_[c]).arg(anchors));
            prevGapsTotal_[c] = gapsTotal_[c];
            prevAnchors_[c] = anchors;
            if (summary)
                logLine(QString("%1: %2 fps, Lücken(60s) %3, gesamt %4, Ring G%5 %6%, Anker %7")
                            .arg(who).arg(fps_[c], 0, 'f', 1).arg(gaps_[c]).arg(gapsTotal_[c]).arg(gen_[c]).arg(ringPct_[c]).arg(anchors));
        }
    }

    // --- Statistik, 1x pro Sekunde ---
    void updateStats(uint64_t live) {
        const double dt = std::max(0.2, statsTimer_.isValid() ? statsTimer_.elapsed() / 1000.0 : 1.0);
        statsTimer_.restart();

        for (size_t c = 0; c < nch_; ++c) {
            // fps aus dem Zähler des Ingest
            uint64_t fr = idx_->stats[c].frames_received.load(std::memory_order_relaxed);
            fps_[c] = (fr >= lastFrames_[c]) ? static_cast<double>(fr - lastFrames_[c]) / dt : 0.0;
            lastFrames_[c] = fr;

            // Lücken der letzten 60 s: Slots, die nicht den erwarteten Tick tragen
            uint64_t hi = live > LIVE_MARGIN_TICKS ? live - LIVE_MARGIN_TICKS : 0;
            uint64_t lo = hi > GAP_WINDOW_TICKS ? hi - GAP_WINDOW_TICKS + 1 : 1;
            // Erst ab dem ersten empfangenen Frame zählen (davor ist der Kanal nur noch nicht angelaufen)
            if (firstTick_[c] == 0) {
                for (uint64_t t = lo; t <= hi; ++t)
                    if (idx_->slots[c][t % TOTAL_SLOTS].master_tick.load(std::memory_order_relaxed) == t) {
                        firstTick_[c] = t;
                        break;
                    }
            }
            if (firstTick_[c] != 0) lo = std::max(lo, firstTick_[c]);
            uint32_t gaps = 0;
            for (uint64_t t = (firstTick_[c] != 0 ? lo : hi + 1); t <= hi; ++t)
                if (idx_->slots[c][t % TOTAL_SLOTS].master_tick.load(std::memory_order_relaxed) != t) ++gaps;
            gaps_[c] = gaps;
            // Gesamtzahl der Lücken seit dem ersten Frame: jede Sekunde nur die neu hinzugekommenen Ticks prüfen
            if (firstTick_[c] != 0) {
                if (scanned_[c] < firstTick_[c]) scanned_[c] = firstTick_[c] - 1;
                if (hi > scanned_[c]) {
                    // höchstens so weit zurück, wie der Index noch gültig ist
                    uint64_t from = scanned_[c] + 1;
                    const uint64_t maxBack = TOTAL_SLOTS - 100;
                    if (hi - from + 1 > maxBack) from = hi - maxBack + 1;
                    for (uint64_t t = from; t <= hi; ++t)
                        if (idx_->slots[c][t % TOTAL_SLOTS].master_tick.load(std::memory_order_relaxed) != t) ++gapsTotal_[c];
                }
                if (hi > scanned_[c]) scanned_[c] = hi;
            }
            // Warnung bei Lücken erst, wenn das volle 60-s-Fenster seit dem ersten Frame vergangen ist
            gapWindowFull_[c] = firstTick_[c] != 0 && hi >= firstTick_[c] + GAP_WINDOW_TICKS;

            // Ring-Stand: Umlauf und Füllung der Datei
            uint64_t wp = idx_->write_pos[c].load(std::memory_order_relaxed);
            gen_[c] = static_cast<uint32_t>(wp >> GEN_SHIFT);
            ringPct_[c] = static_cast<int>(100.0 * static_cast<double>(wp & OFFSET_MASK) /
                                           static_cast<double>(MAX_FILE_SIZE_BYTES));
        }
    }

    void tick() {
        ++ticks_;
        ensureOpen();

        if (!idx_) {
            for (auto* t : cams_) { t->clearImage(); t->setInfo(QStringLiteral("kein Ingest"), QString(), QColor(), true); }
            pgm_->clearImage(); pgm_->setInfo(QStringLiteral("PGM"), QStringLiteral("kein Ingest"), QColor(200, 30, 30), true);
            pvw_->clearImage(); pvw_->setInfo(QStringLiteral("PVW"), QStringLiteral("kein Ingest"), QColor(30, 160, 60), true);
            return;
        }

        const uint64_t live = idx_->current_master_tick.load(std::memory_order_acquire);

        // Läuft der Ingest? (Master-Tick muss weiterzählen)
        if (live != lastLive_) { lastLive_ = live; liveChanged_.restart(); }
        const bool ingestAlive = live > 0 && liveChanged_.isValid() && liveChanged_.elapsed() < 1000;

        // Läuft das Playout? (Heartbeat muss weiterzählen)
        bool playoutAlive = false;
        if (st_) {
            uint64_t hb = st_->heartbeat.load(std::memory_order_relaxed);
            if (hb != lastHb_) { lastHb_ = hb; hbChanged_.restart(); }
            playoutAlive = hbChanged_.isValid() && hbChanged_.elapsed() < 1000;
        }

        // Hat sich seit dem letzten Aufruf etwas getan? Sonst nichts tun (außer 1x pro Sekunde Statistik).
        const uint64_t hbNow = st_ ? st_->heartbeat.load(std::memory_order_acquire) : 0;
        const bool liveNew = (live != seenLive_);
        const bool hbNew = (hbNow != seenHb_);
        const bool statsDue = !statsValid_ || !statsClock_.isValid() || statsClock_.elapsed() >= 1000;
        if (!liveNew && !hbNew && !statsDue) return;
        seenLive_ = live;
        seenHb_ = hbNow;

        if (statsDue) {
            statsClock_.restart();
            if (live > 0) { updateStats(live); statsValid_ = true; logStats(); }
        }

        // Tally: welche Kamera liegt auf PGM / PVW?
        int pgmCam = -1, pvwCam = -1;
        uint64_t pgmTick = 0, pvwTick = 0;
        double pgmSpeed = 0, pvwSpeed = 0;
        bool gang = false;
        if (st_ && playoutAlive) {
            pgmCam = st_->pgm.active_cam.load(std::memory_order_relaxed);
            pvwCam = st_->pvw.active_cam.load(std::memory_order_relaxed);
            pgmTick = static_cast<uint64_t>(std::llround(st_->pgm.playhead_tick.load(std::memory_order_relaxed)));
            pvwTick = static_cast<uint64_t>(std::llround(st_->pvw.playhead_tick.load(std::memory_order_relaxed)));
            pgmSpeed = st_->pgm.speed.load(std::memory_order_relaxed);
            pvwSpeed = st_->pvw.speed.load(std::memory_order_relaxed);
            gang = st_->gang_mode.load(std::memory_order_relaxed) != 0;
        }

        const QColor red(210, 30, 30), green(30, 170, 70);

        // --- Kameras ---
        const uint64_t camEvery = static_cast<uint64_t>(std::max(1, static_cast<int>(FPS) / g_cam_fps));
        const uint64_t bigEvery = static_cast<uint64_t>(std::max(1, static_cast<int>(FPS) / g_big_fps));
        // Neues Kamerabild, wenn der Ingest-Takt in einen neuen Abschnitt (z.B. alle 2 Ticks = 25 fps) kommt;
        // neues PGM/PVW-Bild, wenn das Playout einen neuen Takt ausgegeben hat.
        const bool camFrame = liveNew && (live / camEvery != lastCamSlot_);
        const bool bigFrame = hbNew && (hbNow / bigEvery != lastBigSlot_);
        if (camFrame) lastCamSlot_ = live / camEvery;
        if (bigFrame) lastBigSlot_ = hbNow / bigEvery;
        for (size_t c = 0; c < nch_; ++c) {
            Tile* t = cams_[c];
            QColor border;
            if (static_cast<int>(c) == pgmCam) border = red;
            else if (static_cast<int>(c) == pvwCam) border = green;

            QString detail;
            bool warn = false;
            if (!ingestAlive) {
                detail = QStringLiteral("Ingest steht");
                warn = true;
                t->clearImage();
            } else {
                warn = (fps_[c] < 45.0) || (gapWindowFull_[c] && gaps_[c] > 0);
                detail = timecode(live);
                if (warn)
                    detail += QString::asprintf(" | ! %.1f fps | Lücken(60s): %u", fps_[c], gaps_[c]);

                if (camFrame && live > LIVE_MARGIN_TICKS + 5) {
                    uint64_t used;
                    if (fetchFrame(static_cast<int>(c), live - LIVE_MARGIN_TICKS, used))
                        t->pushFrame(buf_.data(), buf_.size());
                }
            }
            t->setInfo(QString("REC %1").arg(c + 1), detail, border, warn);
            t->setRec(!ingestAlive ? 0 : fps_[c] >= 45.0 ? 1 : fps_[c] > 0.0 ? 2 : 0);
        }

        // --- PGM / PVW ---
        auto updateEngine = [&](Tile* t, const char* name, int cam, uint64_t tickPos, double speed,
                                const QColor& border, const PlayoutEngineState* es,
                                std::unique_ptr<ClipFiles>& clipFiles) {
            if (!st_ || !playoutAlive) {
                t->clearImage();
                t->setInfo(QString(name), QStringLiteral("Playout läuft nicht"), border, true);
                return;
            }
            const uint32_t mode = es->mode.load(std::memory_order_relaxed);
            const int clipId = es->clip_id.load(std::memory_order_relaxed);
            const int clipFrame = es->clip_frame.load(std::memory_order_relaxed);
            if (bigFrame && cam >= 0 && cam < static_cast<int>(nch_)) {
                bool got = false;
                if (mode != MODE_RING && clipId > 0) {
                    // Clip: aus dem Clip-Speicher, solange der Frame schon kopiert ist
                    if (!clipFiles || clipFiles->id() != clipId) {
                        clipFiles = std::make_unique<ClipFiles>();
                        if (!clipFiles->open(clipId)) clipFiles.reset();
                    }
                    if (clipFiles && clipFrame >= 0 &&
                        static_cast<uint32_t>(clipFrame) < clipFiles->frames_available(cam)) {
                        for (int back = 0; back <= 3 && back <= clipFrame && !got; ++back)
                            got = clipFiles->read_frame(cam, static_cast<uint32_t>(clipFrame - back), buf_);
                    }
                }
                if (!got && ingestAlive && tickPos > 0) {  // Ring (bzw. noch nicht kopierter Clip-Frame)
                    uint64_t used;
                    got = fetchFrame(cam, tickPos, used);
                }
                if (got) t->pushFrame(buf_.data(), buf_.size());
            }
            QString detail;
            if (mode == MODE_RING) {
                const uint64_t behind = live > tickPos ? live - tickPos : 0;
                detail = QString::asprintf("TC %s | %.0f%% | %.1f s hinter Live%s",
                                           timecode(tickPos).toUtf8().constData(), speed * 100.0,
                                           static_cast<double>(behind) / FPS, gang ? " | GANG" : "");
            } else {
                QString where = mode == MODE_LIST
                    ? QString::asprintf("LISTE %d [%d/%d] | ", es->list_no.load(), es->list_pos.load() + 1,
                                        es->list_len.load())
                    : QString();
                detail = where + QString::asprintf("CLIP %d | Bild %d | %.0f%%", clipId, clipFrame + 1, speed * 100.0);
            }
            const bool in = es->mark_in.load(std::memory_order_relaxed) != 0;
            const bool out = es->mark_out.load(std::memory_order_relaxed) != 0;
            if (in || out) detail += QString(" | %1 %2").arg(in ? "IN" : "--", out ? "OUT" : "---");
            t->setName(cam >= 0 ? g_names.value(cam) : QString());
            t->setInfo(QString("%1  REC %2").arg(name).arg(cam + 1), detail, border, false);
        };
        updateEngine(pgm_, "PGM", pgmCam, pgmTick, pgmSpeed, red, st_ ? &st_->pgm : nullptr, pgmClip_);
        updateEngine(pvw_, "PVW", pvwCam, pvwTick, pvwSpeed, green, st_ ? &st_->pvw : nullptr, pvwClip_);
    }

    // Kacheln
    size_t nch_ = NUM_CHANNELS;  // aktive REC-Wege (vom Ingest)
    std::array<Tile*, NUM_CHANNELS> cams_{};
    Tile* pgm_ = nullptr;
    Tile* pvw_ = nullptr;

    // Quellen (nur lesend)
    const SharedReplayIndex* idx_ = nullptr;
    const SharedPlayoutState* st_ = nullptr;
    std::array<int, NUM_CHANNELS> fds_{};
    std::vector<uint8_t> buf_;
    std::unique_ptr<ClipFiles> pgmClip_, pvwClip_;  // geöffnete Clips für PGM/PVW

    // Lebenszeichen
    QElapsedTimer clock_, liveChanged_, hbChanged_, statsTimer_;
    uint64_t lastLive_ = 0;
    uint64_t seenLive_ = 0, seenHb_ = 0;          // zuletzt verarbeiteter Ingest-Tick / Playout-Heartbeat
    uint64_t lastCamSlot_ = 0, lastBigSlot_ = 0;
    QElapsedTimer statsClock_;
    uint64_t lastHb_ = 0;
    uint64_t ticks_ = 0;

    // Statistik
    bool statsValid_ = false;
    std::array<uint64_t, NUM_CHANNELS> lastFrames_{};
    std::array<uint64_t, NUM_CHANNELS> firstTick_{};
    std::array<double, NUM_CHANNELS> fps_{};
    std::array<uint32_t, NUM_CHANNELS> gaps_{};
    std::array<uint64_t, NUM_CHANNELS> prevGapsTotal_{}, prevAnchors_{};
    FILE* log_ = nullptr;
    QElapsedTimer logClock_;
    int logCalls_ = 0;
    std::array<uint64_t, NUM_CHANNELS> gapsTotal_{};  // Lücken seit dem ersten Frame (gesamt)
    std::array<uint64_t, NUM_CHANNELS> scanned_{};    // bis zu diesem Tick sind die Lücken schon gezählt
    std::array<bool, NUM_CHANNELS> gapWindowFull_{};
    std::array<uint32_t, NUM_CHANNELS> gen_{};
    std::array<int, NUM_CHANNELS> ringPct_{};
};

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);
    QApplication app(argc, argv);

    // Bildraten:  evs-monitor --cam-fps 25 --big-fps 50   (bei zu hoher CPU-Last z.B. --cam-fps 10)
    auto valid = [](int f) { return f == 50 || f == 25 || f == 10 || f == 5; };
    for (int i = 1; i + 1 < argc; ++i) {
        int v = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--cam-fps") == 0 && valid(v)) g_cam_fps = v;
        if (std::strcmp(argv[i], "--big-fps") == 0 && valid(v)) g_big_fps = v;
    }
    bool allow_hw = true;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--no-hw") == 0) allow_hw = false;
    // Kamera-Namen:  evs-monitor --names "Mitte;16er Hoch;Tor links;Tor rechts;Halbtotale;Spielfeld"
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--names") == 0) g_names = QString::fromUtf8(argv[i + 1]).split(';');
    choose_decode_path(allow_hw);
    std::printf("[Monitor] REC %d fps, PGM/PVW %d fps, Dekodierung: %s\n", g_cam_fps, g_big_fps,
                decode_name(g_decode));

    Monitor w;
    w.show();

    // Testhilfe:  evs-monitor --snapshot bild.png   speichert nach 6 s ein Bild des Fensters und beendet sich
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--snapshot") == 0) {
            const QString file = QString::fromUtf8(argv[i + 1]);
            QTimer::singleShot(6000, &app, [&w, &app, file]() {
                w.grab().save(file);
                app.quit();
            });
        }
    }
    return app.exec();
}

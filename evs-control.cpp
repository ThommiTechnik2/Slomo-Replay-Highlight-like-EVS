// evs-control.cpp — Fenster für Clips und Listen (zweiter Bildschirm)
//
// Zeigt den Clip-Speicher und die 10 Listen und schickt Befehle an evs-playout (UDP 127.0.0.1:20200).
// Liest Clips/Listen direkt aus ./clips und ./lists und lädt neu, sobald das Playout eine Änderung meldet.
// Muss wie die anderen Programme aus dem RUN-Ordner (evs-run) gestartet werden.
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <map>

#include "evs-clips.h"

static QString dur(uint32_t frames) {
    return QString::asprintf("%u:%02u", static_cast<unsigned>(frames / FPS), static_cast<unsigned>(frames % FPS));
}

class Control : public QWidget {
public:
    Control() {
        setWindowTitle("EVS Clips & Listen");
        resize(1500, 850);
        setStyleSheet(
            "QWidget{background:#1b1d22;color:#e6e6e6;font-size:14px}"
            "QTableWidget{background:#14161a;gridline-color:#2c2f36;selection-background-color:#3a5f9a}"
            "QHeaderView::section{background:#262931;color:#cfcfcf;padding:4px;border:0}"
            "QPushButton{background:#2d313a;border:1px solid #444a55;padding:7px 12px;border-radius:4px}"
            "QPushButton:hover{background:#3a404b}"
            "QPushButton#red{background:#8a1f1f}QPushButton#green{background:#1f6a33}"
            "QLineEdit,QComboBox,QDoubleSpinBox{background:#14161a;border:1px solid #444a55;padding:5px}"
            "QTabBar::tab{background:#262931;padding:7px 14px}QTabBar::tab:selected{background:#3a5f9a}");

        // --- Kopfzeile: Status und Mark/Clip/Take ---
        status_ = new QLabel("Playout nicht verbunden");
        QFont f = status_->font();
        f.setPointSize(f.pointSize() + 2);
        f.setBold(true);
        status_->setFont(f);
        reply_ = new QLabel();
        reply_->setStyleSheet("color:#9fb4d8");

        auto* bIn = new QPushButton("MARK IN  (PVW)");
        auto* bOut = new QPushButton("MARK OUT  (PVW)");
        name_ = new QLineEdit();
        name_->setPlaceholderText("Clip-Name (optional)");
        name_->setMinimumWidth(220);
        auto* bSave = new QPushButton("CLIP SPEICHERN");
        bSave->setObjectName("green");
        auto* bTake = new QPushButton("TAKE");
        bTake->setObjectName("red");
        auto* bLive = new QPushButton("PGM LIVE");
        connect(bIn, &QPushButton::clicked, this, [this] { send("MARKIN PVW"); });
        connect(bOut, &QPushButton::clicked, this, [this] { send("MARKOUT PVW"); });
        connect(bSave, &QPushButton::clicked, this, [this] {
            send("SAVECLIP PVW " + name_->text().trimmed());
            name_->clear();
        });
        connect(name_, &QLineEdit::returnPressed, bSave, &QPushButton::click);
        connect(bTake, &QPushButton::clicked, this, [this] { send("TAKE"); });
        connect(bLive, &QPushButton::clicked, this, [this] { send("LIVE PGM"); });

        auto* top = new QHBoxLayout();
        top->addWidget(bIn);
        top->addWidget(bOut);
        top->addWidget(name_);
        top->addWidget(bSave);
        top->addSpacing(30);
        top->addWidget(bTake);
        top->addWidget(bLive);
        top->addStretch();

        // --- Links: Clips ---
        clips_ = new QTableWidget(0, 6);
        clips_->setHorizontalHeaderLabels({"Nr", "Name", "TC IN", "Dauer", "Winkel", "Status"});
        setupTable(clips_);
        clips_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        connect(clips_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
            send("RECALL " + QString::number(clipIdAt(r)) + " PVW");
        });

        auto* bRecall = new QPushButton("Auf PVW laden");
        auto* bToList = new QPushButton("→ in aktuelle Liste");
        auto* bRename = new QPushButton("Umbenennen");
        auto* bDel = new QPushButton("Löschen");
        connect(bRecall, &QPushButton::clicked, this, [this] {
            if (int id = selectedClip()) send("RECALL " + QString::number(id) + " PVW");
        });
        connect(bToList, &QPushButton::clicked, this, [this] { addSelectedClipsToList(); });
        connect(bRename, &QPushButton::clicked, this, [this] {
            int id = selectedClip();
            if (!id) return;
            bool ok = false;
            QString n = QInputDialog::getText(this, "Clip umbenennen", "Neuer Name:", QLineEdit::Normal,
                                              clipName(id), &ok);
            if (ok && !n.trimmed().isEmpty()) send("RENAME " + QString::number(id) + " " + n.trimmed());
        });
        connect(bDel, &QPushButton::clicked, this, [this] {
            int id = selectedClip();
            if (!id) return;
            if (QMessageBox::question(this, "Clip löschen",
                                      QString("Clip %1 \"%2\" endgültig löschen?\nEr wird auch aus allen Listen entfernt.")
                                          .arg(id).arg(clipName(id))) == QMessageBox::Yes)
                send("DELCLIP " + QString::number(id));
        });

        auto* leftBtns = new QHBoxLayout();
        leftBtns->addWidget(bRecall);
        leftBtns->addWidget(bToList);
        leftBtns->addWidget(bRename);
        leftBtns->addWidget(bDel);
        auto* left = new QWidget();
        auto* lv = new QVBoxLayout(left);
        lv->setContentsMargins(0, 0, 0, 0);
        auto* lt = new QLabel("CLIPS");
        lt->setStyleSheet("font-weight:bold;font-size:16px");
        lv->addWidget(lt);
        lv->addWidget(clips_);
        lv->addLayout(leftBtns);

        // --- Rechts: 10 Listen ---
        tabs_ = new QTabWidget();
        for (int l = 1; l <= NUM_LISTS; ++l) {
            auto* t = new QTableWidget(0, 5);
            t->setHorizontalHeaderLabels({"Pos", "Clip", "Name", "Dauer", "Tempo"});
            setupTable(t);
            t->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
            connect(t, &QTableWidget::cellDoubleClicked, this, [this, l](int r, int) {
                send(QString("LISTCUE %1 %2").arg(l).arg(r + 1));
            });
            lists_[l] = t;
            tabs_->addTab(t, QString("Liste %1").arg(l));
        }

        auto* bUp = new QPushButton("▲");
        auto* bDown = new QPushButton("▼");
        auto* bRemove = new QPushButton("Entfernen");
        speed_ = new QDoubleSpinBox();
        speed_->setRange(0, 200);
        speed_->setSuffix(" %");
        speed_->setDecimals(0);
        speed_->setValue(100);
        auto* bSpeed = new QPushButton("Tempo setzen");
        target_ = new QComboBox();
        for (int l = 1; l <= NUM_LISTS; ++l) target_->addItem(QString("Liste %1").arg(l), l);
        auto* bMove = new QPushButton("Verschieben");
        auto* bCopy = new QPushButton("Kopieren");
        connect(bUp, &QPushButton::clicked, this, [this] { moveEntry(-1); });
        connect(bDown, &QPushButton::clicked, this, [this] { moveEntry(+1); });
        connect(bRemove, &QPushButton::clicked, this, [this] {
            int l = currentList(), p = selectedPos();
            if (p > 0) send(QString("LISTDEL %1 %2").arg(l).arg(p));
        });
        connect(bSpeed, &QPushButton::clicked, this, [this] {
            int l = currentList(), p = selectedPos();
            if (p > 0) send(QString("LISTSPEED %1 %2 %3").arg(l).arg(p).arg(speed_->value() / 100.0));
        });
        connect(bMove, &QPushButton::clicked, this, [this] { transfer("MOVE"); });
        connect(bCopy, &QPushButton::clicked, this, [this] { transfer("COPY"); });

        auto* bCue = new QPushButton("Liste auf PGM cuen");
        auto* bPlay = new QPushButton("▶ PLAY PGM");
        bPlay->setObjectName("red");
        auto* bStop = new QPushButton("■ STOP PGM");
        connect(bCue, &QPushButton::clicked, this, [this] {
            int p = std::max(1, selectedPos());
            send(QString("LISTCUE %1 %2").arg(currentList()).arg(p));
        });
        connect(bPlay, &QPushButton::clicked, this, [this] { send("PLAY PGM"); });
        connect(bStop, &QPushButton::clicked, this, [this] { send("STOP PGM"); });

        auto* rb1 = new QHBoxLayout();
        rb1->addWidget(bUp);
        rb1->addWidget(bDown);
        rb1->addWidget(bRemove);
        rb1->addSpacing(20);
        rb1->addWidget(speed_);
        rb1->addWidget(bSpeed);
        rb1->addSpacing(20);
        rb1->addWidget(target_);
        rb1->addWidget(bMove);
        rb1->addWidget(bCopy);
        rb1->addStretch();
        auto* rb2 = new QHBoxLayout();
        rb2->addWidget(bCue);
        rb2->addWidget(bPlay);
        rb2->addWidget(bStop);
        rb2->addStretch();

        auto* right = new QWidget();
        auto* rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        auto* rt = new QLabel("LISTEN  (Doppelklick: ab diesem Eintrag auf PGM cuen)");
        rt->setStyleSheet("font-weight:bold;font-size:16px");
        rv->addWidget(rt);
        rv->addWidget(tabs_);
        rv->addLayout(rb1);
        rv->addLayout(rb2);

        auto* split = new QSplitter();
        split->addWidget(left);
        split->addWidget(right);
        split->setStretchFactor(0, 1);
        split->setStretchFactor(1, 1);

        auto* root = new QVBoxLayout(this);
        alert_ = new QLabel();
        alert_->setAlignment(Qt::AlignCenter);
        alert_->setStyleSheet("background:#c0182b;color:white;font-weight:bold;font-size:22px;padding:8px;border-radius:4px");
        alert_->hide();
        alertTimer_ = new QTimer(this);
        alertTimer_->setSingleShot(true);
        connect(alertTimer_, &QTimer::timeout, this, [this] { alert_->hide(); });

        root->addWidget(alert_);
        root->addLayout(top);
        root->addWidget(status_);
        root->addWidget(split, 1);
        root->addWidget(reply_);

        sock_ = socket(AF_INET, SOCK_DGRAM, 0);

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { tick(); });
        timer->start(100);
        reload();
    }

    ~Control() override {
        if (sock_ >= 0) ::close(sock_);
        if (st_) munmap(st_, sizeof(SharedPlayoutState));
    }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_F11) { isFullScreen() ? showNormal() : showFullScreen(); return; }
        QWidget::keyPressEvent(e);
    }

private:
    static void setupTable(QTableWidget* t) {
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->verticalHeader()->setVisible(false);
        t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }

    // Befehl an das Playout; Antwort (max. 300 ms) in der Fußzeile
    void send(const QString& cmd) {
        if (sock_ < 0) return;
        QByteArray b = cmd.toUtf8();
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(CONTROL_PORT);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(sock_, b.constData(), static_cast<size_t>(b.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        pollfd pf{sock_, POLLIN, 0};
        char buf[512];
        QString r = "keine Antwort – läuft evs-playout?";
        if (poll(&pf, 1, 300) > 0) {
            ssize_t n = recv(sock_, buf, sizeof(buf) - 1, 0);
            if (n > 0) { buf[n] = 0; r = QString::fromUtf8(buf); }
        }
        reply_->setText(cmd + "  →  " + r);
        reply_->setStyleSheet(r.startsWith("OK") ? "color:#9fd8a8" : "color:#ff8c64");
        lastRev_ = UINT64_MAX;  // sofort neu laden
    }

    int currentList() const { return tabs_->currentIndex() + 1; }

    int selectedPos() const {  // 1-basiert, 0 = nichts
        auto* t = lists_[currentList()];
        return t->currentRow() >= 0 && !t->selectedItems().isEmpty() ? t->currentRow() + 1 : 0;
    }

    int clipIdAt(int row) const {
        auto* it = clips_->item(row, 0);
        return it ? it->text().toInt() : 0;
    }

    int selectedClip() const {
        return clips_->currentRow() >= 0 && !clips_->selectedItems().isEmpty() ? clipIdAt(clips_->currentRow()) : 0;
    }

    QString clipName(int id) const {
        for (const auto& m : clipMeta_) if (m.id == id) return QString::fromStdString(m.name);
        return {};
    }

    void addSelectedClipsToList() {
        QList<int> rows;
        for (auto* it : clips_->selectedItems()) if (!rows.contains(it->row())) rows << it->row();
        std::sort(rows.begin(), rows.end());
        for (int r : rows) send(QString("LISTADD %1 %2").arg(currentList()).arg(clipIdAt(r)));
    }

    void moveEntry(int dir) {
        int l = currentList(), p = selectedPos();
        int n = lists_[l]->rowCount();
        if (p < 1 || p + dir < 1 || p + dir > n) return;
        send(QString("LISTMOVE %1 %2 %3").arg(l).arg(p).arg(p + dir));
        pendingSelect_ = {l, p + dir - 1};
    }

    void transfer(const char* how) {
        int l = currentList(), p = selectedPos();
        int t = target_->currentData().toInt();
        if (p > 0) send(QString("LISTXFER %1 %2 %3 %4").arg(l).arg(p).arg(t).arg(how));
    }

    // Kopierfortschritt eines Clips (über die Größe der Index-Datei von Kamera 1)
    static int copiedPercent(const ClipMeta& m) {
        struct stat st;
        if (stat(clip_file(m.id, 0, ".idx").c_str(), &st) != 0 || m.frames == 0) return 0;
        uint64_t n = static_cast<uint64_t>(st.st_size) / sizeof(ClipIdxEntry);
        return static_cast<int>(std::min<uint64_t>(100, n * 100 / m.frames));
    }

    void reload() {
        clipMeta_ = load_all_clips();
        int keepClip = selectedClip();
        clips_->setRowCount(static_cast<int>(clipMeta_.size()));
        std::map<int, const ClipMeta*> byId;
        int row = 0;
        for (const auto& m : clipMeta_) {
            byId[m.id] = &m;
            int pct = copiedPercent(m);
            QStringList cells = {QString::number(m.id), QString::fromStdString(m.name),
                                 QString::fromStdString(m.tc_in), dur(m.frames), QString("%1 (REC %2)").arg(QChar('A' + m.cam)).arg(m.cam + 1),
                                 pct >= 100 ? "gesichert" : QString("kopiert %1 %").arg(pct)};
            for (int c = 0; c < 6; ++c) clips_->setItem(row, c, new QTableWidgetItem(cells[c]));
            if (m.id == keepClip) clips_->selectRow(row);
            ++row;
        }
        for (int l = 1; l <= NUM_LISTS; ++l) {
            auto entries = load_list(l);
            auto* t = lists_[l];
            int keep = t->currentRow();
            t->setRowCount(static_cast<int>(entries.size()));
            for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                const auto& e = entries[i];
                auto it = byId.find(e.clip_id);
                QStringList cells = {QString::number(i + 1), QString::number(e.clip_id),
                                     it != byId.end() ? QString::fromStdString(it->second->name) : "(gelöscht)",
                                     it != byId.end() ? dur(it->second->frames) : "",
                                     QString::asprintf("%.0f %%", e.speed * 100.0)};
                for (int c = 0; c < 5; ++c) t->setItem(i, c, new QTableWidgetItem(cells[c]));
            }
            if (pendingSelect_.first == l) { keep = pendingSelect_.second; pendingSelect_ = {0, 0}; }
            if (keep >= 0 && keep < t->rowCount()) t->selectRow(keep);
            tabs_->setTabText(l - 1, entries.empty() ? QString("Liste %1").arg(l)
                                                     : QString("Liste %1 (%2)").arg(l).arg(entries.size()));
        }
        markPlaying();
    }

    // Laufenden Listeneintrag hervorheben
    void markPlaying() {
        int pl = st_ ? st_->pgm.list_no.load() : 0;
        int pp = st_ ? st_->pgm.list_pos.load() : -1;
        for (int l = 1; l <= NUM_LISTS; ++l) {
            auto* t = lists_[l];
            for (int r = 0; r < t->rowCount(); ++r) {
                bool on = (l == pl && r == pp);
                for (int c = 0; c < t->columnCount(); ++c)
                    if (auto* it = t->item(r, c)) {
                        it->setBackground(on ? QColor(140, 30, 30) : QColor(0, 0, 0, 0));
                        QFont fo = it->font();
                        fo.setBold(on);
                        it->setFont(fo);
                    }
            }
        }
        playingList_ = pl;
        playingPos_ = pp;
    }

    void ensureState() {
        if (st_) return;
        int fd = shm_open(SHM_STATE_NAME, O_RDONLY, 0);
        if (fd < 0) return;
        struct stat sb;
        if (fstat(fd, &sb) == 0 && sb.st_size >= static_cast<off_t>(sizeof(SharedPlayoutState))) {
            void* p = mmap(nullptr, sizeof(SharedPlayoutState), PROT_READ, MAP_SHARED, fd, 0);
            if (p != MAP_FAILED) st_ = static_cast<SharedPlayoutState*>(p);
        }
        ::close(fd);
    }

    static QString engineText(const char* name, const PlayoutEngineState& e) {
        QString src;
        switch (e.mode.load()) {
            case MODE_CLIP: src = QString("Clip %1").arg(e.clip_id.load()); break;
            case MODE_LIST: src = QString("Liste %1 [%2/%3] Clip %4").arg(e.list_no.load()).arg(e.list_pos.load() + 1)
                                      .arg(e.list_len.load()).arg(e.clip_id.load()); break;
            default: src = "Ring"; break;
        }
        QString marks;
        if (e.mark_in.load() || e.mark_out.load())
            marks = QString("  %1 %2").arg(e.mark_in.load() ? "IN" : "--", e.mark_out.load() ? "OUT" : "---");
        return QString("%1: %2 · REC %3 · %4 %%5").arg(name, src).arg(e.active_cam.load() + 1)
                   .arg(e.speed.load() * 100.0, 0, 'f', 0).arg(marks);
    }

    void tick() {
        ensureState();
        if (!st_) { status_->setText("Playout nicht verbunden (evs-playout starten)"); return; }
        uint64_t hb = st_->heartbeat.load();
        bool alive = hb != lastHb_;
        if (alive) { lastHb_ = hb; hbClock_ = 0; } else if (++hbClock_ > 10) {
            status_->setText("Playout läuft nicht");
            return;
        }
        status_->setText(engineText("PGM", st_->pgm) + "        " + engineText("PVW", st_->pvw));

        // Fehlermeldung des Playouts (auch von Controller oder Tastatur): kurz als rotes Banner zeigen
        uint32_t seq = st_->alert_seq.load();
        if (!alertInit_) { lastAlert_ = seq; alertInit_ = true; }
        if (seq != lastAlert_) {
            lastAlert_ = seq;
            alert_->setText(QString::fromUtf8(st_->alert_text).toUpper());
            alert_->show();
            alertTimer_->start(4000);
        }

        uint64_t rev = st_->catalog_rev.load();
        bool copying = false;
        for (const auto& m : clipMeta_) if (copiedPercent(m) < 100) { copying = true; break; }
        if (rev != lastRev_ || (copying && ++copyPoll_ % 5 == 0)) {
            lastRev_ = rev;
            reload();
        } else if (st_->pgm.list_no.load() != playingList_ || st_->pgm.list_pos.load() != playingPos_) {
            markPlaying();
        }
    }

    QLabel* status_ = nullptr;
    QLabel* reply_ = nullptr;
    QLabel* alert_ = nullptr;
    QTimer* alertTimer_ = nullptr;
    uint32_t lastAlert_ = 0;
    bool alertInit_ = false;
    QLineEdit* name_ = nullptr;
    QTableWidget* clips_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    std::array<QTableWidget*, NUM_LISTS + 1> lists_{};
    QDoubleSpinBox* speed_ = nullptr;
    QComboBox* target_ = nullptr;
    std::vector<ClipMeta> clipMeta_;
    SharedPlayoutState* st_ = nullptr;
    int sock_ = -1;
    uint64_t lastRev_ = UINT64_MAX, lastHb_ = 0;
    int hbClock_ = 0, copyPoll_ = 0;
    int playingList_ = -1, playingPos_ = -1;
    std::pair<int, int> pendingSelect_{0, 0};
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    ensure_clip_dirs();
    Control w;
    w.show();
    // Testhilfe:  evs-control --snapshot bild.png   speichert nach 3 s ein Bild des Fensters und beendet sich
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--snapshot") == 0) {
            const QString file = QString::fromUtf8(argv[i + 1]);
            QTimer::singleShot(3000, &app, [&w, &app, file] { w.grab().save(file); app.quit(); });
        }
    return app.exec();
}

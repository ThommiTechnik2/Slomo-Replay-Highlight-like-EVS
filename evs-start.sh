#!/bin/bash
# evs-start.sh — startet Ingest, Playout, Monitor, Clip-Fenster und Controller-Bridge.
# Aufruf:   evs-start.sh 6     (6 REC-Wege, Standard)
#           evs-start.sh 4     (4 REC-Wege)
# Für den Doppelklick gibt es evs-start-4.sh und evs-start-6.sh.
# Ingest, Playout und Controller bekommen je ein eigenes Terminalfenster, Monitor und Clip-Fenster
# sind normale Fenster.

# Kameras kommen per SRT: Der Ingest lauscht auf den Ports 20010 … 20060, OBS ruft an
# (in OBS: srt://IP_DIESES_RECHNERS:20010?latency=200000).
# Oder per SDI über eine Blackmagic DeckLink (REC n = Anschluss n-1): evs-start-sdi.sh bzw. INPUT unten.

# === HIER ANPASSEN ===
BIN="$HOME/evs/build"                        # wo evs-ingest, evs-playout, evs-monitor liegen
RUN="/media/slomo/slomo/evs-run"          # Ordner auf der Festplatte (darin entsteht replay_storage)
PLAYOUT_MODE=1                               # 0 = Fenster, 1 = SRT-Rückweg PGM (Port 20100), 2 = DeckLink, 3 = nur Monitor
# Namen der Kameras im Multiview (durch Semikolon getrennt, REC 1 bis 6; leer lassen = kein Name)
# Quelle der REC-Wege:  srt = alle per SRT (OBS)   sdi = alle per SDI (DeckLink, REC 1 = Anschluss 0 …)
#   test = Testbild + Ton (zum Ausprobieren ohne Kamera)
#   oder pro REC einzeln, z.B. "sdi,sdi,sdi,sdi,srt,srt"  oder  "sdi:2,sdi:3,..." (bestimmte Anschlüsse)
INPUT="${EVS_INPUT:-srt}"
HW="${EVS_HW:-}"                             # leer = automatisch (NVIDIA, dann Intel, dann Software); nv | va | sw erzwingt
SDI_MODE="1080p50"                           # Videomodus der SDI-Eingänge (progressiv, z.B. 1080p50)
NAMES="Mitte;16er Hoch;Tor links;Tor rechts;Halbtotale;Spielfeld"
CONTROLLER_DIR="$HOME/evs/Controller"        # dort liegt evs-jlcooper.js (leer lassen = Controller nicht starten)
# =====================

REC="${1:-6}"
case "$REC" in 4|6) ;; *) echo "Aufruf: $0 [4|6]"; exit 1 ;; esac

fehler() {
    zenity --error --title="EVS Start" --text="$1" 2>/dev/null || echo "$1"
    exit 1
}

# Läuft schon etwas? Dann nicht doppelt starten (die SRT-Ports wären belegt).
if pgrep -x evs-ingest >/dev/null || pgrep -x evs-playout >/dev/null; then
    fehler "EVS läuft bereits. Bitte zuerst beenden (Ingest: Strg+C, Playout: q)."
fi

[ -d "$RUN" ] || fehler "Ordner nicht gefunden:\n$RUN\n\nIst die Platte eingehängt?"
[ -x "$BIN/evs-ingest" ] || fehler "Programme nicht gefunden in:\n$BIN\n\nWurde gebaut?"

# Alte Index-/Zustandsbereiche im Arbeitsspeicher entfernen (die Videodateien bleiben, sie werden
# vom Ring einfach überschrieben; neu anlegen bei jedem Start ist nicht nötig).
rm -f /dev/shm/evs_replay_index /dev/shm/evs_playout_state

# Jedes Terminal bleibt nach dem Beenden offen, damit eventuelle Fehlermeldungen lesbar sind.
HALTEN='echo; read -rp "Beendet – Enter schließt das Fenster"'

gnome-terminal --title="EVS Ingest" --working-directory="$RUN" -- \
    bash -c "export EVS_HW=\"$HW\"; \"$BIN/evs-ingest\" $REC --input \"$INPUT\" --sdi-mode $SDI_MODE; $HALTEN"

sleep 3   # Ingest muss den Index angelegt haben, bevor Playout und Monitor starten

gnome-terminal --title="EVS Playout" --working-directory="$RUN" -- \
    bash -c "export EVS_HW=\"$HW\"; \"$BIN/evs-playout\" $PLAYOUT_MODE; $HALTEN"

sleep 1

# Controller-Bridge (JLCooper SloMo Mini), falls vorhanden
if [ -n "$CONTROLLER_DIR" ] && [ -f "$CONTROLLER_DIR/evs-jlcooper.js" ]; then
    gnome-terminal --title="EVS Controller" --working-directory="$CONTROLLER_DIR" -- \
        bash -c "node evs-jlcooper.js; $HALTEN"
fi

cd "$RUN" && EVS_HW="$HW" setsid "$BIN/evs-monitor" --names "$NAMES" >/dev/null 2>&1 &

# Clip- und Listenfenster (für den zweiten Bildschirm)
cd "$RUN" && setsid "$BIN/evs-control" >/dev/null 2>&1 &

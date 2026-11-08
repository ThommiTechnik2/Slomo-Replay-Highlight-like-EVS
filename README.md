# EVS-Replay (Eigenbau) · Self-built EVS-style Replay System

**Deutsch** · [English](#english)

---

# Deutsch

Ein selbstgebautes Mehrkamera-Replay-System nach dem Vorbild der EVS (LSM-VIA) für Linux. Es nimmt bis zu 6 Kameras (1080p50, All-Intra H.264 + AAC-Stereo, per SRT von OBS) in einem 1-Stunden-Ring auf, spielt PGM/PVW in Zeitlupe, Rückwärts und bildgenau ab, speichert Clips mit allen Winkeln, verwaltet 10 Listen und wird mit dem JLCooper SloMo Mini bedient.

## Inhalt

1. [Überblick](#überblick)
2. [Voraussetzungen](#voraussetzungen)
3. [Bauen](#bauen)
4. [OBS einrichten](#obs-einrichten)
5. [Starten](#starten)
6. [Bedienung](#bedienung)
7. [Controller-Belegung (JLCooper SloMo Mini)](#controller-belegung-jlcooper-slomo-mini)
8. [Clip-Nummern im EVS-Schema](#clip-nummern-im-evs-schema)
9. [Der Monitor](#der-monitor)
10. [Dateien und Speicher](#dateien-und-speicher)
11. [Befehle (UDP)](#befehle-udp)
12. [Hinweise und Fehlersuche](#hinweise-und-fehlersuche)
13. [Bekannte Grenzen](#bekannte-grenzen)

## Überblick

| Programm | Aufgabe |
|---|---|
| `evs-ingest` | Empfängt die Kameras per SRT (Listener auf den Ports 20010 bis 20060), schreibt Bild und Ton in je einen Ring pro Kamera auf der Platte und führt den 50-Hz-Master-Takt. |
| `evs-playout` | Spielt PGM und PVW aus Ring, Clips oder Listen. Bild und Ton laufen mit Zeitlupe und Rückwärts. PGM geht optional per SRT als Rückweg zu OBS. Nimmt Befehle per UDP an. |
| `evs-monitor` | Multiview (Qt6): PGM, PVW und alle Kameras, Kamerabeschriftung, Warnungen, Log der Messwerte. |
| `evs-control` | Fenster "Clips & Listen": Clips verwalten, 10 Listen, Marken, Take, Play/Stop, rotes Fehlerbanner. |
| `Controller/evs-jlcooper.js` | Brücke vom JLCooper SloMo Mini (USB) zum Playout (UDP), inklusive Display. |

Die Programme tauschen Daten über Shared Memory (`/dev/shm`) und Dateien aus. Der Controller und das Clip-Fenster sprechen mit dem Playout über UDP auf `127.0.0.1:20200`.

## Voraussetzungen

- Linux (getestet: Linux Mint), Rechner mit Intel-Grafik für Quick Sync (Dekodierung und Rückweg-Encoder), sonst läuft alles auch in Software (langsamer).
- Schnelle NVMe für den Ring (rund 70 MB/s Schreiblast bei 6 Kameras, etwa 250 GB pro Stunde Betrieb).
- Display mit **50 Hz** (sonst ruckeln PGM und PVW).
- Pakete (typisch, Namen je nach Distribution): `build-essential cmake pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-vaapi qt6-base-dev intel-media-va-driver zenity nodejs npm`
- `gstreamer1.0-libav` wird für den Ton (AAC) benötigt; fehlt es, warnt der Ingest und nimmt nur Bild auf.
- Node.js 18 oder neuer für die Controller-Brücke.

## Bauen

```bash
cd ~/evs
mkdir -p build && cd build
cmake ..
make
```

Es entstehen `evs-ingest`, `evs-playout`, `evs-monitor` und `evs-control`.

Controller-Brücke einrichten (einmalig):

```bash
cd ~/evs/Controller
npm install                       # installiert das Modul "usb"
# Zugriffsrechte für den USB-Controller (ohne sudo):
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0760", ATTR{idProduct}=="0005", MODE="0666"' | sudo tee /etc/udev/rules.d/99-jlcooper.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
# Controller danach einmal abziehen und wieder einstecken
```

## OBS einrichten

**Kameras zum Replay (pro Kamera ein Ausgang):**

- Ziel: `srt://IP_DES_REPLAY_RECHNERS:20010?latency=200000` (Kamera 1), `…:20020` (Kamera 2) usw. bis `…:20060`. OBS ruft an, das Replay lauscht.
- Encoder: x264, `ultrafast`, Tune `zerolatency`, CBR rund 100000 kbit/s, Keyframe-Intervall so, dass **jedes Bild ein Keyframe** ist (`keyint=1`), Profil High, **keine B-Frames**.
- Ton: AAC Stereo (Kanal 1/2 der Kamera oder Stereo-Mix). Kanäle 3 bis 8 werden nicht aufgezeichnet.
- Hinweis: Der Apple-VT-Encoder kann den Keyframe-Abstand nur in Sekunden einstellen und ist dafür ungeeignet.

**Rückweg (PGM zu OBS):**

- Das Replay (`evs-playout` im Modus 1) ist **Listener** auf Port 20100, OBS ist **Caller**: `srt://IP_DES_REPLAY_RECHNERS:20100`.
- In OBS **keine Hardware-Dekodierung** für die Rückweg-Quelle einschalten.
- Der Rückweg-Encoder schickt keine B-Frames und nur ein Referenzbild (Quick Sync, sonst x264).

**Bei 4 statt 6 Wegen:** In OBS auch die Ausgänge 5 und 6 ausschalten. Sendet OBS weiter an Ports, auf denen niemand lauscht, versucht es dauernd, neu zu verbinden, und das kann OBS so belasten, dass auch die laufenden Streams unruhig werden.

## Starten

Doppelklick auf eine der Dateien (oder im Terminal):

```bash
./evs-start-6.sh        # 6 REC-Wege
./evs-start-4.sh        # 4 REC-Wege
./evs-start.sh 4        # dasselbe mit Parameter
```

Das Skript startet nacheinander Ingest (eigenes Terminal), Playout (eigenes Terminal), Controller-Brücke (eigenes Terminal), Monitor und Clip-Fenster.

Einstellungen am Anfang von `evs-start.sh`:

| Variable | Bedeutung |
|---|---|
| `BIN` | Ordner mit den gebauten Programmen (z. B. `~/evs/build`) |
| `RUN` | Arbeitsordner auf der schnellen Platte; hier entstehen `replay_storage`, `clips`, `lists`, `monitor-log.txt` |
| `PLAYOUT_MODE` | 0 = Fenster, 1 = SRT-Rückweg PGM (Port 20100), 2 = DeckLink (ungetestet), 3 = nur Monitor |
| `NAMES` | Kameranamen für den Multiview, durch Semikolon getrennt, REC 1 bis 6, z. B. `"Mitte;16er Hoch;Tor links;Tor rechts;Halbtotale;Spielfeld"` |
| `CONTROLLER_DIR` | Ordner der Controller-Brücke (leer lassen = Brücke nicht starten) |

Beenden: Ingest mit Strg+C, Playout mit `q`. Alle drei Programme sollten nach einem Neustart des Ingest ebenfalls neu gestartet werden (das Skript räumt den Shared Memory dazu auf).

Der Ring wird bei jedem Start des Ingest neu angelegt, die Aufnahme der vorigen Sitzung steht danach nicht mehr zur Verfügung. Gespeicherte Clips bleiben erhalten.

## Bedienung

**Tastatur im Playout-Fenster (zum Testen):**

| Taste | Funktion |
|---|---|
| `1`–`6` | REC auf PVW wählen |
| `Shift+1`–`6` | REC direkt auf PGM schalten |
| `Leertaste` | TAKE (PVW nach PGM; läuft eine Liste: nächster Eintrag) |
| `g` | GANG-Modus (PGM und PVW koppeln) |
| `j` / `k` / `l` | PVW rückwärts / Stand / Play |
| `,` / `.` | PVW ein Bild zurück / vor |
| `s` `d` `f` `p` `x` | PGM Tempo 25 / 50 / 75 / 100 % / Stopp |
| `b` | PVW 5 Sekunden hinter Live |
| `i` `u` `o` | IN setzen / OUT setzen / zum IN springen |
| `c` | Clip aus IN/OUT speichern |
| `e` | PGM zurück zu Live |
| `a` | Ton: Varispeed / nur bei 100 % |
| `q` | Beenden |

**Clip-Fenster:** Marken setzen, Clip speichern, Clips laden, 10 Listen (Reiter), Einträge hinzufügen, verschieben, kopieren, Tempo je Eintrag, Liste auf PGM cuen und abspielen. Fehler erscheinen als rotes Banner oben im Fenster.

**Listen:** Ein Eintrag besteht aus Clip-Nummer und Tempo. Die Wiedergabe schneidet hart von Clip zu Clip. Beim Wechsel zum nächsten Eintrag gilt wieder dessen Tempo.

## Controller-Belegung (JLCooper SloMo Mini)

`SHIFT` bedeutet: SHIFT-Taste gehalten.

| Taste | Funktion |
|---|---|
| **CAM A–D** | REC 1–4 auf PVW (mit SHIFT: A = REC 5, B = REC 6) |
| **3 Ziffern + CAM A–D** | Clip laden im gewählten Winkel (z. B. `1 1 2` + CAM B = Clip 112, REC 2) |
| **3 Ziffern + ENTER** | Clip laden im gespeicherten Winkel |
| **CLR** | Ziffern löschen |
| **MARK IN / MARK OUT** | Marken auf PVW (SHIFT + MARK IN: beide löschen) |
| **STORE CUE** | Clip speichern (nächste freie Nummer auf aktueller Seite/Bank) |
| **REPLAY** | TAKE (PVW nach PGM, startet mit dem T-Bar-Tempo; läuft PVW schon, bleibt dessen Tempo) |
| **PLAY / STOP** | PVW starten / anhalten (läuft eine Liste auf PGM: PGM). SHIFT + PLAY: aktive Liste cuen. SHIFT + STOP: PGM anhalten |
| **REVERSE PLAY** | PVW rückwärts |
| **F-FWD** | PVW zurück an die Live-Kante |
| **REWIND** | PVW zum IN-Punkt |
| **MANUAL TIME** | PVW 5 s hinter Live (Cue) |
| **LAST CUE / NEXT CUE** | vorigen / nächsten Clip laden |
| **INSERT** | Clip auf PVW an die aktive Liste anhängen |
| **V/TC** | nächste Liste (SHIFT: vorige), 1–10 im Kreis |
| **W1 + Ziffer** | Liste direkt wählen (1–9, 0 = Liste 10) |
| **A1 / A2** | Seite zurück / vor (SHIFT: Bank zurück / vor) |
| **PRESET** | Ton: Varispeed / nur bei 100 % umschalten |
| **CYCLE PLAYER** | GANG-Modus umschalten |
| **JOG ON/OFF** | Jog-Rad sperren / freigeben |
| **Jog-Rad** | PVW bildweise |
| **T-Bar** | Tempo 5–100 % des laufenden Kanals: PVW, wenn dort PLAY läuft, sonst PGM, wenn es läuft. Steht der Kanal, startet der T-Bar nichts. |
| ENABLE, SCALED, W2, FRAME ±1 | frei bzw. wie im Code beschrieben (FRAME ±1: ein Bild zurück / vor) |

Das Display zeigt in Zeile 1 Seite, Bank, aktive Liste und die nächste freie Nummer, in Zeile 2 Winkel (A–F), Clip oder LIVE, Marken und Tempo. Rückmeldungen (z. B. `112B gespeichert`) und kurze Fehler (z. B. `BANK VOLL`) erscheinen für einige Sekunden.

Mit `node evs-jlcooper.js --debug` siehst du jeden Tastendruck, jede Bewegung von T-Bar und Jog und jeden gesendeten Befehl.

## Clip-Nummern im EVS-Schema

Die Clip-Nummer besteht aus **Seite, Bank und Position**, jeweils 1 bis 9: Clip `112` ist Seite 1, Bank 1, Position 2. Damit gibt es 729 Plätze.

- Der Winkel gehört nicht zur Nummer: Jeder Clip enthält alle Winkel derselben Zeitspanne. Den Winkel wählst du beim Aufruf (CAM A–D, SHIFT + A/B für REC 5/6). Mit ENTER lädt der Clip im Winkel, der beim Speichern auf PVW lag.
- STORE CUE legt den Clip auf die nächste freie Position der aktuellen Seite und Bank. Ist die Bank voll, erscheint `BANK VOLL`, und du wählst mit A1/A2 eine andere Seite oder Bank.
- Das Clip-Fenster zeigt dieselben Nummern.
- Clips mit alten Nummern (1, 2, 3 …, aus Tests) bleiben nutzbar, passen aber nicht ins Schema und lassen sich im Clip-Fenster löschen.

## Der Monitor

- Oben PGM und PVW, darunter die REC-Kacheln (6 Wege: 2 Reihen mit je 3, 4 Wege: eine Reihe mit 4). Jede Kachel ist exakt 16:9.
- Rot = Kamera auf PGM, Grün = Kamera auf PVW.
- Kamerakacheln im EVS-Stil: Titel oben links, Zeit unten links, Kameraname unten mittig, Punkt oben rechts (rot = Signal ok, orange = Warnung, grau = kein Signal).
- Bei Problemen (unter 45 fps oder Lücken im 60-s-Fenster) erscheint zusätzlich eine orange Warnzeile.
- Alle Messwerte (fps, Lücken, Ring, Anker) stehen in `monitor-log.txt` im Arbeitsordner: jede Änderung sofort, eine Übersicht jede Minute.
- Parameter: `--names "A;B;…"`, `--cam-fps 25|50|10|5` (Standard 25), `--big-fps 50|25|10|5` (Standard 50), `--no-hw` (Software-Dekodierung erzwingen), `--snapshot bild.png` (Testbild nach 6 s).
- `F11` schaltet Vollbild um.

**Begriffe:**

- **Lücken:** Bilder, die im Ring fehlen (der Ingest hat zu diesem Zeitpunkt kein Bild geschrieben). Idealerweise 0.
- **Anker:** Wie oft der Ingest die Zeitzuordnung einer Kamera nachgezogen hat. 1 ist der Normalzustand (Erstanker); wenige zusätzliche Korrekturen pro Stunde sind unkritisch.
- **Ring G/%:** Umlauf (G) und Füllstand (%) der Ringdatei.

## Dateien und Speicher

Im Arbeitsordner `RUN`:

| Pfad | Inhalt |
|---|---|
| `replay_storage/cam_N.raw` | Video-Ring der Kamera N (N = 0…5), rund 67,5 GB je Kamera bei 1 Stunde und angenommenen 120 Mbit/s plus Reserve |
| `replay_storage/cam_N.aud` | Ton-Ring (Stereo, 32 Bit, 48 kHz), rund 1,4 GB je Kamera |
| `clips/clip_NNNN/` | gespeicherter Clip: `clip.ini`, je Kamera `.h264`, `.idx`, `.pcm` |
| `lists/list_01.txt … list_10.txt` | Listen, eine Zeile `<Clip-Nr> <Tempo>` |
| `monitor-log.txt` | Messwerte des Monitors |

- Der Index im Arbeitsspeicher (`/dev/shm/evs_replay_index`) fasst eine Stunde; die Ringdatei wird bei 92 Mbit/s nach rund 98 Minuten einmal überschrieben.
- Der Ring braucht bei 6 Kameras rund 405 GB plus Ton und Clips. Die Größe lässt sich in `evs-common.h` anpassen (`BUFFER_SECONDS`, `ASSUMED_MAX_MBIT_PER_S`).
- Kameras 5 und 6 sind bei einem Start mit 4 Wegen nicht aktiv. Clips aus einem 4-Wege-Betrieb haben nur 4 Winkel.

## Befehle (UDP)

Textzeilen an `127.0.0.1:20200`, die Antwort kommt an den Absender zurück. Mehrere Zeilen in einem Paket werden nacheinander ausgeführt. `PGM`/`PVW` ist optional, sonst gilt der Standard des Befehls.

| Befehl | Wirkung |
|---|---|
| `CAM [PGM\|PVW] <1-6>` | REC wählen |
| `SPEED [PGM\|PVW] <-4..4>` | Tempo (1.0 = 100 %, negativ = rückwärts) |
| `PLAY [PGM]` / `STOP [PGM]` | Wiedergabe starten / anhalten |
| `STEP [PVW] <n>` | n Bilder (negativ = zurück) |
| `TAKE` | PVW nach PGM, bei laufender Liste nächster Eintrag |
| `GANG` | Kopplung umschalten |
| `LIVE [PGM\|PVW]` | an die Live-Kante |
| `JUMP [PVW] <s>` | Live minus s Sekunden, stehend |
| `MARKIN` / `MARKOUT` / `CLEARMARKS` / `GOTOIN` / `GOTOOUT` | Marken |
| `SAVECLIP [PGM\|PVW] [Name]` | Clip speichern |
| `RECALL <Nr> [PGM\|PVW] [SAVED\|1-6]` | Clip laden (optional Winkel) |
| `RENAME <Nr> <Name>` / `DELCLIP <Nr>` | Clip umbenennen / löschen |
| `PAGE <1-9\|+\|->` / `BANK <1-9\|+\|->` | Seite / Bank wählen |
| `LISTADD <Liste> <Nr> [Pos] [Tempo]` | Clip an Liste anhängen / einfügen |
| `LISTDEL` / `LISTMOVE` / `LISTXFER` / `LISTSPEED` / `LISTCLEAR` | Listen bearbeiten |
| `LISTCUE <Liste> [Pos]` | Liste auf PGM bereitstellen (stehend), `PLAY` startet |
| `AUDIO` | Ton Varispeed / nur bei 100 % |
| `STATUS` | Zustand als eine Zeile `key=value` |

## Hinweise und Fehlersuche

| Problem | Hinweis |
|---|---|
| `permission denied` beim Controller | udev-Regel anlegen (siehe [Bauen](#bauen)) und Controller neu einstecken. Hält der Kernel-Treiber `ftdi_sio` das Gerät, hilft `sudo rmmod ftdi_sio`. |
| `module usb not found` | Im Ordner `Controller` `npm install` ausführen (Datei `package.json` muss vorhanden sein). |
| Keine Verbindung von OBS | Firewall prüfen, IP des Replay-Rechners und Ports 20010–20060 (SRT/UDP). Der Ingest ist Listener, OBS ruft an. |
| Kein Ton | `gstreamer1.0-libav` installieren, Ingest neu starten. |
| PGM/PVW ruckeln | Display auf 50 Hz stellen (`xrandr`, der Ausgang heißt z. B. `DP-1`). |
| Hohe CPU-Last oder Temperatur | Quick Sync nutzen (`intel-media-va-driver`), Monitor mit `--cam-fps 10`, Maximaltakt der CPU begrenzen (`cpupower`). Dauerhaft über 90 °C vermeiden. |
| Viele Lücken oder Anker | Last prüfen, auf der OBS-Seite Encoder-Last und nicht genutzte Ausgänge prüfen, Netzwerk prüfen, Log (`monitor-log.txt`) auswerten. |
| Rückweg ruckelt | In OBS keine Hardware-Dekodierung, Rückweg ohne B-Frames (ist im Playout so eingestellt). |

## Bekannte Grenzen

- DeckLink-Ausgang (`PLAYOUT_MODE=2`) ist nicht getestet.
- Listen schneiden hart zwischen Clips (keine Blenden).
- Keine Portierung auf macOS (geplant).
- Der T-Bar meldet seine Position erst bei der ersten Bewegung. Bis dahin gilt 100 %.
- Der Ring wird beim Start des Ingest neu angelegt.

---

# English

<a name="english"></a>

A self-built multi-camera replay system modeled after EVS (LSM-VIA) for Linux. It records up to 6 cameras (1080p50, All-Intra H.264 + AAC stereo, delivered by OBS over SRT) into a 1-hour ring, plays PGM/PVW in slow motion, in reverse and frame-accurately, saves clips with all angles, manages 10 lists and is operated with the JLCooper SloMo Mini.

## Contents

1. [Overview](#overview)
2. [Requirements](#requirements)
3. [Build](#build)
4. [OBS setup](#obs-setup)
5. [Starting](#starting)
6. [Operation](#operation)
7. [Controller mapping (JLCooper SloMo Mini)](#controller-mapping-jlcooper-slomo-mini)
8. [EVS-style clip numbers](#evs-style-clip-numbers)
9. [The monitor](#the-monitor)
10. [Files and storage](#files-and-storage)
11. [Commands (UDP)](#commands-udp)
12. [Notes and troubleshooting](#notes-and-troubleshooting)
13. [Known limitations](#known-limitations)

## Overview

| Program | Purpose |
|---|---|
| `evs-ingest` | Receives the cameras over SRT (listener on ports 20010 to 20060), writes video and audio into one ring per camera on disk and runs the 50 Hz master clock. |
| `evs-playout` | Plays PGM and PVW from the ring, clips or lists. Video and audio support slow motion and reverse. PGM can optionally be returned to OBS over SRT. Accepts commands over UDP. |
| `evs-monitor` | Multiview (Qt6): PGM, PVW and all cameras, camera labels, warnings, log of the measurements. |
| `evs-control` | "Clips & Lists" window: manage clips, 10 lists, marks, take, play/stop, red error banner. |
| `Controller/evs-jlcooper.js` | Bridge from the JLCooper SloMo Mini (USB) to the playout (UDP), including the display. |

The programs exchange data through shared memory (`/dev/shm`) and files. The controller and the clip window talk to the playout over UDP on `127.0.0.1:20200`.

## Requirements

- Linux (tested on Linux Mint). An Intel GPU is recommended for Quick Sync (decoding and the return encoder); everything also runs in software, just slower.
- Fast NVMe for the ring (about 70 MB/s write load with 6 cameras, roughly 250 GB per hour of operation).
- A display running at **50 Hz** (otherwise PGM and PVW will judder).
- Packages (typical, names vary by distribution): `build-essential cmake pkg-config libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-vaapi qt6-base-dev intel-media-va-driver zenity nodejs npm`
- `gstreamer1.0-libav` is needed for audio (AAC); if it is missing, the ingest warns and records video only.
- Node.js 18 or newer for the controller bridge.

## Build

```bash
cd ~/evs
mkdir -p build && cd build
cmake ..
make
```

This produces `evs-ingest`, `evs-playout`, `evs-monitor` and `evs-control`.

Set up the controller bridge (once):

```bash
cd ~/evs/Controller
npm install                       # installs the "usb" module
# Access rights for the USB controller (without sudo):
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0760", ATTR{idProduct}=="0005", MODE="0666"' | sudo tee /etc/udev/rules.d/99-jlcooper.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
# Then unplug and re-plug the controller once
```

## OBS setup

**Cameras to the replay (one output per camera):**

- Target: `srt://IP_OF_THE_REPLAY_MACHINE:20010?latency=200000` (camera 1), `…:20020` (camera 2), and so on up to `…:20060`. OBS is the caller, the replay listens.
- Encoder: x264, `ultrafast`, tune `zerolatency`, CBR about 100000 kbit/s, keyframe interval so that **every frame is a keyframe** (`keyint=1`), profile High, **no B-frames**.
- Audio: AAC stereo (channels 1/2 of the camera or a stereo mix). Channels 3 to 8 are not recorded.
- Note: The Apple VT encoder only lets you set the keyframe interval in seconds and is therefore unsuitable.

**Return path (PGM to OBS):**

- The replay (`evs-playout` in mode 1) is the **listener** on port 20100, OBS is the **caller**: `srt://IP_OF_THE_REPLAY_MACHINE:20100`.
- In OBS, **do not enable hardware decoding** for the return source.
- The return encoder sends no B-frames and only one reference frame (Quick Sync, otherwise x264).

**With 4 instead of 6 channels:** Also switch off outputs 5 and 6 in OBS. If OBS keeps sending to ports nobody is listening on, it constantly tries to reconnect, which can load OBS enough to make the running streams unstable.

## Starting

Double-click one of these files (or run in a terminal):

```bash
./evs-start-6.sh        # 6 REC channels
./evs-start-4.sh        # 4 REC channels
./evs-start.sh 4        # same with a parameter
```

The script starts the ingest (own terminal), playout (own terminal), controller bridge (own terminal), monitor and clip window one after another.

Settings at the top of `evs-start.sh`:

| Variable | Meaning |
|---|---|
| `BIN` | Folder with the built programs (e.g. `~/evs/build`) |
| `RUN` | Working folder on the fast disk; `replay_storage`, `clips`, `lists` and `monitor-log.txt` are created here |
| `PLAYOUT_MODE` | 0 = window, 1 = SRT return PGM (port 20100), 2 = DeckLink (untested), 3 = monitor only |
| `NAMES` | Camera names for the multiview, separated by semicolons, REC 1 to 6, e.g. `"Center;High 16;Goal left;Goal right;Wide;Pitch"` |
| `CONTROLLER_DIR` | Folder of the controller bridge (leave empty to not start the bridge) |

To stop: Ctrl+C in the ingest, `q` in the playout. After restarting the ingest, restart all three programs (the script cleans up the shared memory for this).

The ring is re-created each time the ingest starts, so the recording of the previous session is no longer available. Saved clips are kept.

## Operation

**Keyboard in the playout window (for testing):**

| Key | Function |
|---|---|
| `1`–`6` | Select REC on PVW |
| `Shift+1`–`6` | Switch REC directly to PGM |
| `Space` | TAKE (PVW to PGM; if a list is running: next entry) |
| `g` | GANG mode (couple PGM and PVW) |
| `j` / `k` / `l` | PVW reverse / stop / play |
| `,` / `.` | PVW one frame back / forward |
| `s` `d` `f` `p` `x` | PGM speed 25 / 50 / 75 / 100 % / stop |
| `b` | PVW 5 seconds behind live |
| `i` `u` `o` | Set IN / set OUT / go to IN |
| `c` | Save clip from IN/OUT |
| `e` | PGM back to live |
| `a` | Audio: varispeed / only at 100 % |
| `q` | Quit |

**Clip window:** set marks, save clips, load clips, 10 lists (tabs), add, move and copy entries, tempo per entry, cue a list to PGM and play it. Errors appear as a red banner at the top of the window.

**Lists:** An entry consists of a clip number and a speed. Playback cuts hard from clip to clip. When moving to the next entry, that entry's own speed applies again.

## Controller mapping (JLCooper SloMo Mini)

`SHIFT` means: SHIFT key held.

| Key | Function |
|---|---|
| **CAM A–D** | REC 1–4 to PVW (with SHIFT: A = REC 5, B = REC 6) |
| **3 digits + CAM A–D** | Load clip in the chosen angle (e.g. `1 1 2` + CAM B = clip 112, REC 2) |
| **3 digits + ENTER** | Load clip in its saved angle |
| **CLR** | Clear digits |
| **MARK IN / MARK OUT** | Marks on PVW (SHIFT + MARK IN: clear both) |
| **STORE CUE** | Save clip (next free number on the current page/bank) |
| **REPLAY** | TAKE (PVW to PGM, starts at the T-bar speed; if PVW is already running it keeps its speed) |
| **PLAY / STOP** | Start / stop PVW (if a list runs on PGM: PGM). SHIFT + PLAY: cue the active list. SHIFT + STOP: stop PGM |
| **REVERSE PLAY** | PVW in reverse |
| **F-FWD** | PVW back to the live edge |
| **REWIND** | PVW to the IN point |
| **MANUAL TIME** | PVW 5 s behind live (cue) |
| **LAST CUE / NEXT CUE** | Load previous / next clip |
| **INSERT** | Append the clip on PVW to the active list |
| **V/TC** | Next list (SHIFT: previous), 1–10 in a loop |
| **W1 + digit** | Select list directly (1–9, 0 = list 10) |
| **A1 / A2** | Page back / forward (SHIFT: bank back / forward) |
| **PRESET** | Audio: toggle varispeed / only at 100 % |
| **CYCLE PLAYER** | Toggle GANG mode |
| **JOG ON/OFF** | Lock / unlock the jog wheel |
| **Jog wheel** | PVW frame by frame |
| **T-bar** | Speed 5–100 % of the running channel: PVW if PLAY is running there, otherwise PGM if it is running. If the channel is stopped, the T-bar does not start anything. |
| ENABLE, SCALED, W2, FRAME ±1 | Free or as described in the code (FRAME ±1: one frame back / forward) |

The display shows page, bank, active list and the next free number in line 1, and angle (A–F), clip or LIVE, marks and speed in line 2. Feedback (e.g. `112B gespeichert`) and short errors (e.g. `BANK VOLL`) appear for a few seconds. (The display texts are in German.)

With `node evs-jlcooper.js --debug` you see every key press, every movement of the T-bar and jog wheel, and every command that is sent.

## EVS-style clip numbers

A clip number consists of **page, bank and position**, each from 1 to 9: clip `112` is page 1, bank 1, position 2. That gives 729 slots.

- The angle is not part of the number: every clip contains all angles of the same time span. You choose the angle when recalling (CAM A–D, SHIFT + A/B for REC 5/6). With ENTER the clip loads in the angle that was on PVW when it was saved.
- STORE CUE puts the clip on the next free position of the current page and bank. If the bank is full, `BANK VOLL` appears and you choose another page or bank with A1/A2.
- The clip window shows the same numbers.
- Clips with old numbers (1, 2, 3 … from tests) stay usable, but do not fit the scheme and can be deleted in the clip window.

## The monitor

- PGM and PVW on top, the REC tiles below (6 channels: 2 rows of 3; 4 channels: one row of 4). Every tile is exactly 16:9.
- Red = camera on PGM, green = camera on PVW.
- Camera tiles in EVS style: title top left, time bottom left, camera name bottom center, dot top right (red = signal ok, orange = warning, grey = no signal).
- On problems (below 45 fps or gaps in the 60 s window) an orange warning line is added.
- All measurements (fps, gaps, ring, anchors) are written to `monitor-log.txt` in the working folder: every change immediately, a summary every minute.
- Parameters: `--names "A;B;…"`, `--cam-fps 25|50|10|5` (default 25), `--big-fps 50|25|10|5` (default 50), `--no-hw` (force software decoding), `--snapshot image.png` (test image after 6 s).
- `F11` toggles fullscreen.

**Terms:**

- **Gaps:** Frames missing in the ring (the ingest wrote no frame for that moment). Ideally 0.
- **Anchor:** How often the ingest re-adjusted a camera's time assignment. 1 is the normal state (initial anchor); a few additional corrections per hour are harmless.
- **Ring G/%:** Wrap count (G) and fill level (%) of the ring file.

## Files and storage

In the working folder `RUN`:

| Path | Content |
|---|---|
| `replay_storage/cam_N.raw` | Video ring of camera N (N = 0…5), about 67.5 GB per camera for 1 hour at an assumed 120 Mbit/s plus headroom |
| `replay_storage/cam_N.aud` | Audio ring (stereo, 32 bit, 48 kHz), about 1.4 GB per camera |
| `clips/clip_NNNN/` | Saved clip: `clip.ini`, per camera `.h264`, `.idx`, `.pcm` |
| `lists/list_01.txt … list_10.txt` | Lists, one line `<clip no.> <speed>` |
| `monitor-log.txt` | Monitor measurements |

- The index in memory (`/dev/shm/evs_replay_index`) holds one hour; at 92 Mbit/s the ring file is overwritten once after about 98 minutes.
- With 6 cameras the ring needs about 405 GB plus audio and clips. The size can be adjusted in `evs-common.h` (`BUFFER_SECONDS`, `ASSUMED_MAX_MBIT_PER_S`).
- Cameras 5 and 6 are not active when started with 4 channels. Clips from a 4-channel session only have 4 angles.

## Commands (UDP)

Text lines to `127.0.0.1:20200`; the reply goes back to the sender. Several lines in one packet are executed in order. `PGM`/`PVW` is optional, otherwise the command's default applies.

| Command | Effect |
|---|---|
| `CAM [PGM\|PVW] <1-6>` | Select REC |
| `SPEED [PGM\|PVW] <-4..4>` | Speed (1.0 = 100 %, negative = reverse) |
| `PLAY [PGM]` / `STOP [PGM]` | Start / stop playback |
| `STEP [PVW] <n>` | n frames (negative = back) |
| `TAKE` | PVW to PGM, next entry if a list is running |
| `GANG` | Toggle coupling |
| `LIVE [PGM\|PVW]` | To the live edge |
| `JUMP [PVW] <s>` | Live minus s seconds, stopped |
| `MARKIN` / `MARKOUT` / `CLEARMARKS` / `GOTOIN` / `GOTOOUT` | Marks |
| `SAVECLIP [PGM\|PVW] [name]` | Save clip |
| `RECALL <no.> [PGM\|PVW] [SAVED\|1-6]` | Load clip (optionally with angle) |
| `RENAME <no.> <name>` / `DELCLIP <no.>` | Rename / delete clip |
| `PAGE <1-9\|+\|->` / `BANK <1-9\|+\|->` | Select page / bank |
| `LISTADD <list> <no.> [pos] [speed]` | Append / insert clip into a list |
| `LISTDEL` / `LISTMOVE` / `LISTXFER` / `LISTSPEED` / `LISTCLEAR` | Edit lists |
| `LISTCUE <list> [pos]` | Prepare a list on PGM (stopped), `PLAY` starts it |
| `AUDIO` | Audio varispeed / only at 100 % |
| `STATUS` | State as one `key=value` line |

## Notes and troubleshooting

| Problem | Hint |
|---|---|
| `permission denied` with the controller | Create the udev rule (see [Build](#build)) and re-plug the controller. If the kernel driver `ftdi_sio` holds the device, `sudo rmmod ftdi_sio` helps. |
| `module usb not found` | Run `npm install` in the `Controller` folder (the file `package.json` must be present). |
| No connection from OBS | Check the firewall, the IP of the replay machine and ports 20010–20060 (SRT/UDP). The ingest is the listener, OBS calls. |
| No audio | Install `gstreamer1.0-libav`, restart the ingest. |
| PGM/PVW judder | Set the display to 50 Hz (`xrandr`, the output is named e.g. `DP-1`). |
| High CPU load or temperature | Use Quick Sync (`intel-media-va-driver`), start the monitor with `--cam-fps 10`, limit the maximum CPU clock (`cpupower`). Avoid staying above 90 °C. |
| Many gaps or anchors | Check the load, check encoder load and unused outputs on the OBS side, check the network, evaluate the log (`monitor-log.txt`). |
| Return path judders | No hardware decoding in OBS; the return path has no B-frames (set that way in the playout). |

## Known limitations

- The DeckLink output (`PLAYOUT_MODE=2`) is untested.
- Lists cut hard between clips (no dissolves).
- No macOS port yet (planned).
- The T-bar reports its position only after the first movement. Until then 100 % applies.
- The ring is re-created when the ingest starts.

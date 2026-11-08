// evs-jlcooper.js
//
// Bridge: JLCooper SloMo Mini (USB/FTDI, Raw-Modus) -> evs-playout (UDP 127.0.0.1:20200).
// Basiert auf der OBS-Bridge, spricht aber direkt mit dem Replay-System. Kein OBS nötig.
//
//   node evs-jlcooper.js           normal
//   node evs-jlcooper.js --debug   zusätzlich jede gesendete Zeile ausgeben
//
// Belegung (SHIFT = SHIFT-Taste gehalten):
//   CAM A-D            Kamera REC 1-4 auf PVW          (mit SHIFT: A = REC 5, B = REC 6)
//   3 Ziffern + CAM    Clip laden im gewählten Winkel  (z. B. 1 1 2 + CAM B = Clip 112, REC 2)
//   3 Ziffern + ENTER  Clip laden im gespeicherten Winkel
//   CLR                Ziffern löschen
//   MARK IN / OUT      Marken auf PVW                  (SHIFT + MARK IN: Marken löschen)
//   STORE CUE          Clip speichern (nächste freie Nummer auf aktueller Seite/Bank)
//   REPLAY             TAKE (PVW -> PGM)
//   PLAY / STOP        PVW (läuft eine Liste auf PGM: PGM) | SHIFT+PLAY: aktive Liste cuen
//   REVERSE PLAY       PVW rückwärts
//   F-FWD              PVW zurück an die Live-Kante
//   REWIND             PVW zum IN-Punkt
//   MANUAL TIME        PVW 5 s hinter Live (Cue)
//   LAST CUE / NEXT    voriger / nächster Clip (Nummer) laden
//   INSERT             Clip auf PVW an die aktive Liste anhängen
//   W1 + Ziffer        aktive Liste wählen (1-9, 0 = Liste 10)
//   A1 / A2            Seite zurück / vor              (mit SHIFT: Bank zurück / vor)
//   V/TC               nächste Liste (SHIFT: vorige), 1-10 im Kreis
//   PRESET             Ton: Varispeed / nur bei 100 % umschalten
//   CYCLE PLAYER       GANG-Modus umschalten
//   JOG ON/OFF         Jog-Rad sperren / freigeben
//   Jog-Rad            PVW bildweise
//   T-Bar              Tempo 5-100 % des laufenden Kanals: PVW, wenn dort PLAY läuft, sonst PGM, wenn es läuft
//                      (steht der Kanal, startet der T-Bar nichts; PLAY startet mit dem T-Bar-Tempo)

const { usb } = require('usb');
const dgram = require('dgram');

const DEBUG = process.argv.includes('--debug');

// ---------------------------------------------------------------------------
// Verbindung zum Playout (UDP, ein kurzlebiger Socket pro Anfrage -> keine Verwechslung der Antworten)
// ---------------------------------------------------------------------------
const PLAYOUT_HOST = '127.0.0.1';
const PLAYOUT_PORT = 20200;

function send(text, timeoutMs = 400) {
  if (DEBUG && text !== 'STATUS') console.log(`  -> ${text.replace(/\n/g, ' | ')}`);
  return new Promise((resolve) => {
    const sock = dgram.createSocket('udp4');
    const done = (reply) => {
      clearTimeout(timer);
      try { sock.close(); } catch (_) { /* schon zu */ }
      resolve(reply);
    };
    const timer = setTimeout(() => done(null), timeoutMs);
    sock.on('message', (msg) => done(msg.toString().trim()));
    sock.on('error', () => done(null));
    sock.send(Buffer.from(text), PLAYOUT_PORT, PLAYOUT_HOST, (err) => { if (err) done(null); });
  });
}

// ---------------------------------------------------------------------------
// Gerät
// ---------------------------------------------------------------------------
const VENDOR_ID = 0x0760;
const PRODUCT_ID = 0x0005;
const IN_ENDPOINT = 1;
const OUT_ENDPOINT = 2;
const BAUD_38400 = 0xc04e;
const DISPLAY_REFRESH_MS = 200;
const KEEP_ALIVE_MS = 50;

const BUTTON_NAMES = {
  0x40: 'replay', 0x42: 'enable', 0x43: 'preset', 0x44: 'scaled', 0x45: 'shift', 0x46: 'cycle-player',
  0x47: 'insert', 0x48: 'v-tc', 0x49: 'a1', 0x4a: 'a2', 0x4b: 'rewind',
  0x4c: 'reverse-play', 0x4d: 'stop', 0x4e: 'play', 0x4f: 'f-fwd',
  0x50: 'cam-a', 0x51: 'cam-b', 0x52: 'cam-c', 0x53: 'cam-d',
  0x54: 'mark-in', 0x55: 'w1', 0x56: 'frame-minus1', 0x57: 'jog-onoff', 0x58: 'frame-plus1', 0x59: 'w2',
  0x5a: 'mark-out', 0x5b: 'manual-time', 0x5c: 'store-cue', 0x5d: 'last-cue', 0x5e: 'next-cue',
  0x5f: 'digit-0', 0x60: 'digit-1', 0x61: 'digit-2', 0x62: 'digit-3', 0x63: 'digit-4',
  0x64: 'digit-5', 0x65: 'digit-6', 0x66: 'digit-7', 0x67: 'digit-8', 0x68: 'digit-9',
  0x69: 'clr', 0x6a: 'enter',
};

// ---------------------------------------------------------------------------
// Zustand der Bridge
// ---------------------------------------------------------------------------
let shiftHeld = false;
let w1Held = false;
let jogEnabled = true;
let digitBuffer = '';
let activeList = 1;
let tbarSpeed = 1.0;     // 0.05 .. 1.0, zuletzt am T-Bar eingestellt
let pvwPlaying = false;  // läuft PVW (von PLAY/REV gestartet)? Nur dann wirkt der T-Bar
let status = null;       // letzte STATUS-Antwort als Objekt
let statusOk = false;    // erreichbar?
let message = null;      // { l1, l2, until } – kurze Meldung auf dem Display

function showMessage(l1, l2 = '', ms = 2500) {
  message = { l1, l2, until: Date.now() + ms };
}

function parseStatus(line) {
  if (!line || !line.startsWith('STATUS')) return null;
  const o = {};
  for (const kv of line.split(' ').slice(1)) {
    const i = kv.indexOf('=');
    if (i > 0) o[kv.slice(0, i)] = kv.slice(i + 1);
  }
  return o;
}

const camLetter = (n) => 'ABCDEF'[Math.max(0, Math.min(5, (parseInt(n, 10) || 1) - 1))];
const num = (key, def = 0) => (status && status[key] !== undefined ? parseInt(status[key], 10) : def);

// Fehler des Playouts kurz und lesbar auf dem 2x16-Display
function shortError(reply) {
  const t = reply.replace('FEHLER: ', '');
  if (t.includes('voll')) return ['BANK VOLL', 'A1/A2: Seite/Bank'];
  if (t.includes('kein IN')) return ['Kein IN gesetzt', ''];
  if (t.includes('OUT')) return ['OUT fehlt', 'nach IN setzen'];
  if (t.includes('IN liegt nicht')) return ['IN zu alt', 'nicht mehr im Ring'];
  if (t.includes('Marke')) return ['Marke fehlt', ''];
  return ['Fehler', t.slice(0, 16)];
}

// Antwort des Playouts kurz anzeigen, wenn es ein Fehler war
function reportError(reply, what) {
  if (reply === null) showMessage('Kein Playout', 'laeuft evs-playout?');
  else if (reply.startsWith('FEHLER')) { const [l1, l2] = shortError(reply); showMessage(l1, l2, 3500); }
}

// ---------------------------------------------------------------------------
// Aktionen
// ---------------------------------------------------------------------------
function playTarget() {
  return status && status.pgmmode === 'LIST' ? 'PGM' : 'PVW';
}

async function recallClip(numberStr, camArg) {
  const id = parseInt(numberStr, 10);
  const r = await send(`RECALL ${id} PVW ${camArg}`);
  if (r && r.startsWith('OK')) {
    pvwPlaying = false;
    showMessage(`Clip ${id} geladen`, '');
  } else if (r && r.startsWith('FEHLER')) {
    showMessage(`Clip ${id}`, 'nicht vorhanden');
  } else reportError(r, `Clip ${id}`);
}

// nächsten/vorigen vorhandenen Clip laden (die Nummern laufen 111..999 ohne Nullen)
async function stepClip(delta) {
  let id = num('pvwclip', 0) || (delta > 0 ? 110 : 1000);
  for (let tries = 0; tries < 800; tries++) {
    id += delta;
    if (id < 111 || id > 999) break;
    if (id % 10 === 0 || Math.floor(id / 10) % 10 === 0) continue;
    const r = await send(`RECALL ${id} PVW`);
    if (r && r.startsWith('OK')) {
      pvwPlaying = false;
      showMessage(`Clip ${id} geladen`, '');
      return;
    }
    if (r === null) { reportError(r, 'Clip'); return; }
  }
  showMessage('Kein weiterer', 'Clip');
}

function selectCamera(n) {
  if (digitBuffer.length === 3) {
    const nr = digitBuffer;
    digitBuffer = '';
    recallClip(nr, n);
    return;
  }
  send(`CAM ${n} PVW`).then((r) => { if (r && r.startsWith('FEHLER')) showMessage(`REC ${n}`, 'nicht vorhanden'); });
}

const BUTTON_ACTIONS = {
  'cam-a': () => selectCamera(shiftHeld ? 5 : 1),
  'cam-b': () => selectCamera(shiftHeld ? 6 : 2),
  'cam-c': () => { if (!shiftHeld) selectCamera(3); },
  'cam-d': () => { if (!shiftHeld) selectCamera(4); },
  'mark-in': () => send(shiftHeld ? 'CLEARMARKS PVW' : 'MARKIN PVW'),
  'mark-out': () => send('MARKOUT PVW'),
  replay: async () => {
    // Stand PVW (gecuet), startet PGM mit dem T-Bar-Tempo. Läuft PVW schon, bleibt dessen Tempo.
    // Läuft eine Liste auf PGM, springt TAKE nur zum nächsten Eintrag (Tempo aus der Liste).
    const standing = status && num('pvwspeed', 0) === 0 && status.pgmmode !== 'LIST';
    const r = standing
      ? await send(`SPEED PVW ${tbarSpeed.toFixed(3)}\nTAKE`)
      : await send('TAKE');
    pvwPlaying = false;
    reportError(r, 'TAKE');
    if (standing && tbarSpeed < 0.995) showMessage('TAKE', `Start ${Math.round(tbarSpeed * 100)}%`, 1200);
  },
  play: async () => {
    if (shiftHeld) {
      const r = await send(`LISTCUE ${activeList}`);
      if (r && r.startsWith('OK')) showMessage(`Liste ${activeList} bereit`, 'PLAY startet');
      else reportError(r, `Liste ${activeList}`);
      return;
    }
    if (playTarget() === 'PGM') { send('PLAY PGM'); return; }
    pvwPlaying = true;
    send(`SPEED PVW ${tbarSpeed.toFixed(3)}`);
  },
  stop: () => {
    if (shiftHeld || playTarget() === 'PGM') { send('STOP PGM'); return; }
    pvwPlaying = false;
    send('STOP PVW');
  },
  'reverse-play': () => {
    pvwPlaying = true;
    send(`SPEED PVW ${(-tbarSpeed).toFixed(3)}`);
  },
  'f-fwd': () => { pvwPlaying = false; send('LIVE PVW'); },
  rewind: () => { pvwPlaying = false; send('GOTOIN PVW').then((r) => reportError(r, 'Zum IN')); },
  'manual-time': () => { pvwPlaying = false; send('JUMP PVW 5'); },
  'store-cue': async () => {
    const r = await send('SAVECLIP PVW');
    if (r && r.startsWith('OK')) {
      const m = r.match(/Clip (\d+) gespeichert.*REC (\d)/);
      if (m) showMessage(`${m[1]}${camLetter(m[2])} gespeichert`, `Seite ${m[1][0]} Bank ${m[1][1]}`);
      else showMessage('Gespeichert', '');
    } else reportError(r, 'Speichern');
  },
  'last-cue': () => stepClip(-1),
  'next-cue': () => stepClip(1),
  insert: async () => {
    const id = num('pvwclip', 0);
    if (!id) { showMessage('Kein Clip auf', 'PVW geladen'); return; }
    const r = await send(`LISTADD ${activeList} ${id}`);
    if (r && r.startsWith('OK')) showMessage(`${id} -> Liste ${activeList}`, 'angehaengt');
    else reportError(r, 'Insert');
  },
  a1: async () => {
    const r = await send(shiftHeld ? 'BANK -' : 'PAGE -');
    if (r) showMessage(r.replace('OK ', ''), '', 1500);
  },
  a2: async () => {
    const r = await send(shiftHeld ? 'BANK +' : 'PAGE +');
    if (r) showMessage(r.replace('OK ', ''), '', 1500);
  },
  'v-tc': () => {  // Liste wechseln: nächste (SHIFT: vorige), 1..10 im Kreis
    activeList = shiftHeld ? (activeList + 8) % 10 + 1 : activeList % 10 + 1;
    showMessage(`Liste ${activeList}`, 'aktiv', 1500);
  },
  preset: async () => {  // Ton: Varispeed / nur bei 100 % umschalten
    const r = await send('AUDIO');
    if (r) showMessage(r.replace('OK ', '').slice(0, 16), '', 1500);
  },
  'cycle-player': async () => {
    const r = await send('GANG');
    if (r) showMessage(r.replace('OK ', ''), '', 1500);
  },
  'jog-onoff': () => {
    jogEnabled = !jogEnabled;
    showMessage(jogEnabled ? 'Jog frei' : 'Jog gesperrt', '', 1500);
  },
  'frame-minus1': () => send('STEP PVW -1'),
  'frame-plus1': () => send('STEP PVW 1'),
};

function handleButton(name, pressed) {
  if (DEBUG) {
    const ts = new Date().toISOString().split('T')[1].replace('Z', '');
    console.log(`[${ts}] ${name}: ${pressed ? 'press' : 'release'}`);
  }
  if (name === 'shift') { shiftHeld = pressed; return; }
  if (name === 'w1') { w1Held = pressed; return; }
  if (!pressed) return;

  if (name.startsWith('digit-')) {
    const d = name.slice(-1);
    if (w1Held) {
      activeList = d === '0' ? 10 : parseInt(d, 10);
      showMessage(`Liste ${activeList}`, 'aktiv', 1500);
      return;
    }
    if (d === '0') { showMessage('Ziffern 1-9', 'keine Null', 1200); return; }
    if (digitBuffer.length < 3) digitBuffer += d;
    return;
  }
  if (name === 'clr') { digitBuffer = ''; return; }
  if (name === 'enter') {
    if (digitBuffer.length === 3) { const nr = digitBuffer; digitBuffer = ''; recallClip(nr, 'SAVED'); }
    else showMessage('Nummer fehlt', '3 Ziffern', 1500);
    return;
  }
  const action = BUTTON_ACTIONS[name];
  if (action) action();
  else if (DEBUG) console.log(`  (keine Funktion fuer "${name}")`);
}

// ---------------------------------------------------------------------------
// T-Bar (Tag 0x83, 0..63) -> Tempo 5..100 %
// ---------------------------------------------------------------------------
let lastSentSpeedPct = -1;
let lastTBarTarget = '';
function handleTBar(value) {
  const pct = Math.max(5, Math.round((value / 63) * 100));
  tbarSpeed = pct / 100;
  // Ziel: der Kanal, der gerade läuft. PVW (von PLAY/REV gestartet) hat Vorrang, sonst PGM, falls es läuft.
  // Steht der Kanal, startet der T-Bar nichts.
  let target = null;
  let curSpeed = 0;
  if (pvwPlaying) { target = 'PVW'; curSpeed = num('pvwspeed', 0); }
  else if (status && num('pgmspeed', 0) !== 0) { target = 'PGM'; curSpeed = num('pgmspeed', 0); }
  if (DEBUG) console.log(`T-Bar ${value}/63 -> ${pct}%  Ziel: ${target || 'keins (PVW startet nicht per PLAY, PGM steht)'}`);
  if (!target) return;
  if (target !== lastTBarTarget) { lastTBarTarget = target; lastSentSpeedPct = -1; }
  if (pct === lastSentSpeedPct) return;
  lastSentSpeedPct = pct;
  // Laufrichtung beibehalten (rückwärts bleibt rückwärts)
  const sign = curSpeed < 0 ? -1 : 1;
  send(`SPEED ${target} ${(sign * tbarSpeed).toFixed(3)}`);
}

// ---------------------------------------------------------------------------
// Jog-Rad (Tag 0x81): 7-Bit-Delta, 0x01..0x3F vorwärts, 0x40..0x7F rückwärts.
// Über 40 ms summiert und als EIN "STEP PVW n" gesendet.
// ---------------------------------------------------------------------------
const JOG_FLUSH_MS = 40;
let jogAccumulator = 0;
let jogTimer = null;
function flushJog() {
  jogTimer = null;
  if (jogAccumulator === 0) return;
  const n = jogAccumulator;
  jogAccumulator = 0;
  pvwPlaying = false;
  send(`STEP PVW ${n}`);
}
function handleJog(value) {
  if (value === 0x00) return;
  if (!jogEnabled) { if (DEBUG) console.log('JOG gesperrt (JOG On/Off)'); return; }
  jogAccumulator += value < 0x40 ? value : value - 128;
  if (!jogTimer) jogTimer = setTimeout(flushJog, JOG_FLUSH_MS);
}

// ---------------------------------------------------------------------------
// USB / FTDI (unverändert aus der OBS-Bridge)
// ---------------------------------------------------------------------------
let device = null;

async function ftdiControlOut(request, value, index = 0) {
  await device.controlTransferOut(
    { requestType: 'vendor', recipient: 'device', request, value, index },
    new Uint8Array(0)
  );
}

async function initFtdi() {
  const SIO_DATA_8O1 = 0x0008 | (0x1 << 8);
  await ftdiControlOut(0x00, 0x00);
  await ftdiControlOut(0x00, 0x01);
  await ftdiControlOut(0x00, 0x02);
  await ftdiControlOut(0x04, SIO_DATA_8O1);
  await ftdiControlOut(0x02, 0x0000);
  await ftdiControlOut(0x03, BAUD_38400);
  await ftdiControlOut(0x09, 1);
}

// ALLE Schreibzugriffe laufen durch diese Warteschlange (zwei gleichzeitige transferOut schlagen fehl).
let writeQueue = Promise.resolve();
function sendBytes(bytes) {
  const write = writeQueue.then(() => device.transferOut(OUT_ENDPOINT, new Uint8Array(bytes)));
  writeQueue = write.catch(() => {});
  return write;
}

async function enterRawMode() {
  const blank32 = new Array(32).fill(0x20);
  await sendBytes([0x8f, 0xff]);
  for (let i = 0; i < 3; i++) for (const reg of [0x10, 0x11, 0x12, 0x13]) await sendBytes([0x80, reg]);
  for (let i = 0; i < 2; i++) {
    await sendBytes([0x80, 0x15]);
    await sendBytes([0x80, 0x19]);
    await sendBytes([0x81, 0x00, ...blank32, 0xff]);
  }
  console.log('Raw mode on.');
}

function startKeepAlive() {
  setInterval(async () => {
    try {
      for (const reg of [0x0b, 0x0c, 0x0d, 0x0e, 0x0f]) await sendBytes([0x80, reg]);
    } catch (err) {
      console.error('Keep-alive failed:', err.message);
    }
  }, KEEP_ALIVE_MS);
}

// ---------------------------------------------------------------------------
// Display: zwei Zeilen à 16 Zeichen, nur ASCII
// ---------------------------------------------------------------------------
function writeDisplay(line1, line2) {
  const pad16 = (s) => s.replace(/[^\x20-\x7e]/g, '?').padEnd(16, ' ').slice(0, 16);
  const text = Array.from(Buffer.from(pad16(line1) + pad16(line2), 'ascii'));
  sendBytes([0x81, 0x00, ...text, 0xff]).catch((err) => console.error('Display write failed:', err.message));
}

function composeDisplay() {
  if (message && Date.now() < message.until) return [message.l1, message.l2];
  if (!statusOk) return ['Kein Playout', 'wartet ...'];
  if (digitBuffer.length > 0) {
    const shown = digitBuffer.padEnd(3, '_');
    return [`Clip ${shown}`, digitBuffer.length === 3 ? 'ENTER/CAM A-F' : ''];
  }
  const page = num('page', 1), bank = num('bank', 1);
  const free = num('free', 0);
  const line1 = `S${page} B${bank} L${String(activeList).padStart(2, '0')} ${free ? 'frei ' + page + bank + free : 'Bank voll'}`;
  const clip = num('pvwclip', 0);
  const mode = status.pvwmode;
  const src = mode === 'CLIP' || mode === 'LIST' ? String(clip) : 'LIVE';
  const speed = String(num('pvwspeed', 0)).padStart(4, ' ') + '%';
  const marks = (status.marks || '00') === '00' ? '' : status.marks === '10' ? ' I' : status.marks === '01' ? ' O' : ' IO';
  return [line1, `${camLetter(status.pvwcam)} ${src}${marks}`.padEnd(8, ' ') + speed.padStart(8, ' ')];
}

function startDisplayLoop() {
  setInterval(async () => {
    const reply = await send('STATUS', 250);
    status = parseStatus(reply);
    statusOk = status !== null;
    const [l1, l2] = composeDisplay();
    writeDisplay(l1, l2);
  }, DISPLAY_REFRESH_MS);
}

// ---------------------------------------------------------------------------
// Eingang: Zwei-Byte-Pakete (Tag, Wert)
// ---------------------------------------------------------------------------
function processBuffer(buffer) {
  let offset = 0;
  while (offset < buffer.length) {
    const tag = buffer[offset];
    if (tag !== 0x80 && tag !== 0x81 && tag !== 0x83) { offset += 1; continue; }
    if (offset + 1 >= buffer.length) break;
    const value = buffer[offset + 1];
    offset += 2;
    if (tag === 0x80) {
      if (BUTTON_NAMES[value]) handleButton(BUTTON_NAMES[value], true);
      else if (BUTTON_NAMES[value + 0x40]) handleButton(BUTTON_NAMES[value + 0x40], false);
      else if (DEBUG) console.log(`Unbekannter Tastencode 0x${value.toString(16)}`);
    } else if (tag === 0x81) handleJog(value);
    else handleTBar(value);
  }
  return offset;
}

async function main() {
  console.log('Suche SloMo Mini ...');
  device = await usb.findDeviceByIds(VENDOR_ID, PRODUCT_ID);
  if (!device) { console.error('Controller nicht gefunden.'); process.exit(1); }
  await device.open();
  if (!device.configuration && device.configurations.length > 0)
    await device.selectConfiguration(device.configurations[0].configurationValue);
  const iface = device.configuration.interfaces[0].interfaceNumber;
  await device.claimInterface(iface);

  await initFtdi();
  await enterRawMode();
  startKeepAlive();
  startDisplayLoop();
  console.log('Bereit. Sendet an evs-playout auf 127.0.0.1:' + PLAYOUT_PORT + (DEBUG ? ' (debug)' : ''));

  let pending = Buffer.alloc(0);
  for (;;) {
    let result;
    try {
      result = await device.transferIn(IN_ENDPOINT, 64);
    } catch (err) {
      console.error('Lesefehler:', err.message);
      break;
    }
    if (!result || !result.data || result.data.byteLength <= 2) continue;
    const raw = new Uint8Array(result.data.buffer, result.data.byteOffset, result.data.byteLength);
    pending = Buffer.concat([pending, Buffer.from(raw.slice(2))]);
    pending = pending.subarray(processBuffer(pending));
  }
  await device.releaseInterface(iface);
  await device.close();
}

main().catch((err) => {
  console.error('Unerwarteter Fehler:', err);
  process.exit(1);
});

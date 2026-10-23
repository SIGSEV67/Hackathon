// App web del nodo: consulta la API cada pocos segundos y dibuja el estado de la
// red, el feed de mensajes y el mapa. Sin dependencias: se sirve desde el
// firmware y el teléfono no tiene internet.
'use strict';

const STATE_MS = 2000;
const MESSAGES_MS = 1500;
const HOLD_MS = 900;
const NEW_FOR_S = 30;
const ALERT_LABELS = ['SOS', 'MÉDICO', 'PELIGRO', 'ESTOY BIEN'];
const VOICE_RATE = 8000;  // lo que viaja por la mesh (MESSAGES_VOICE_SAMPLE_RATE)

const $ = (sel) => document.querySelector(sel);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

const app = {
  messages: [],
  lastId: 0,
  firstLoad: true,
  playing: null,  // id del mensaje de voz que está sonando o cargando
  known: [],      // historial de nodos (para elegir el destino de un privado)
  pending: new Set(),  // privados propios todavía sin confirmar
  voiceMaxS: 20,
  fileMax: 48 * 1024,
  speaker: false,
  thumbs: new Map(),  // id -> <img>, para no volver a pedir la imagen en cada refresco
};

// --- API -----------------------------------------------------------------------

async function api(path, opts) {
  const res = await fetch(path, opts);
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body.error || res.status);
  return body;
}

const post = (path, body) => api(path, { method: 'POST', body: body || '' });

// --- formato -----------------------------------------------------------------------

function ago(seconds) {
  if (seconds < 10) return 'recién';
  if (seconds < 60) return `${seconds} s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} min`;
  return `${Math.floor(seconds / 3600)} h`;
}

function hopsText(hops) {
  if (hops < 0) return 'vos';
  if (hops === 0) return 'directo';
  return `${hops + 1} saltos`;
}

function noiseColor(level) {
  const stops = [[0x4A, 0x3D, 0x14], [0xC2, 0x9F, 0x24], [0xE6, 0xC0, 0x3D], [0xE0, 0x56, 0x3B]];
  const t = (Math.min(level, 100) / 100) * (stops.length - 1);
  const i = Math.min(Math.floor(t), stops.length - 2);
  const f = t - i;
  return `rgb(${stops[i].map((v, k) => Math.round(v + (stops[i + 1][k] - v) * f)).join(',')})`;
}

// --- banner -----------------------------------------------------------------------

let bannerTimer;
function banner(text, ok) {
  const b = $('#banner');
  b.textContent = text;
  b.classList.toggle('ok', !!ok);
  b.hidden = false;
  clearTimeout(bannerTimer);
  bannerTimer = setTimeout(() => { b.hidden = true; }, 6000);
}

// --- feed --------------------------------------------------------------------------

function kb(bytes) {
  return bytes < 1024 ? `${bytes} B` : `${Math.round(bytes / 1024)} KB`;
}

function describe(m) {
  if (m.kind === 'alert') return ALERT_LABELS[m.alert] || m.label;
  if (m.kind === 'text') return m.text;
  if (m.kind === 'file') return m.size ? `${m.name} · ${kb(m.size)}` : 'Archivo';
  return `Mensaje de voz · ${Math.max(1, Math.round(m.ms / 1000))} s`;
}

async function playOnBoard(m) {
  try {
    await post(`/api/play?id=${m.id}`);
    banner('Sonando en la placa', true);
  } catch (e) { banner(`No se pudo (${e.message})`); }
}

function fileBody(m, body) {
  const complete = m.size > 0 && m.got >= m.blocks;
  if (!complete) {
    body.append(el('div', 'meta', `Recibiendo ${Math.floor((100 * m.got) / m.blocks)}%`));
    return;
  }
  const url = `/api/file?id=${m.id}`;
  if (/^image\/(jpeg|png|webp|gif)$/.test(m.mime)) {
    let img = app.thumbs.get(m.id);
    if (!img) {
      img = el('img', 'thumb');
      img.src = url;
      img.alt = m.name;
      img.onclick = () => window.open(url, '_blank');
      app.thumbs.set(m.id, img);
    }
    body.append(img);
  }
  const link = el('a', 'play', `⬇ Descargar`);
  link.href = url;
  link.download = m.name;
  body.append(link);
}

async function playVoice(m) {
  app.playing = m.id;
  renderFeed();
  const done = () => { app.playing = null; renderFeed(); };
  try {
    // Se baja entero y se reproduce desde un blob: Safari no reproduce bien
    // audio en streaming chunked sin soporte de Range.
    const res = await fetch(`/api/voice?id=${m.id}`);
    if (!res.ok) throw new Error(res.status);
    const url = URL.createObjectURL(await res.blob());
    const audio = new Audio(url);
    audio.onended = () => { URL.revokeObjectURL(url); done(); };
    await audio.play();
  } catch (e) {
    done();
    banner('No se pudo reproducir el audio');
  }
}

function feedMeta(m) {
  const meta = el('div', 'meta');
  if (!m.to) {
    meta.textContent = `de ${m.from} · ${hopsText(m.hops)}`;
  } else if (m.hops < 0) {
    meta.append(`privado para ${m.to_name} · `,
      m.delivered ? el('span', 'ok', '✓ entregado') : el('span', 'wait', 'en camino…'));
  } else {
    meta.textContent = `privado de ${m.from} · ${hopsText(m.hops)}`;
  }
  return meta;
}

function feedItem(m) {
  const li = el('li', m.kind === 'alert' ? `alert alert-${m.alert}` : m.kind);
  li.append(el('span', 'when', ago(m.age)));

  const body = el('div', 'body');
  const what = el('div', 'what');
  if (m.to) what.append(el('span', 'dm', 'PRIVADO'));
  what.append(describe(m));
  if (m.age < NEW_FOR_S && m.hops >= 0) what.append(el('span', 'new', 'NEW!'));
  body.append(what, feedMeta(m));

  if (m.kind === 'voice') {
    const complete = m.got >= m.blocks;
    let label = complete ? '▸ Escuchar' : `Recibiendo ${Math.floor((100 * m.got) / m.blocks)}%`;
    if (app.playing === m.id) label = '♪ Sonando…';
    const actions = el('div', 'actions');
    const button = el('button', 'play', label);
    button.disabled = !complete || app.playing !== null;
    button.onclick = () => playVoice(m);
    actions.append(button);
    if (app.speaker && complete) {
      const board = el('button', 'play', '▶ en la placa');
      board.onclick = () => playOnBoard(m);
      actions.append(board);
    }
    body.append(actions);
  }
  if (m.kind === 'file') fileBody(m, body);
  li.append(body);
  return li;
}

function renderFeed() {
  $('#feed').replaceChildren(...app.messages.map(feedItem));
  $('#feed-empty').hidden = app.messages.length > 0;
  $('#stat-msgs').textContent = app.messages.length;
}

async function pollMessages() {
  try {
    // Pide todo (no solo lo nuevo) para actualizar edades y voces a medio
    // llegar: son pocos mensajes y la red es local.
    const { messages } = await api('/api/messages');
    const fresh = messages.filter((m) => m.id > app.lastId);
    app.messages = messages;
    if (messages.length) app.lastId = Math.max(app.lastId, ...messages.map((m) => m.id));

    const alert = !app.firstLoad && fresh.find((m) => m.kind === 'alert' && m.hops >= 0);
    const dm = !app.firstLoad && fresh.find((m) => m.to && m.hops >= 0);
    if (alert) {
      banner(`${ALERT_LABELS[alert.alert]} · ${alert.from} (${hopsText(alert.hops)})`, alert.alert === 3);
      if (navigator.vibrate) navigator.vibrate(alert.alert === 3 ? 200 : [300, 100, 300, 100, 300]);
    } else if (dm) {
      banner(`Privado de ${dm.from}: ${describe(dm)}`, true);
      if (navigator.vibrate) navigator.vibrate([100, 60, 100]);
    }

    // Aviso cuando un privado nuestro llega a destino.
    for (const m of messages) {
      if (!m.to || m.hops >= 0) continue;
      if (!m.delivered) app.pending.add(m.id);
      else if (app.pending.delete(m.id) && !app.firstLoad) banner(`✓ Le llegó a ${m.to_name}`, true);
    }
    app.firstLoad = false;
    renderFeed();
  } catch (e) { /* el nodo puede estar ocupado: reintenta en el próximo ciclo */ }
}

// --- alertas (mantener apretado) ---------------------------------------------------------

function setupAlertButtons() {
  document.documentElement.style.setProperty('--hold', `${HOLD_MS}ms`);
  for (const b of document.querySelectorAll('.alert-btn')) {
    let timer;
    const cancel = () => { clearTimeout(timer); b.classList.remove('holding'); };
    b.addEventListener('pointerdown', (ev) => {
      ev.preventDefault();
      b.classList.add('holding');
      timer = setTimeout(async () => {
        b.classList.remove('holding');
        const kind = Number(b.dataset.kind);
        try {
          await post(`/api/alert?kind=${kind}`);
          banner(`Enviado: ${ALERT_LABELS[kind]}`, true);
          if (navigator.vibrate) navigator.vibrate(80);
          pollMessages();
        } catch (e) { banner(`No se pudo enviar (${e.message})`); }
      }, HOLD_MS);
    });
    for (const name of ['pointerup', 'pointerleave', 'pointercancel']) b.addEventListener(name, cancel);
    b.addEventListener('contextmenu', (ev) => ev.preventDefault());
  }
}

// --- destino de un privado ---------------------------------------------------------------

const OTHER = 'otro';

function knownLabel(k) {
  const where = k.alive ? 'en la red' : k.seen ? `visto hace ${ago(k.age)}` : 'sin ver';
  return `${k.name} · ${k.id.slice(-4).toUpperCase()} · ${where}`;
}

// Rehace las opciones solo si cambiaron, para no cerrarle el menú a quien lo
// tiene abierto, y conserva la elección.
function renderRecipients(state) {
  app.known = state.known || [];
  const select = $('#to');
  const options = [['', 'Todos'], ...app.known.map((k) => [k.id, knownLabel(k)]), [OTHER, 'Otro ID…']];
  const key = JSON.stringify(options);
  if (select.dataset.key === key) return;
  const value = select.value;
  select.replaceChildren(...options.map(([v, label]) => {
    const o = el('option', '', label);
    o.value = v;
    return o;
  }));
  select.dataset.key = key;
  select.value = options.some(([v]) => v === value) ? value : '';
}

function updateComposeMode() {
  const to = $('#to').value;
  $('#to-id').hidden = to !== OTHER;
  $('#compose').classList.toggle('private', to !== '');
  $('#text').placeholder = to ? 'Mensaje privado (160)' : 'Escribí un mensaje (160)';
}

function selectRecipient(id) {
  $('#to').value = id;
  updateComposeMode();
  document.querySelector('.indice button[data-tab="feed"]').click();
  $('#text').focus();
}

// Devuelve el id de 8 hex del destino ('' = todos). A mano se acepta el id
// completo o sus últimas cifras (las que muestra la placa), si no hay dudas.
function resolveRecipient() {
  const to = $('#to').value;
  if (to !== OTHER) return to;
  const typed = $('#to-id').value.trim().toLowerCase();
  if (!/^[0-9a-f]{1,8}$/.test(typed)) throw new Error('el ID son hasta 8 cifras hexa');
  if (typed.length === 8) return typed;
  const matches = app.known.filter((k) => k.id.endsWith(typed));
  if (matches.length === 1) return matches[0].id;
  throw new Error(matches.length ? 'hay varios nodos con ese final, escribí más cifras' : 'no conozco ese nodo, escribí las 8 cifras');
}

function recipientName(id) {
  const k = app.known.find((n) => n.id === id);
  return k ? k.name : id.toUpperCase();
}

// --- audio desde el celular --------------------------------------------------------------
// El navegador solo da el micrófono (getUserMedia) en HTTPS y el portal es HTTP,
// así que se usa el grabador del sistema (<input capture>) y Web Audio, que sí
// anda en HTTP, para decodificar lo que devuelve (m4a, webm, ogg, wav...).

function decodeAudio(buf) {
  const Ctx = window.AudioContext || window.webkitAudioContext;
  const ctx = new Ctx();
  // Safari viejo solo tiene la versión con callbacks.
  return new Promise((resolve, reject) => ctx.decodeAudioData(buf, resolve, reject))
    .finally(() => ctx.close && ctx.close());
}

// Mezcla a mono, baja a 8 kHz promediando (filtro anti-alias simple), recorta
// al largo máximo y normaliza, para aprovechar los bits del ADPCM.
function toVoicePcm(audio, maxS) {
  const chans = [...Array(audio.numberOfChannels).keys()].map((c) => audio.getChannelData(c));
  const ratio = audio.sampleRate / VOICE_RATE;
  const n = Math.min(Math.floor(audio.length / ratio), maxS * VOICE_RATE);
  const out = new Float32Array(n);
  let peak = 1e-4;
  for (let i = 0; i < n; i++) {
    const a = Math.floor(i * ratio), b = Math.max(a + 1, Math.floor((i + 1) * ratio));
    let sum = 0;
    for (const ch of chans) for (let j = a; j < b; j++) sum += ch[j];
    out[i] = sum / ((b - a) * chans.length);
    peak = Math.max(peak, Math.abs(out[i]));
  }
  const gain = Math.min(0.8 / peak, 20);
  const pcm = new Int16Array(n);
  for (let i = 0; i < n; i++) pcm[i] = Math.max(-32768, Math.min(32767, Math.round(out[i] * gain * 32767)));
  return pcm;
}

async function sendAudioFile(file) {
  let to;
  try {
    to = resolveRecipient();
  } catch (e) { banner(`Destino inválido: ${e.message}`); return; }
  const button = $('#rec-btn');
  button.disabled = true;
  button.textContent = 'Procesando…';
  try {
    const audio = await decodeAudio(await file.arrayBuffer());
    const pcm = toVoicePcm(audio, app.voiceMaxS);
    if (pcm.length < VOICE_RATE / 4) throw new Error('muy corto');
    button.textContent = 'Enviando…';
    await api(to ? `/api/voice?to=${to}` : '/api/voice', {
      method: 'POST', body: pcm.buffer, headers: { 'Content-Type': 'application/octet-stream' },
    });
    const cut = audio.duration > app.voiceMaxS ? ` (recortado a ${app.voiceMaxS} s)` : '';
    banner(to ? `Audio privado para ${recipientName(to)}: en camino${cut}` : `Audio enviado${cut}`, true);
    pollMessages();
  } catch (e) {
    banner(`No se pudo mandar el audio (${e.message || 'formato no soportado'})`);
  } finally {
    button.disabled = false;
    button.textContent = '● Grabar audio';
  }
}

// --- archivos ------------------------------------------------------------------------------
// Entra poco (decenas de KB sin PSRAM), así que las fotos se achican y se
// recomprimen acá antes de subirlas.

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => { URL.revokeObjectURL(url); resolve(img); };
    img.onerror = () => { URL.revokeObjectURL(url); reject(new Error('imagen ilegible')); };
    img.src = url;
  });
}

const toBlob = (canvas, q) => new Promise((resolve) => canvas.toBlob(resolve, 'image/jpeg', q));

async function shrinkImage(file, limit) {
  const img = await loadImage(file);
  for (const side of [1280, 1024, 800, 640, 480, 360, 240]) {
    const scale = Math.min(1, side / Math.max(img.naturalWidth, img.naturalHeight));
    const canvas = document.createElement('canvas');
    canvas.width = Math.round(img.naturalWidth * scale);
    canvas.height = Math.round(img.naturalHeight * scale);
    canvas.getContext('2d').drawImage(img, 0, 0, canvas.width, canvas.height);
    for (const q of [0.8, 0.65, 0.5, 0.35]) {
      const blob = await toBlob(canvas, q);
      if (blob && blob.size <= limit) return blob;
    }
  }
  throw new Error('no se pudo achicar lo suficiente');
}

// Corta el nombre a 40 bytes de UTF-8 (lo que guarda la placa).
function shortName(name) {
  const enc = new TextEncoder();
  let out = '';
  for (const ch of name) {
    if (enc.encode(out + ch).length > 40) break;
    out += ch;
  }
  return out;
}

async function sendFile(file) {
  let to;
  try {
    to = resolveRecipient();
  } catch (e) { banner(`Destino inválido: ${e.message}`); return; }
  const button = $('#file-btn');
  button.disabled = true;
  button.textContent = 'Preparando…';
  try {
    let body = file, type = file.type || 'application/octet-stream', name = file.name || 'archivo';
    const photo = /^image\/(jpeg|png|webp|heic|heif)$/.test(file.type);
    if (photo && file.size > app.fileMax * 0.9) {
      body = await shrinkImage(file, app.fileMax * 0.9);
      type = 'image/jpeg';
      name = name.replace(/\.[^.]*$/, '') + '.jpg';
    }
    if (body.size > app.fileMax) throw new Error(`máximo ${kb(app.fileMax)}`);
    button.textContent = 'Enviando…';
    await api(to ? `/api/file?to=${to}` : '/api/file', {
      method: 'POST', body, headers: { 'Content-Type': type, 'X-File-Name': encodeURIComponent(shortName(name)) },
    });
    banner(`${to ? `Archivo privado para ${recipientName(to)}` : 'Archivo enviado'} (${kb(body.size)})`, true);
    pollMessages();
  } catch (e) {
    banner(`No se pudo mandar el archivo (${e.message})`);
  } finally {
    button.disabled = false;
    button.textContent = '＋ Foto / archivo';
  }
}

function setupForms() {
  $('#to').addEventListener('change', updateComposeMode);
  $('#file-btn').addEventListener('click', () => $('#file-in').click());
  $('#file-in').addEventListener('change', (ev) => {
    const file = ev.target.files[0];
    ev.target.value = '';
    if (file) sendFile(file);
  });
  $('#rec-btn').addEventListener('click', () => $('#rec-file').click());
  $('#rec-file').addEventListener('change', (ev) => {
    const file = ev.target.files[0];
    ev.target.value = '';  // para poder elegir el mismo archivo de nuevo
    if (file) sendAudioFile(file);
  });
  $('#compose').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const input = $('#text');
    const text = input.value.trim();
    if (!text) return;
    let to;
    try {
      to = resolveRecipient();
    } catch (e) { banner(`Destino inválido: ${e.message}`); return; }
    try {
      await post(to ? `/api/text?to=${to}` : '/api/text', text);
      input.value = '';
      if (to) banner(`Privado para ${recipientName(to)}: en camino`, true);
      pollMessages();
    } catch (e) { banner(`No se pudo enviar (${e.message})`); }
  });
  $('#rename').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const input = $('#name');
    const name = input.value.trim();
    if (!name) return;
    try {
      await post('/api/name', name);
      input.value = '';
      banner(`Ahora este nodo es "${name}"`, true);
      pollState();
    } catch (e) { banner(`Nombre inválido (${e.message})`); }
  });
}

// --- mapa de la red --------------------------------------------------------------------
// Layout de fuerzas simple: los nodos se repelen y los enlaces actúan como
// resortes. Las posiciones se mantienen entre actualizaciones.

const map = { pos: new Map(), nodes: [], edges: [] };
const W = 360, H = 300;

function updateMap(state) {
  const all = [state.self, ...state.nodes];
  const ids = new Set(all.map((n) => n.id));
  map.nodes = all;
  for (const n of all) {
    if (!map.pos.has(n.id)) {
      map.pos.set(n.id, { x: W / 2 + (Math.random() - 0.5) * 80, y: H / 2 + (Math.random() - 0.5) * 80, vx: 0, vy: 0 });
    }
  }
  for (const id of [...map.pos.keys()]) if (!ids.has(id)) map.pos.delete(id);

  // Un enlace por par, con el mejor RSSI que reporte cualquiera de los dos.
  const edges = new Map();
  for (const n of all) {
    for (const l of n.links) {
      if (!ids.has(l.id)) continue;
      const key = n.id < l.id ? `${n.id}-${l.id}` : `${l.id}-${n.id}`;
      const prev = edges.get(key);
      if (!prev || l.rssi > prev.rssi) edges.set(key, { a: n.id, b: l.id, rssi: l.rssi });
    }
  }
  map.edges = [...edges.values()];
}

function stepLayout() {
  const pts = map.nodes.map((n) => map.pos.get(n.id));
  for (let i = 0; i < pts.length; i++) {
    for (let j = i + 1; j < pts.length; j++) {
      const dx = pts[j].x - pts[i].x, dy = pts[j].y - pts[i].y;
      const d2 = Math.max(dx * dx + dy * dy, 25);
      const d = Math.sqrt(d2);
      const f = 1800 / d2;
      pts[i].vx -= (dx / d) * f; pts[i].vy -= (dy / d) * f;
      pts[j].vx += (dx / d) * f; pts[j].vy += (dy / d) * f;
    }
  }
  for (const e of map.edges) {
    const a = map.pos.get(e.a), b = map.pos.get(e.b);
    const dx = b.x - a.x, dy = b.y - a.y;
    const d = Math.sqrt(dx * dx + dy * dy) || 1;
    const f = (d - 100) * 0.02;
    a.vx += (dx / d) * f; a.vy += (dy / d) * f;
    b.vx -= (dx / d) * f; b.vy -= (dy / d) * f;
  }
  for (const p of pts) {
    p.vx += (W / 2 - p.x) * 0.003; p.vy += (H / 2 - p.y) * 0.003;
    p.vx *= 0.8; p.vy *= 0.8;
    p.x = Math.min(W - 30, Math.max(30, p.x + p.vx));
    p.y = Math.min(H - 30, Math.max(24, p.y + p.vy));
  }
}

function drawMap() {
  const ns = 'http://www.w3.org/2000/svg';
  const mk = (tag, attrs) => {
    const e = document.createElementNS(ns, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  };
  const parts = [];
  for (const e of map.edges) {
    const a = map.pos.get(e.a), b = map.pos.get(e.b);
    const quality = Math.min(1, Math.max(0.1, (e.rssi + 95) / 50));  // -95 dBm malo, -45 dBm excelente
    parts.push(mk('line', { x1: a.x, y1: a.y, x2: b.x, y2: b.y, stroke: '#6B5A28', 'stroke-width': 1 + quality * 4 }));
  }
  map.nodes.forEach((n, i) => {
    const p = map.pos.get(n.id);
    const s = 12 + n.noise / 6;  // lado del cuadrado: más ruido, más grande
    if (i === 0) parts.push(mk('rect', { x: p.x - s / 2 - 5, y: p.y - s / 2 - 5, width: s + 10, height: s + 10, class: 'self' }));
    parts.push(mk('rect', { x: p.x - s / 2, y: p.y - s / 2, width: s, height: s, fill: noiseColor(n.noise) }));
    const label = mk('text', { x: p.x, y: p.y + s / 2 + 14 });
    label.textContent = n.name;
    parts.push(label);
  });
  $('#map').replaceChildren(...parts);
}

function animateMap() {
  if (!$('#tab-map').hidden && map.nodes.length) {
    stepLayout();
    drawMap();
  }
  requestAnimationFrame(animateMap);
}

// --- estado ---------------------------------------------------------------------------

function datos(target, pairs) {
  $(target).replaceChildren(...pairs.map(([k, v]) => {
    const li = el('li');
    li.append(el('span', 'k', k), el('span', '', v));
    return li;
  }));
}

function renderHero(state) {
  const s = state.self;
  const total = state.nodes.length + 1;
  $('#hero-name').textContent = s.name;
  const rec = state.rec.active ? ` · ● grabando ${Math.floor(state.rec.ms / 1000)} s` : '';
  $('#hero-net').textContent = `${total} nodo${total > 1 ? 's' : ''} · ${s.links.length} vecino${s.links.length === 1 ? '' : 's'}${rec}`;
  const meter = $('#hero-noise');
  meter.style.width = `${s.noise}%`;
  meter.style.background = noiseColor(s.noise);
  $('#stat-nodes').textContent = total;
  $('#stat-links').textContent = s.links.length;
}

function dmButton(id) {
  const b = el('button', 'dm-btn', '✉');
  b.title = 'Mandar privado';
  b.onclick = () => selectRecipient(id);
  return b;
}

function renderNodes(state) {
  const rows = [state.self, ...state.nodes].map((n, i) => {
    const li = el('li');
    li.append(el('span', 'when', i === 0 ? 'este' : hopsText(n.hops)));
    const name = el('span', 'name', `${n.name} `);
    name.append(el('small', '', i === 0 ? '' : `· ${ago(Math.floor(n.age / 1000))}`));
    const bar = el('span', 'bar');
    const fill = el('i');
    fill.style.width = `${n.noise}%`;
    fill.style.background = noiseColor(n.noise);
    bar.append(fill);
    li.append(name, bar);
    if (i > 0) li.append(dmButton(n.id));
    return li;
  });
  // Los que conocemos pero no están: se les puede escribir igual y les llega
  // cuando vuelvan.
  for (const k of (state.known || []).filter((n) => !n.alive)) {
    const li = el('li', 'off');
    li.append(el('span', 'when', 'fuera'));
    const name = el('span', 'name', `${k.name} `);
    name.append(el('small', '', k.seen ? `· visto hace ${ago(k.age)}` : '· del historial'));
    li.append(name, dmButton(k.id));
    rows.push(li);
  }
  $('#nodes').replaceChildren(...rows);
}

function renderSelf(state) {
  const s = state.self;
  const caps = [s.flags & 1 ? 'pantalla' : null, s.flags & 2 ? 'PSRAM 8 MB' : null].filter(Boolean).join(', ') || '—';
  datos('#self', [
    ['Nombre', s.name], ['ID', s.id], ['Prendido', ago(s.up)], ['Hardware', caps], ['Ruido', `${s.noise} / 100`],
  ]);
  const st = state.stats;
  $('#traffic').replaceChildren(...[
    ['recibidos', st.rx], ['enviados', st.tx], ['reenviados', st.fwd], ['duplicados', st.dup], ['perdidos', st.dropped],
    ['en custodia', state.transit || 0],
  ].map(([label, num]) => {
    const li = el('li');
    li.append(el('span', 'num', String(num)), el('span', 'label', label));
    return li;
  }));
}

async function pollState() {
  try {
    const state = await api('/api/state');
    renderHero(state);
    renderNodes(state);
    renderSelf(state);
    renderRecipients(state);
    if (state.voice_max_s) app.voiceMaxS = state.voice_max_s;
    if (state.file_max) app.fileMax = state.file_max;
    app.speaker = !!state.speaker;
    $('#file-note').textContent = `Archivos de hasta ${kb(app.fileMax)}; las fotos se achican solas.`;
    updateMap(state);
  } catch (e) {
    $('#hero-net').textContent = 'sin conexión con el nodo…';
  }
}

// --- pestañas y arranque -------------------------------------------------------------------

function setupTabs() {
  for (const b of document.querySelectorAll('.indice button')) {
    b.addEventListener('click', () => {
      for (const o of document.querySelectorAll('.indice button')) o.setAttribute('aria-selected', String(o === b));
      for (const t of document.querySelectorAll('.tab')) t.hidden = t.id !== `tab-${b.dataset.tab}`;
    });
  }
}

setupTabs();
setupAlertButtons();
setupForms();
renderFeed();
pollState();
pollMessages();
setInterval(pollState, STATE_MS);
setInterval(pollMessages, MESSAGES_MS);
requestAnimationFrame(animateMap);

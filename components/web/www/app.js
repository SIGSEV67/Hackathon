// App web del nodo: consulta la API cada pocos segundos y dibuja el feed, el
// mapa de la red y el estado del nodo. Sin dependencias: se sirve desde el
// firmware y el teléfono no tiene internet.
'use strict';

const STATE_MS = 2000;
const MESSAGES_MS = 1500;
const HOLD_MS = 900;
const ALERT_LABELS = ['SOS', 'MÉDICO', 'PELIGRO', 'ESTOY BIEN'];

const $ = (sel) => document.querySelector(sel);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

const app = {
  state: null,
  messages: new Map(),   // id -> mensaje
  lastId: 0,
  firstLoad: true,
  playing: null,         // id del mensaje de voz que está sonando o cargando
};

// --- API ---------------------------------------------------------------------

async function api(path, opts) {
  const res = await fetch(path, opts);
  const body = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(body.error || res.status);
  return body;
}

const post = (path, body) => api(path, { method: 'POST', body: body || '' });

// --- formato -------------------------------------------------------------------

function ago(seconds) {
  if (seconds < 10) return 'recién';
  if (seconds < 60) return `hace ${seconds} s`;
  if (seconds < 3600) return `hace ${Math.floor(seconds / 60)} min`;
  return `hace ${Math.floor(seconds / 3600)} h`;
}

function hopsText(hops) {
  if (hops < 0) return 'vos';
  if (hops === 0) return 'directo';
  return `${hops + 1} saltos`;
}

// --- banner ----------------------------------------------------------------------

let bannerTimer;
function banner(text, ok) {
  const b = $('#banner');
  b.textContent = text;
  b.classList.toggle('ok', !!ok);
  b.hidden = false;
  clearTimeout(bannerTimer);
  bannerTimer = setTimeout(() => { b.hidden = true; }, 6000);
}

// --- feed ------------------------------------------------------------------------

function describe(m) {
  if (m.kind === 'alert') return ALERT_LABELS[m.alert] || m.label;
  if (m.kind === 'text') return m.text;
  return `Mensaje de voz · ${Math.round(m.ms / 1000)} s`;
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

function renderFeed() {
  const feed = $('#feed');
  const list = [...app.messages.values()].sort((a, b) => b.id - a.id);
  if (!list.length) return;
  feed.replaceChildren(...list.map((m) => {
    const li = el('li', m.kind === 'alert' ? `alert-${m.alert}` : m.kind);
    li.append(el('div', 'what', describe(m)));
    li.append(el('div', 'meta', `${m.from} · ${hopsText(m.hops)} · ${ago(m.age)}`));
    if (m.kind === 'voice') {
      const complete = m.got >= m.blocks;
      let label = complete ? '▶ Escuchar' : `Recibiendo ${Math.floor((100 * m.got) / m.blocks)}%`;
      if (app.playing === m.id) label = '♪ Sonando…';
      const button = el('button', 'play', label);
      button.disabled = !complete || app.playing !== null;
      button.onclick = () => playVoice(m);
      li.append(button);
    }
    return li;
  }));
}

async function pollMessages() {
  try {
    // Pide todo (no solo lo nuevo) para actualizar edades y voces a medio llegar;
    // son pocos mensajes y la red es local.
    const { messages } = await api('/api/messages');
    const fresh = messages.filter((m) => m.id > app.lastId);
    app.messages = new Map(messages.map((m) => [m.id, m]));
    if (messages.length) app.lastId = Math.max(app.lastId, ...messages.map((m) => m.id));

    if (!app.firstLoad) {
      const alert = fresh.find((m) => m.kind === 'alert' && m.hops >= 0);
      if (alert) {
        banner(`${ALERT_LABELS[alert.alert]} — ${alert.from} (${hopsText(alert.hops)})`, alert.alert === 3);
        if (navigator.vibrate) navigator.vibrate(alert.alert === 3 ? 200 : [300, 100, 300, 100, 300]);
      }
    }
    app.firstLoad = false;
    renderFeed();
  } catch (e) { /* el nodo puede estar ocupado; reintenta en el próximo ciclo */ }
}

// --- alertas (mantener apretado) ---------------------------------------------------

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
    for (const evName of ['pointerup', 'pointerleave', 'pointercancel']) b.addEventListener(evName, cancel);
    b.addEventListener('contextmenu', (ev) => ev.preventDefault());
  }
}

function setupForms() {
  $('#compose').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const input = $('#text');
    const text = input.value.trim();
    if (!text) return;
    try {
      await post('/api/text', text);
      input.value = '';
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

// --- mapa de la red -------------------------------------------------------------------
// Layout de fuerzas simple: los nodos se repelen y los enlaces actúan como
// resortes. Las posiciones se mantienen entre actualizaciones.

const map = { pos: new Map(), nodes: [], edges: [] };
const W = 360, H = 300;

function noiseColor(level) {
  const stops = [[0x4A, 0x3D, 0x14], [0xC2, 0x9F, 0x24], [0xE6, 0xC0, 0x3D], [0xE0, 0x56, 0x3B]];
  const t = Math.min(level, 100) / 100 * (stops.length - 1);
  const i = Math.min(Math.floor(t), stops.length - 2);
  const f = t - i;
  const c = stops[i].map((v, k) => Math.round(v + (stops[i + 1][k] - v) * f));
  return `rgb(${c.join(',')})`;
}

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
      const f = 1800 / d2;
      const d = Math.sqrt(d2);
      pts[i].vx -= (dx / d) * f; pts[i].vy -= (dy / d) * f;
      pts[j].vx += (dx / d) * f; pts[j].vy += (dy / d) * f;
    }
  }
  for (const e of map.edges) {
    const a = map.pos.get(e.a), b = map.pos.get(e.b);
    const dx = b.x - a.x, dy = b.y - a.y;
    const d = Math.sqrt(dx * dx + dy * dy) || 1;
    const f = (d - 90) * 0.02;
    a.vx += (dx / d) * f; a.vy += (dy / d) * f;
    b.vx -= (dx / d) * f; b.vy -= (dy / d) * f;
  }
  for (const p of pts) {
    p.vx += (W / 2 - p.x) * 0.003; p.vy += (H / 2 - p.y) * 0.003;
    p.vx *= 0.8; p.vy *= 0.8;
    p.x = Math.min(W - 24, Math.max(24, p.x + p.vx));
    p.y = Math.min(H - 24, Math.max(24, p.y + p.vy));
  }
}

function drawMap() {
  const svg = $('#map');
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
    parts.push(mk('line', { x1: a.x, y1: a.y, x2: b.x, y2: b.y, stroke: '#9C8A55',
      'stroke-width': 1 + quality * 4, 'stroke-opacity': 0.35 + quality * 0.5 }));
  }
  for (const n of map.nodes) {
    const p = map.pos.get(n.id);
    const r = 9 + n.noise / 9;
    if (n === map.nodes[0]) parts.push(mk('circle', { cx: p.x, cy: p.y, r: r + 5, class: 'self-ring' }));
    parts.push(mk('circle', { cx: p.x, cy: p.y, r, fill: noiseColor(n.noise), stroke: '#000', 'stroke-width': 2 }));
    const label = mk('text', { x: p.x, y: p.y + r + 12 });
    label.textContent = n.name;
    parts.push(label);
  }
  svg.replaceChildren(...parts);
}

function animateMap() {
  if (!$('#tab-map').hidden && map.nodes.length) {
    stepLayout();
    drawMap();
  }
  requestAnimationFrame(animateMap);
}

function renderNodes(state) {
  const rows = [state.self, ...state.nodes].map((n, i) => {
    const tr = el('tr');
    tr.append(el('td', '', i === 0 ? `${n.name} (este)` : n.name));
    const noise = el('td');
    const bar = el('span', 'bar');
    bar.style.width = `${Math.max(2, n.noise / 2)}px`;
    bar.style.background = noiseColor(n.noise);
    noise.append(bar, document.createTextNode(n.noise));
    tr.append(noise);
    tr.append(el('td', '', i === 0 ? '—' : String(n.hops + 1)));
    tr.append(el('td', '', i === 0 ? 'ahora' : ago(Math.floor(n.age / 1000))));
    return tr;
  });
  $('#nodes').replaceChildren(...rows);
}

// --- estado del nodo ----------------------------------------------------------------

function kv(target, pairs) {
  const items = [];
  for (const [k, v] of pairs) items.push(el('dt', '', k), el('dd', '', v));
  $(target).replaceChildren(...items);
}

function renderSelf(state) {
  const s = state.self;
  const caps = [s.flags & 1 ? 'pantalla' : null, s.flags & 2 ? 'PSRAM 8 MB' : null].filter(Boolean).join(', ') || '—';
  kv('#self', [
    ['Nombre', s.name], ['ID', s.id], ['Encendido', ago(s.up).replace('hace ', '')],
    ['Hardware', caps], ['Ruido local', `${s.noise}/100`], ['Vecinos directos', String(s.links.length)],
  ]);
  const st = state.stats;
  kv('#stats', [
    ['Recibidos', String(st.rx)], ['Enviados', String(st.tx)], ['Reenviados', String(st.fwd)],
    ['Duplicados', String(st.dup)], ['Perdidos', String(st.dropped)],
  ]);
  const total = state.nodes.length + 1;
  const rec = state.rec.active ? ` · <b>● grabando ${Math.floor(state.rec.ms / 1000)} s</b>` : '';
  $('#status').innerHTML = `<b>${escapeHtml(s.name)}</b><br>${total} nodo${total > 1 ? 's' : ''} en la red${rec}`;
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

async function pollState() {
  try {
    const state = await api('/api/state');
    app.state = state;
    renderSelf(state);
    renderNodes(state);
    updateMap(state);
  } catch (e) {
    $('#status').textContent = 'sin conexión con el nodo…';
  }
}

// --- tabs y arranque -------------------------------------------------------------------

function setupTabs() {
  for (const b of document.querySelectorAll('.tabs button')) {
    b.addEventListener('click', () => {
      for (const o of document.querySelectorAll('.tabs button')) o.setAttribute('aria-selected', String(o === b));
      for (const t of document.querySelectorAll('.tab')) t.hidden = t.id !== `tab-${b.dataset.tab}`;
    });
  }
}

setupTabs();
setupAlertButtons();
setupForms();
pollState();
pollMessages();
setInterval(pollState, STATE_MS);
setInterval(pollMessages, MESSAGES_MS);
requestAnimationFrame(animateMap);

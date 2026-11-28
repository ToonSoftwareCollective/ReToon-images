/* Toon web interface: page editor, import from Home Assistant, settings. */
'use strict';
const $ = (s, el = document) => el.querySelector(s);
const el = (tag, props = {}, ...kids) => {
  const e = Object.assign(document.createElement(tag), props);
  for (const k of kids.flat()) e.append(k);
  return e;
};
/* From the origin, never from the page address: opened from the QR code on
   the Toon's screen, that address holds the login, and browsers refuse
   requests built from an address with a login in it. */
const BASE = location.origin + location.pathname.replace(/[^/]*$/, '');
const API = BASE + 'cgi-bin/api';
const SUPPORTED = ['tile', 'heading', 'thermostat', 'entities', 'markdown', 'custom:toon-clock',
  'grid', 'vertical-stack', 'horizontal-stack', 'conditional', 'custom:retoon-app', 'history-graph',
  'statistics-graph', 'energy-usage-graph', 'energy-solar-graph', 'energy-gas-graph', 'energy-distribution', 'gauge'];
const TYPES = { tile: 'Tile', heading: 'Heading', thermostat: 'Thermostat', gauge: 'Gauge', entities: 'List of entities',
  markdown: 'Text', 'custom:toon-clock': 'Clock', 'custom:retoon-app': 'App', 'history-graph': 'Graph' };
const ENERGY = { 'energy-usage-graph': 'Energy usage', 'energy-solar-graph': 'Solar production', 'energy-gas-graph': 'Gas usage',
  'energy-distribution': 'Energy distribution' };
const HOURS = { 1: '1 hour', 3: '3 hours', 6: '6 hours', 12: '12 hours', 24: '24 hours', 48: '2 days', 72: '3 days',
  168: 'A week' };
const FEATURES = { '': 'None', 'light-brightness': 'Brightness slider', 'media-player-volume-slider': 'Volume slider',
  'cover-position': 'Position slider', 'cover-open-close': 'Open / stop / close buttons',
  'target-temperature': 'Temperature − / +', 'select-options': 'Option buttons', 'climate-hvac-modes': 'Heating mode buttons',
  'climate-preset-modes': 'Preset buttons', 'climate-fan-modes': 'Fan speed buttons',
  'climate-swing-modes': 'Swing buttons', 'fan-preset-modes': 'Fan preset buttons', 'fan-speed': 'Fan speed slider' };
/* the features with a row of buttons: which list in the card picks them, and
   where the entity keeps what can be chosen (as Home Assistant has it) */
/* a tile's own text size and colour (our own options, the screen's names) */
const TEXT_SIZES = { '': 'Standard', small: 'Small', large: 'Large', 'x-large': 'Extra large' };
const TEXT_COLORS = { '': 'Standard', white: 'White', grey: 'Grey', red: 'Red', orange: 'Orange', amber: 'Amber',
  yellow: 'Yellow', green: 'Green', teal: 'Teal', cyan: 'Cyan', blue: 'Blue', purple: 'Purple', pink: 'Pink',
  own: 'Own colour…' };
function sizePicker(c, key) {
  const s = el('select', {}, Object.entries(TEXT_SIZES).map(([k, v]) => el('option', { value: k, textContent: v, selected: (c[key] || '') === k })));
  s.onchange = () => { if (s.value) c[key] = s.value; else delete c[key]; };
  return s;
}
function colorPicker(c, key) {
  const own = /^#[0-9a-f]{6}$/i.test(c[key] || '');
  const s = el('select', {}, Object.entries(TEXT_COLORS).map(([k, v]) => el('option', { value: k, textContent: v,
    selected: own ? k === 'own' : (c[key] || '') === k })));
  const pick = el('input', { type: 'color', value: own ? c[key] : '#ffffff', hidden: !own });
  s.onchange = () => {
    pick.hidden = s.value !== 'own';
    if (s.value === 'own') c[key] = pick.value; else if (s.value) c[key] = s.value; else delete c[key];
  };
  pick.oninput = () => { c[key] = pick.value; };
  return el('span', { className: 'row' }, s, pick);
}
const OPTION_LISTS = { 'select-options': 'options', 'climate-hvac-modes': 'hvac_modes', 'climate-preset-modes': 'preset_modes',
  'climate-fan-modes': 'fan_modes', 'climate-swing-modes': 'swing_modes', 'fan-preset-modes': 'preset_modes' };
const MAX_OPTIONS = 5;
const MAX_VIEWS = 32;	/* pages the Toon shows (dash.c) */

let dash = { views: [] }, saved = '', cur = 0, entities = [], topics = [], apps = [];
let curDim = false;	/* editing the home page's dimmed page */
/* Home Assistant set up (address and a login or token): only then ask it for entities */
let haSet = false;

/* ---- talking to the Toon ------------------------------------------------ */

async function api(path, body) {
  const opt = body === undefined ? {} :
    { method: 'POST', headers: { 'Content-Type': 'application/x-toon' }, body };
  const r = await fetch(`${API}/${path}`, opt);
  if (!r.ok) throw new Error((await r.text()).trim() || r.statusText);
  return r;
}
const lines = o => Object.entries(o).map(([k, v]) => `${k}=${String(v).replace(/[\r\n]/g, '')}`).join('\n') + '\n';

function say(text, cls = '') {
  const m = $('#msg');
  m.textContent = text;
  m.dataset.en = text;	/* what was said, before translation */
  m.className = cls;
  if (cls === 'ok' && lastForm) {	/* that form's changes are saved */
    for (const e of lastForm.querySelectorAll('[data-edited]')) delete e.dataset.edited;
    delete lastForm.dataset.dirty;
    lastForm = null;
    armForms();
  }
  if (cls === 'ok') setTimeout(() => { if (m.dataset.en === text) { m.textContent = ''; m.dataset.en = ''; } }, 4000);
}
const fail = e => say(e.message || String(e), 'bad');

function dirty() {
  const d = JSON.stringify(dash) !== saved;
  $('#save').disabled = $('#revert').disabled = !d;
  if (d) say('Not saved yet');
  else if ($('#msg').dataset.en === 'Not saved yet') say('');
}

async function loadDash() {
  dash = await (await api('dashboard')).json();
  if (!Array.isArray(dash.views)) dash.views = [];
  saved = JSON.stringify(dash);
  /* older pages (the standard ones) as grids; saving them keeps that */
  dash.views.forEach(toGrid);
  cur = Math.min(cur, Math.max(0, dash.views.length - 1));
  render();
  dirty();
}

async function saveDash() {
  try {
    await api('dashboard', JSON.stringify(dash));
    saved = JSON.stringify(dash);
    dirty();
    say('Saved', 'ok');
    await api('ui', `view ${cur}\n`);
    setTimeout(refreshScreen, 1500);
  } catch (e) { fail(e); }
}

/* ---- what can be shown: the Toon's own values, Home Assistant, MQTT ------ */

/* The Toon asks Home Assistant itself, so the access token never leaves it. */
async function haAsk(path) {
  const m = await (await api(path)).json();
  if (m.success === false) throw new Error(m.error && m.error.message || 'Home Assistant error');
  return 'result' in m ? m.result : m;
}

/* what a value of a meter adapter is, from its key in retoon/meter:
   "n<node>_<what>" (toon-zwaved names the endpoints of the Toon's adapter) */
const METER_NAMES = { gas: 'Gas', gas_flow: 'Gas flow', pulse_import: 'Electricity import (meter sensor)',
  pulse_export: 'Electricity export (meter sensor)', import_t1: 'Electricity import tariff 1',
  import_t2: 'Electricity import tariff 2', export_t1: 'Electricity export tariff 1',
  export_t2: 'Electricity export tariff 2', heat: 'Heat' };
function meterName(key) {
  const what = key.replace(/^n\d+_/, ''), base = what.replace(/_power$/, '');
  const n = METER_NAMES[base] ? T(METER_NAMES[base]) : what;
  return what !== base ? T`${n} power` : n;
}
const meterUnit = what => /_power$/.test(what) ? 'kW' : /_flow$/.test(what) ? 'm³/h' : what === 'gas' ? 'm³' :
  /^heat|_s\d+$/.test(what) ? '' : 'kWh';

async function loadEntities() {
  const local = await (await fetch(BASE + 'local-entities.json')).json();
  /* src: where an entity comes from, shown in the search list */
  /* a Toon 1 measures only the temperature: no humidity, light or air quality */
  const none = lastSettings.board === 'toon1' ? ['toon.humidity', 'toon.illuminance', 'toon.air_quality'] : [];
  const list = local.filter(e => !none.includes(e.id))
    .map(e => ({ id: e.id, name: T(e.name), en: e.name, state: '', local: true, src: 'ReToon' }));
  /* the boiler's other OpenTherm values: "key=value|unit|...|name;..." */
  for (const l of (lastSettings.opentherm || '').split(';').filter(Boolean)) {
    const [k, rest] = l.split('='), [v, unit, , , name] = (rest || '').split('|');
    if (k && name) list.push({ id: 'toon.' + k, name: T(name), en: name, state: `${v} ${unit}`.trim(), local: true, src: 'ReToon' });
  }
  /* the meter adapters that are read: their values are the Toon's own too
     (asked only when one is ticked: every request costs a Toon 1 a moment) */
  if (meterNodes().length) try {
    const m = await (await api('meter')).json(), nodes = new Set(Object.keys(m).map(k => k.split('_')[0]));
    for (const [k, v] of Object.entries(m))
      list.push({ id: 'toon.meter_' + k, name: meterName(k) + (nodes.size > 1 ? ` (${k.split('_')[0].slice(1)})` : ''),
        en: k, state: `${v} ${meterUnit(k.replace(/^n\d+_/, ''))}`.trim(), local: true, src: 'ReToon' });
  } catch (e) { /* no meter adapters */ }
  try {
    for (const e of await (await api('mqtt/entities')).json())
      list.push({ id: e.id, name: e.name, state: e.topic, kind: e.kind, src: 'MQTT' });
  } catch (e) { /* no broker */ }
  if (haSet) {
    try {
      list.push(...(await haAsk('ha/entities')).map(e => ({ ...e, src: 'Home Assistant' })));
    } catch (e) { /* the ReToon's own values still work */ }
  }
  entities = list.sort((a, b) => a.name.localeCompare(b.name));
  render();
}
/* a name with its source, as before for the Toon's own and MQTT ones */
const entName = id => {
  const e = entities.find(x => x.id === id);
  return !e ? id : e.src && e.src !== 'Home Assistant' ? `${e.name} (${e.src})` : e.name;
};

/* ---- the page editor ---------------------------------------------------- */

const view = () => curDim && dash.views[0] && dash.views[0].dimmed ? dash.views[0].dimmed : dash.views[cur];
/* the dimmed page belongs to the home page, the first one: it moves along */
function keepDimHome() {
  const i = dash.views.findIndex(v => v.dimmed);
  if (i > 0 && dash.views[0] && !dash.views[0].dimmed) {
    dash.views[0].dimmed = dash.views[i].dimmed;
    delete dash.views[i].dimmed;
  }
  if (!dash.views[0] || !dash.views[0].dimmed) curDim = false;
}
const newDimmed = () => ({ type: 'retoon-grid',
  cards: [{ type: 'custom:toon-clock', position: { x: 0, y: 0, w: 12, h: 8 } }] });

/* ---- pages as a grid --------------------------------------------------------
   A page is a grid of 12 columns over its width; a row is a tile high (on
   the screen 7 rows above the bar, 8 on the dimmed page). Each card has its
   place: position { x, y, w, h } in cells. */
const GRID_COLS = 12;
const pageRows = () => curDim ? 8 : 7;
const posOf = c => c.position;
function overlaps(a, b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
/* does c fit at p (x, y, w, h) among cards, without touching another? */
function fits(cards, c, p) {
  return p.x >= 0 && p.y >= 0 && p.w >= 1 && p.h >= 1 && p.x + p.w <= GRID_COLS &&
    cards.every(o => o === c || !o.position || !overlaps(o.position, p));
}
/* the first free place of that size: from the top, left to right */
function freePlace(cards, w, h) {
  for (let y = 0; ; y++)
    for (let x = 0; x + w <= GRID_COLS; x++)
      if (fits(cards, null, { x, y, w, h })) return { x, y, w, h };
}
/* a new card's size: a quarter of the page for a small card, else a half */
function defaultSize(c) {
  const r = sizeRules(c);
  const w = c.type === 'thermostat' && (c.style || 'dial') === 'dial' ? 6 : SMALL.includes(c.type) ? 3 : 6;
  return { w: Math.max(r.min[0], w), h: Math.max(r.min[1], r.auto) };
}

/* A page in Home Assistant's layout (sections, a list of cards; a copied
   page, or the standard pages) to the grid: the sections side by side as
   their columns say, the cards in each one in order, each in the first
   place where it fits, as the screen showed them. */
function toGrid(v) {
  if (v.dimmed) toGrid(v.dimmed);
  if (v.type === 'retoon-grid') { v.cards = v.cards || []; return v; }
  const pcols = Math.min(4, Math.max(1, v.max_columns || v.columns || 2));
  const secs = Array.isArray(v.sections) ? v.sections : [{ cards: v.cards || [] }];
  const out = [];
  let sx = 0, sy = 0, rowH = 0;
  for (const sec of secs) {
    const span = Math.min(pcols, sec.column_span || 1), sw = Math.max(1, Math.round(GRID_COLS * span / pcols));
    if (sx + sw > GRID_COLS) { sx = 0; sy += rowH; rowH = 0; }
    const local = [], put = (c, w, h) => {
      w = Math.min(w, sw);
      for (let y = 0; ; y++)
        for (let x = 0; x + w <= sw; x++) {
          const p = { x, y, w, h };
          if (local.every(q => !overlaps(q, p))) {
            local.push(p);
            c.position = { x: sx + x, y: sy + y, w, h };
            out.push(c);
            return;
          }
        }
    };
    if (sec.title) put({ type: 'heading', heading: sec.title }, sw, 1);
    for (const c of sec.cards || []) {
      const g = c.grid_options || c.layout_options || {};
      const gc = g.columns !== undefined ? g.columns : g.grid_columns, gr = g.rows !== undefined ? g.rows : g.grid_rows;
      const cols = typeof gc === 'number' ? gc : gc === 'full' ? 12 : SMALL.includes(c.type) ? 6 : 12;
      const r = sizeRules(c);
      delete c.grid_options; delete c.layout_options;
      put(c, Math.max(1, Math.round(cols * sw / 12)), typeof gr === 'number' ? Math.max(1, gr) : Math.max(r.min[1], r.auto));
    }
    const h = local.reduce((m, p) => Math.max(m, p.y + p.h), 0);
    sx += sw;
    rowH = Math.max(rowH, h);
  }
  delete v.sections; delete v.columns; delete v.max_columns;
  v.type = 'retoon-grid';
  v.cards = out;
  return v;
}
const cardsOf = v => toGrid(v).cards;
function move(arr, i, d) {
  const j = i + d;
  if (j < 0 || j >= arr.length) return;
  [arr[i], arr[j]] = [arr[j], arr[i]];
}
const iconBtn = (txt, title, fn, cls = '') => el('button', { className: 'icon ' + cls, textContent: txt, title,
  onclick: ev => { ev.stopPropagation(); fn(); render(); dirty(); } });

function cardSummary(c) {
  const t = c.type || '';
  if (t === 'heading') return [c.heading || 'Heading', 'Heading'];
  if (t === 'markdown') return [(c.content || '').split('\n')[0].slice(0, 60) || 'Text', 'Text'];
  if (t === 'custom:toon-clock') return ['Clock', c.style === 'analog' ? 'Analogue (hands)' : 'Clock'];
  if (ENERGY[t]) return [c.title || ENERGY[t], 'Energy · from Home Assistant'];
  if (t === 'custom:retoon-app') {
    const a = apps.find(x => x.dir === c.app);
    return [a ? a.manifest.name || c.app : c.app || 'App', a ? 'App' : 'App · not installed'];
  }
  if (Array.isArray(c.entities) && /graph/.test(t))
    return [c.title || c.entities.map(e => entName(typeof e === 'string' ? e : e.entity)).join(', ') || 'Graph', TYPES[t] || t];
  if (Array.isArray(c.entities)) return [c.title || `${c.entities.length} entities`, TYPES[t] || t];
  if (c.entity) return [c.name && typeof c.name === 'string' ? c.name : entName(c.entity), `${TYPES[t] || t} · ${c.entity}`];
  if (Array.isArray(c.cards)) return [`${c.cards.length} cards`, t];
  return [t || 'Card', 'not shown on the ReToon'];
}
const shown = c => SUPPORTED.includes(c.type) || c.entity || Array.isArray(c.entities);

function render() {
  keepDimHome();
  const vl = $('#views');
  vl.replaceChildren(...dash.views.flatMap((v, i) => [el('li', { className: i === cur && !curDim ? 'on' : '', onclick: () => selectView(i) },
    el('span', { className: 't', textContent: v.title || T`Page ${i + 1}`, translate: false }),
    iconBtn('↑', 'Move up', () => { move(dash.views, i, -1); if (cur === i) cur--; else if (cur === i - 1) cur++; }),
    iconBtn('↓', 'Move down', () => { move(dash.views, i, 1); if (cur === i) cur++; else if (cur === i + 1) cur--; }),
    iconBtn('✕', 'Remove this page', () => {
      if (!confirm(T`Remove the page "${v.title || i + 1}"?`)) return;
      dash.views.splice(i, 1);
      cur = Math.max(0, Math.min(cur, dash.views.length - 1));
    }, 'danger')),
    ...(i === 0 && v.dimmed ? [el('li', { className: 'sub' + (curDim ? ' on' : ''), onclick: selectDim },
      el('span', { className: 't', textContent: '↳ ' + T('Home page (dimmed)') }))] : [])]));
  cur = Math.max(0, Math.min(cur, dash.views.length - 1));

  const v = view(), props = $('#view-props'), secs = $('#sections');
  $('#add-section').hidden = !v;
  if (!v) {
    props.replaceChildren();
    secs.replaceChildren(el('p', { className: 'hint', textContent: 'Add a page, or copy one with "Copy pages from Home Assistant".' }));
    $('#cards-title').textContent = 'Cards';
    return;
  }
  if (curDim) {
    $('#cards-title').textContent = T`Cards on "${T('Home page (dimmed)')}"`;
    props.replaceChildren(
      el('p', { className: 'hint', textContent: 'Shown while the screen is dimmed (Screen, Brightness). A touch wakes the screen and shows the home page, without pressing anything.' }));
  } else
  props.replaceChildren(
    field('Name', v.title || '', x => {
      v.title = x;	/* update in place: redrawing would take the focus away */
      const li = $('#views li.on .t');
      if (li) li.textContent = x || T`Page ${cur + 1}`;
      $('#cards-title').textContent = T`Cards on "${x || 'this page'}"`;
    }),
    field('Icon', v.icon || '', x => { if (x) v.icon = x; else delete v.icon; }, 'mdi:home'),
    el('p', { className: 'hint' }, 'Icon names come from ',
      el('a', { href: 'https://pictogrammers.com/library/mdi/', target: '_blank', textContent: 'Material Design Icons', style: 'color:var(--accent)' }),
      ', for example mdi:sofa.'),
    ...(cur === 0 ? [(() => {
      const cb = el('input', { type: 'checkbox', checked: !!v.dimmed });
      cb.onchange = () => {
        if (cb.checked) { v.dimmed = newDimmed(); curDim = true; }
        else if (!cardsOf(v.dimmed).length || confirm(T('Remove the dimmed page and its cards?'))) { delete v.dimmed; curDim = false; }
        else cb.checked = true;
        dirty(); render();
        if (curDim) api('ui', 'view dim\n').then(() => setTimeout(refreshScreen, 700)).catch(() => {});
      };
      return el('label', { className: 'check' }, cb, el('span', { textContent: 'Different page when the screen is dimmed' }));
    })()] : []));

  const cards = cardsOf(v);
  const rows = Math.max(pageRows(), ...cards.map(c => c.position.y + c.position.h)) + 1;
  const grid = el('div', { className: 'pgrid' });
  grid.cards = cards;
  grid.style.gridTemplateRows = `repeat(${rows}, ${ROW_PX}px)`;
  /* the empty cells: a click adds a card there */
  for (let y = 0; y < rows; y++)
    for (let x = 0; x < GRID_COLS; x++)
      if (cards.every(c => !overlaps(c.position, { x, y, w: 1, h: 1 })))
        grid.append(el('div', { className: 'cell', title: 'Add a card here', style: `grid-column:${x + 1};grid-row:${y + 1}`,
          onclick: () => editCard(cards, -1, { x, y }) }));
  grid.append(...cards.map((c, ci) => gridCard(cards, ci)));
  /* where the Toon's screen ends without scrolling */
  grid.append(el('div', { className: 'fold', style: `top:${pageRows() * (ROW_PX + GAP_PX) - GAP_PX / 2}px` }));
  secs.replaceChildren(grid);
}

/* ---- the cards in the grid -------------------------------------------------- */

const SMALL = ['tile', 'button', 'entity', 'sensor', 'light'];
const ROW_PX = 46, GAP_PX = 7;	/* a row and the gap between cells in the editor */
let screenSize = [1024, 600];	/* the Toon's, as its screen picture says */

/* the rows a card shows itself in on the Toon (its first size), and how far
   it can be made smaller or larger ([columns, rows], columns of the page's
   12); fixed: its height cannot change */
function sizeRules(c) {
  const t = c.type || '';
  let auto = 2, min = [2, 1], max = [12, 16], fixed = false;
  if (t === 'tile') {
    auto = (c.features || []).some(f => f.type && FEATURES[f.type]) ? 2 : 1;
    min = [2, auto];
  } else if (t === 'heading') {
    auto = 1; fixed = true;
  } else if (t === 'thermostat') {
    const st = c.style || 'dial';
    if (st === 'dial') { auto = 6; min = [3, 4]; }
    else { auto = { compact: 3, bar: 2, classic: 5 }[st] || 1; min = [3, auto]; }
  } else if (t === 'custom:retoon-app') {
    const a = apps.find(x => x.dir === c.app), tile = a && a.manifest.tile;
    auto = Math.max(1, Math.round(((c.height || (tile && tile.height) || 150) + 10) / 73));
  } else if (t === 'gauge') {
    /* the Toon rounds it up to whole rows: about 3 when half a section
       wide or more, 2 when narrower; one row still fits (no name then) */
    const gc = (c.grid_options || {}).columns;
    auto = (typeof gc === 'number' ? gc : gc === 'full' ? 12 : 6) >= 5 ? 3 : 2;
    min = [3, 1];
  } else if (t === 'energy-distribution') {
    auto = 7; min = [4, 5];
  } else if (ENERGY[t]) {
    auto = 5; min = [3, 3];
  } else if (t === 'history-graph' || t === 'statistics-graph' || (t === 'sensor' && c.graph === 'line')) {
    auto = 3; min = [3, 2];
  } else if (Array.isArray(c.entities)) {
    auto = Math.max(1, Math.round(((c.title ? 48 : 8) + 44 * c.entities.length + 18) / 73));
  } else if (t === 'markdown') {
    auto = Math.max(1, Math.round((38 + 24 * Math.max(1, (c.content || '').split('\n').length)) / 73));
  } else if (t === 'custom:toon-clock') {
    min = [2, 2];
  } else if (c.entity && !['grid', 'vertical-stack', 'horizontal-stack', 'conditional'].includes(t)) {
    auto = 1;	/* a card the Toon shows as a tile (a camera, the weather...) */
  }
  return { auto, min, max, fixed };
}

function placeCard(e, c) {
  const p = c.position;
  e.style.gridColumn = `${p.x + 1} / span ${p.w}`;
  e.style.gridRow = `${p.y + 1} / span ${p.h}`;
  e.classList.toggle('one', p.h === 1);
  if (e.size) e.size.textContent = `${p.w} × ${p.h}`;
}

function gridCard(cards, ci) {
  const c = cards[ci], [title, sub] = cardSummary(c), rules = sizeRules(c);
  const e = el('div', { className: 'gcard' + (shown(c) ? '' : ' unsupported') },
    el('div', { className: 't' }, el('b', { textContent: title, translate: false }), el('small', { textContent: sub })),
    el('small', { className: 'size' }),
    el('span', { className: 'grip', textContent: '⠿', title: 'Drag to move' }),
    el('div', { className: 'tools' },
      iconBtn('✕', 'Remove', () => cards.splice(ci, 1), 'danger')),
    el('span', { className: 'rs', title: 'Drag to change the size' }));
  e.size = e.querySelector('.size');
  e.card = c;
  placeCard(e, c);
  e.addEventListener('click', ev => {
    if (e.dragged || ev.target.closest('button, .rs')) return;
    editCard(cards, cards.indexOf(c));
  });
  e.addEventListener('pointerdown', ev => {
    if (ev.button !== 0 || ev.target.closest('button')) return;
    if (ev.target.classList.contains('rs')) return startResize(ev, e, cards, c, rules);
    /* a finger moves the page, except on the grip */
    if (ev.pointerType !== 'mouse' && !ev.target.classList.contains('grip')) return;
    startMove(ev, e, cards, c);
  });
  return e;
}

/* a cell of the grid in pixels of the editor */
function cellPx(grid) {
  return { col: (grid.clientWidth + GAP_PX) / GRID_COLS, row: ROW_PX + GAP_PX };
}

/* bigger or smaller, a cell at a time, as far as it fits next to the others */
function startResize(ev, e, cards, c, rules) {
  ev.preventDefault();
  const grid = e.parentElement, px = cellPx(grid), p0 = { ...c.position }, x0 = ev.clientX, y0 = ev.clientY;
  e.setPointerCapture(ev.pointerId);
  e.classList.add('sizing');
  const moveFn = m => {
    const w = Math.min(GRID_COLS - p0.x, rules.max[0], Math.max(rules.min[0], Math.round(p0.w + (m.clientX - x0) / px.col)));
    const h = rules.fixed ? p0.h : Math.min(rules.max[1], Math.max(rules.min[1], Math.round(p0.h + (m.clientY - y0) / px.row)));
    /* each way on its own: blocked one way, it can still change the other */
    for (const p of [{ ...p0, w, h }, { ...c.position, w }, { ...c.position, h }])
      if (fits(cards, c, p)) { c.position = p; break; }
    placeCard(e, c);
  };
  const up = () => {
    e.removeEventListener('pointermove', moveFn);
    e.removeEventListener('pointerup', up);
    e.removeEventListener('pointercancel', up);
    e.classList.remove('sizing');
    e.dragged = true;
    setTimeout(() => { e.dragged = false; });
    render();
    dirty();
  };
  e.addEventListener('pointermove', moveFn);
  e.addEventListener('pointerup', up);
  e.addEventListener('pointercancel', up);
}

/* to any free place: the outline shows where it lands (red: not there) */
function startMove(ev, e, cards, c) {
  const grid = e.parentElement, px = cellPx(grid), x0 = ev.clientX, y0 = ev.clientY;
  const r0 = e.getBoundingClientRect(), dx = x0 - r0.left, dy = y0 - r0.top;
  let ghost = null, drop = null, to = null;
  const moveFn = m => {
    if (!ghost) {
      if (Math.hypot(m.clientX - x0, m.clientY - y0) < 6) return;
      ghost = e.cloneNode(true);
      ghost.classList.add('ghost');
      Object.assign(ghost.style, { width: r0.width + 'px', height: r0.height + 'px' });
      document.body.append(ghost);
      drop = el('div', { className: 'drop' });
      grid.append(drop);
      e.classList.add('moving');
    }
    ghost.style.left = (m.clientX - dx) + 'px';
    ghost.style.top = (m.clientY - dy) + 'px';
    const g = grid.getBoundingClientRect(), p = c.position;
    const x = Math.min(GRID_COLS - p.w, Math.max(0, Math.round((m.clientX - dx - g.left) / px.col)));
    const y = Math.max(0, Math.round((m.clientY - dy - g.top) / px.row));
    to = { x, y, w: p.w, h: p.h };
    const ok = fits(cards, c, to);
    drop.classList.toggle('bad', !ok);
    drop.style.gridColumn = `${x + 1} / span ${p.w}`;
    drop.style.gridRow = `${y + 1} / span ${p.h}`;
    if (!ok) to = null;
  };
  const up = () => {
    window.removeEventListener('pointermove', moveFn);
    window.removeEventListener('pointerup', up);
    window.removeEventListener('pointercancel', up);
    if (!ghost) return;
    ghost.remove();
    drop.remove();
    e.classList.remove('moving');
    e.dragged = true;
    setTimeout(() => { e.dragged = false; });
    if (!to) return;
    c.position = to;
    render();
    dirty();
  };
  window.addEventListener('pointermove', moveFn);
  window.addEventListener('pointerup', up);
  window.addEventListener('pointercancel', up);
}

function field(label, value, set, placeholder = '') {
  const inp = el('input', { value, placeholder });
  inp.oninput = () => { set(inp.value.trim()); dirty(); };
  return el('label', { textContent: label }, inp);
}

function selectView(i) {
  cur = i;
  curDim = false;
  render();
  api('ui', `view ${i}\n`).then(() => setTimeout(refreshScreen, 700)).catch(() => {});
}

function selectDim() {
  cur = 0;
  curDim = true;
  render();
  api('ui', 'view dim\n').then(() => setTimeout(refreshScreen, 700)).catch(() => {});
}

/* ---- card dialog --------------------------------------------------------- */

function entityPicker(get, set, filter) {
  const inp = el('input', { placeholder: 'Search by name…', value: get() ? `${entName(get())}` : '' });
  const box = el('div', { className: 'picker', hidden: true });
  const show = () => {
    const q = inp.value.toLowerCase().split(/\s+/).filter(Boolean);
    const full = inp.value.trim().toLowerCase();
    /* the Toon's own entities first (they are few and easily lost among
       Home Assistant's), found by their translated and English names */
    const names = e => [e.name.toLowerCase(), (e.en || '').toLowerCase(), (e.src || '').toLowerCase()];
    const rank = e => { const n = names(e); return n.some(x => x.startsWith(full)) ? 0 : n.some(x => x.includes(full)) ? 1 : 2; };
    const hits = entities.filter(e => (!filter || filter(e)) &&
      q.every(w => names(e).some(x => x.includes(w)) || e.id.includes(w)))
      .sort((a, b) => (b.local ? 1 : 0) - (a.local ? 1 : 0) || rank(a) - rank(b)).slice(0, 60);
    box.hidden = false;
    box.replaceChildren(...hits.map(e => el('div', { onclick: () => { set(e.id); inp.value = entName(e.id); box.hidden = true; } },
      el('span', { textContent: e.name }), el('small', { textContent: e.state || e.id }),
      el('small', { className: 'src src-' + (e.src || '').replace(/ /g, '-').toLowerCase(), textContent: e.src || '' }))));
    if (!hits.length) box.replaceChildren(el('div', {}, el('small', { textContent: 'Nothing found. You can also type an entity id.' })));
  };
  inp.oninput = () => { if (/^[a-z_]+\.[a-z0-9_]+$/.test(inp.value.trim())) set(inp.value.trim()); show(); };
  inp.onfocus = show;
  return [inp, box];
}

/* which options of the entity get a button, and in which order: the
   feature's own list (Home Assistant's "options"); none ticked = all */
function optionPicker(c, feat) {
  const key = OPTION_LISTS[feat.type];
  const e = entities.find(x => x.id === c.entity) || {};
  const have = Array.isArray(e[key]) ? e[key].filter(o => !(key === 'preset_modes' && o === 'none')) : [];
  const box = el('div', { className: 'app-list' });
  /* the rows: the chosen ones first, in their order, then the rest */
  let chosen = Array.isArray(feat[key]) ? [...feat[key]] : [];
  const save = () => { if (chosen.length) feat[key] = [...chosen]; else delete feat[key]; };
  const drawRows = () => {
    const rows = [...chosen, ...have.filter(o => !chosen.includes(o))];
    box.replaceChildren(...rows.map(o => {
      const on = chosen.includes(o), i = chosen.indexOf(o);
      const cb = el('input', { type: 'checkbox', checked: on });
      cb.onchange = () => {
        if (cb.checked && chosen.length >= MAX_OPTIONS) { cb.checked = false; return alert(T`At most ${MAX_OPTIONS} buttons fit.`); }
        chosen = cb.checked ? [...chosen, o] : chosen.filter(x => x !== o);
        save(); drawRows();
      };
      const move = (d, label) => el('button', { className: 'icon', type: 'button', textContent: label, disabled: !on || !chosen[i + d],
        onclick: () => { [chosen[i], chosen[i + d]] = [chosen[i + d], chosen[i]]; save(); drawRows(); } });
      return el('div', { className: 'app-row' }, el('label', { className: 'check' }, cb, el('span', { translate: false, textContent: o })),
        move(-1, '▲'), move(1, '▼'));
    }));
  };
  if (!have.length) {
    /* the entity does not say (yet): type them */
    const t = el('input', { value: chosen.join(', '), placeholder: 'Fast, Eco, Stopped' });
    t.oninput = () => { chosen = t.value.split(',').map(x => x.trim()).filter(Boolean).slice(0, MAX_OPTIONS); save(); };
    return el('div', {}, el('label', { textContent: 'Buttons (as the entity calls them, separated by commas; empty = all)' }, t));
  }
  drawRows();
  return el('div', {}, el('label', { textContent: 'Buttons (none ticked = all, at most 5)' }), box);
}

/* what a tap on a tile does: its own thing, nothing, or a Home Assistant
   action (tap_action "perform-action"), optionally after "Are you sure?" */
function tapEditor(c) {
  const ta = c.tap_action || {};
  const kind = ta.action === 'none' ? 'none' : ta.action === 'perform-action' || ta.action === 'call-service' ? 'action' : '';
  const sel = el('select', {}, [['', 'Its usual thing (switch, press...)'], ['none', 'Nothing'], ['action', 'Perform an action']]
    .map(([k, v]) => el('option', { value: k, textContent: v, selected: kind === k })));
  const out = [el('label', { textContent: 'When tapped' }, sel)];
  const act = el('div', { hidden: kind !== 'action' });
  sel.onchange = () => {
    if (!sel.value) delete c.tap_action;
    else if (sel.value === 'none') c.tap_action = { action: 'none' };
    else c.tap_action = { action: 'perform-action', perform_action: '', target: { entity_id: c.entity || '' } };
    act.hidden = sel.value !== 'action';
    if (sel.value === 'action') fill();
  };
  const fill = () => {
    const a = c.tap_action;
    const svc = el('input', { value: a.perform_action || a.service || '', placeholder: 'script.turn_on' });
    svc.oninput = () => { a.perform_action = svc.value.trim(); delete a.service; };
    const tgt = el('input', { value: (a.target && a.target.entity_id) || '', placeholder: c.entity || 'select.charger_mode' });
    tgt.oninput = () => { if (tgt.value.trim()) a.target = { entity_id: tgt.value.trim() }; else delete a.target; };
    const data = el('textarea', { value: a.data ? JSON.stringify(a.data, null, 1) : '', placeholder: '{ "amount": 10 }', spellcheck: false });
    data.oninput = () => {
      if (!data.value.trim()) { delete a.data; data.style.borderColor = ''; return; }
      try { const o = JSON.parse(data.value); if (typeof o !== 'object' || Array.isArray(o)) throw 0; a.data = o; data.style.borderColor = ''; }
      catch (e) { data.style.borderColor = 'var(--bad)'; }
    };
    const ask = el('input', { type: 'checkbox', checked: !!a.confirmation });
    const askText = el('input', { value: (a.confirmation && a.confirmation.text) || '', placeholder: 'Are you sure?' });
    const setAsk = () => {
      if (!ask.checked) delete a.confirmation;
      else a.confirmation = askText.value.trim() ? { text: askText.value.trim() } : true;
      askText.disabled = !ask.checked;
    };
    ask.onchange = setAsk;
    askText.oninput = setAsk;
    askText.disabled = !ask.checked;
    act.replaceChildren(
      el('label', { textContent: 'Action (domain.action)' }, svc),
      el('label', { textContent: 'On entity (empty = none)' }, tgt),
      el('label', { textContent: 'Data, as JSON (empty = none)' }, data),
      el('label', { className: 'check' }, ask, 'Ask first'),
      el('label', { textContent: 'Question' }, askText));
  };
  if (kind === 'action') fill();
  out.push(act);
  return out;
}

function editCard(cards, index, at) {
  const isNew = index < 0;
  const c = isNew ? { type: 'tile' } : JSON.parse(JSON.stringify(cards[index]));
  const body = $('#dlg-body'), dlg = $('#dlg');
  const KINDS = { ...TYPES, ...ENERGY };
  let raw = !isNew && !KINDS[c.type];

  let cardInputs = null;	/* an app card's own settings (the manifest's cardSettings) */
  const draw = () => {
    const kids = [];
    cardInputs = null;
    if (raw) {
      const ta = el('textarea', { value: JSON.stringify(c, null, 2), spellcheck: false });
      ta.oninput = () => {
        try { const o = JSON.parse(ta.value); for (const k in c) delete c[k]; Object.assign(c, o); ta.style.borderColor = ''; }
        catch (e) { ta.style.borderColor = 'var(--bad)'; }
      };
      kids.push(el('p', { className: 'hint', textContent: shown(c) ? 'Card settings as Home Assistant writes them.' :
        T`The ReToon cannot show a "${c.type}" card. You can keep it, change it here, or remove it.` }), ta);
    } else {
      const type = el('select', {}, Object.entries(KINDS).map(([k, v]) => el('option', { value: k, textContent: v, selected: c.type === k })));
      type.onchange = () => { c.type = type.value; draw(); };
      kids.push(el('label', { textContent: 'Kind of card' }, type));
      const text = (label, key, ph = '') => {
        const i = el('input', { value: typeof c[key] === 'string' ? c[key] : '', placeholder: ph });
        i.oninput = () => { if (i.value.trim()) c[key] = i.value.trim(); else delete c[key]; };
        return el('label', { textContent: label }, i);
      };
      if (c.type === 'tile' || c.type === 'thermostat') {
        const want = c.type === 'thermostat' ? e => e.id.startsWith('climate.') || e.id === 'toon.thermostat' : null;
        kids.push(el('label', { textContent: 'Shows' }), ...entityPicker(() => c.entity, id => {
          c.entity = id;
          if (OPTION_LISTS[((c.features || [])[0] || {}).type] && entities.some(e => e.id === id)) setTimeout(draw);
          /* the display is of little use without its slider: put it there */
          if (id === 'toon.display' && c.type === 'tile' && !c.features) { c.features = [{ type: 'light-brightness' }]; draw(); }
        }, want),
          text('Name (empty = its own name)', 'name'));
      }
      if (c.type === 'gauge') {
        /* a number field: stored as a number, empty = not set */
        const num = (label, key, ph) => {
          const i = el('input', { value: typeof c[key] === 'number' ? String(c[key]) : '', placeholder: ph, inputmode: 'decimal' });
          i.oninput = () => { const v = parseFloat(i.value.replace(',', '.')); if (isNaN(v)) delete c[key]; else c[key] = v; };
          return el('label', { textContent: label }, i);
        };
        const sev = (label, key) => {
          const s = c.severity || {};
          const i = el('input', { value: typeof s[key] === 'number' ? String(s[key]) : '', inputmode: 'decimal' });
          i.oninput = () => {
            const v = parseFloat(i.value.replace(',', '.'));
            c.severity = c.severity || {};
            if (isNaN(v)) delete c.severity[key]; else c.severity[key] = v;
            if (!Object.keys(c.severity).length) delete c.severity;
            /* segments win over severity on the screen: these colours replace them */
            delete c.segments;
          };
          return el('label', { textContent: label }, i);
        };
        const needle = el('input', { type: 'checkbox', checked: !!c.needle });
        needle.onchange = () => { if (needle.checked) c.needle = true; else delete c.needle; };
        kids.push(el('label', { textContent: 'Shows' }), ...entityPicker(() => c.entity, id => { c.entity = id; }),
          text('Name (empty = its own name)', 'name'),
          num('Minimum', 'min', '0'), num('Maximum', 'max', '100'), text('Unit (empty = its own)', 'unit'),
          el('label', { className: 'check' }, needle, 'Needle'),
          el('p', { className: 'hint', textContent: 'Colours: the value from which the ring turns green, yellow or red. Leave them empty for a blue ring.' }),
          ...(Array.isArray(c.segments) ? [el('p', { className: 'hint', textContent:
            'This card has its own colour segments (from Home Assistant). Filling in a colour here replaces them.' })] : []),
          sev('Green from', 'green'), sev('Yellow from', 'yellow'), sev('Red from', 'red'));
      }
      if (c.type === 'thermostat') {
        const STYLES = { dial: 'Large dial (fills its section)', compact: 'Small dial', bar: 'Bar with a slider', row: 'One line with − and +', classic: 'Classic (no dial, presets two a row)' };
        const st = el('select', {}, Object.entries(STYLES).map(([k, v]) => el('option', { value: k, textContent: v, selected: (c.style || 'dial') === k })));
        st.onchange = () => { if (st.value === 'dial') delete c.style; else c.style = st.value; draw(); };
        kids.push(el('label', { textContent: 'Style' }, st));
        if (c.style && c.style !== 'dial' && c.style !== 'classic') {
          kids.push(text('Icon (empty = automatic)', 'icon', 'mdi:thermostat'));
          if (c.style === 'compact') {
            /* as Home Assistant's show_name: off leaves out icon and name; the
               ring grows with the rows the card gets */
            const sn = el('input', { type: 'checkbox', checked: c.show_name !== false });
            sn.onchange = () => { if (sn.checked) delete c.show_name; else c.show_name = false; };
            kids.push(el('label', { className: 'check' }, sn, 'Show the name'));
          }
        } else {
          /* the buttons under the dial: as Home Assistant's features */
          const feat = (c.features || []).find(f => OPTION_LISTS[f.type]) || {};
          /* ReToon's own without a choice of its own follows the old setting
             for every card (Screen, before 1.81): off shows here as None */
          const own = c.entity === 'toon.thermostat', oldOff = own && lastSettings.thermostat_buttons === '0';
          /* none: an empty features list, as a Home Assistant card without features */
          const none = Array.isArray(c.features) ? !c.features.length : oldOff;
          const BTNS = { '': 'Automatic (presets, else modes)', 'climate-hvac-modes': 'Heating mode buttons', 'climate-preset-modes': 'Preset buttons',
            'climate-fan-modes': 'Fan speed buttons', 'climate-swing-modes': 'Swing buttons', none: 'None' };
          const bs = el('select', {}, Object.entries(BTNS).map(([k, v]) => el('option', { value: k, textContent: v,
            selected: none ? k === 'none' : (feat.type || '') === k })));
          bs.onchange = () => {
            if (bs.value === 'none') c.features = [];
            else if (bs.value) c.features = [{ type: bs.value }];
            /* automatic would follow the old "off" again: the presets */
            else if (oldOff) c.features = [{ type: 'climate-preset-modes' }];
            else delete c.features;
            draw();
          };
          kids.push(el('label', { textContent: 'Buttons under the thermostat' }, bs));
          if (own) kids.push(el('p', { className: 'hint', textContent: 'With the buttons comes the week program button. None: only the temperature, set with − and + (or from Home Assistant).' }));
          if (feat.type) kids.push(optionPicker(c, feat));
        }
      }
      if (c.type === 'custom:toon-clock') {
        const st = el('select', {}, el('option', { value: 'digital', textContent: 'Digital', selected: c.style !== 'analog' }),
          el('option', { value: 'analog', textContent: 'Analogue (hands)', selected: c.style === 'analog' }));
        st.onchange = () => { if (st.value === 'analog') c.style = 'analog'; else delete c.style; delete c.background; draw(); };
        const check = (key, on, label) => {
          const i = el('input', { type: 'checkbox', checked: c[key] === on });
          i.onchange = () => { if (i.checked) c[key] = on; else delete c[key]; };
          return el('label', { className: 'check' }, i, label);
        };
        kids.push(el('label', { textContent: 'Style' }, st));
        if (c.style === 'analog') {
          kids.push(check('seconds', true, 'Seconds hand'),
            el('label', { textContent: 'Dial: colour (the others follow it)' }, colorPicker(c, 'dial_color')),
            el('label', { textContent: 'Hands: colour' }, colorPicker(c, 'hands_color')),
            el('label', { textContent: 'Hour marks: colour' }, colorPicker(c, 'marks_color')),
            el('label', { textContent: 'Minute marks: colour' }, colorPicker(c, 'minute_marks_color')));
        }
        /* black: the analogue clock's standard, the digital one's choice */
        const black = el('input', { type: 'checkbox', checked: c.style === 'analog' ? c.background !== 'card' : c.background === 'black' });
        black.onchange = () => {
          if (c.style === 'analog') { if (black.checked) delete c.background; else c.background = 'card'; }
          else if (black.checked) c.background = 'black'; else delete c.background;
        };
        kids.push(el('label', { className: 'check' }, black, 'Black background'));
      }
      if (c.type === 'custom:retoon-app') {
        const sel = el('select', {}, el('option', { value: '', textContent: apps.length ? 'Choose…' : 'No apps installed' }),
          apps.map(a => el('option', { value: a.dir, textContent: a.manifest.name || a.dir, selected: c.app === a.dir })));
        sel.onchange = () => { if (sel.value) c.app = sel.value; else delete c.app; draw(); };
        kids.push(el('label', { textContent: 'App' }, sel));
        /* settings of this card only, over the app's own (level 10) */
        const cs = ((apps.find(a => a.dir === c.app) || {}).manifest || {}).cardSettings;
        if (Array.isArray(cs) && cs.length) {
          const own = c.settings && typeof c.settings === 'object' ? c.settings : {};
          cardInputs = {};
          kids.push(el('p', { className: 'hint', textContent: 'For this card only (the other settings are under Apps):' }),
            ...cs.map(s => settingRow(s, s.key in own ? own[s.key] : s.default, cardInputs)));
        }
      }
      if (c.type === 'tile') {
        const feat = (c.features || [])[0] || {};
        const fs = el('select', {}, Object.entries(FEATURES).map(([k, v]) => el('option', { value: k, textContent: v, selected: (feat.type || '') === k })));
        fs.onchange = () => { if (fs.value) c.features = [{ type: fs.value }]; else delete c.features; draw(); };
        const hide = el('input', { type: 'checkbox', checked: !!c.hide_state });
        hide.onchange = () => { if (hide.checked) c.hide_state = true; else delete c.hide_state; };
        kids.push(text('Icon (empty = automatic)', 'icon', 'mdi:lightbulb'),
          el('label', { textContent: 'Extra control' }, fs));
        if (OPTION_LISTS[feat.type]) kids.push(optionPicker(c, feat));
        kids.push(el('label', { className: 'check' }, hide, 'Hide the state text'),
          el('label', { textContent: 'Name: text size' }, sizePicker(c, 'name_size')),
          el('label', { textContent: 'Name: colour' }, colorPicker(c, 'name_color')),
          el('label', { textContent: 'State: text size' }, sizePicker(c, 'state_size')),
          el('label', { textContent: 'State: colour' }, colorPicker(c, 'state_color')),
          el('label', { textContent: 'Colour when on' }, colorPicker(c, 'on_color')),
          ...tapEditor(c));
      }
      if (c.type === 'heading') {
        const st = el('select', {}, el('option', { value: 'title', textContent: 'Title', selected: c.heading_style !== 'subtitle' }),
          el('option', { value: 'subtitle', textContent: 'Small', selected: c.heading_style === 'subtitle' }));
        st.onchange = () => { c.heading_style = st.value; };
        kids.push(text('Text', 'heading'), text('Icon', 'icon', 'mdi:sofa'), el('label', { textContent: 'Size' }, st));
      }
      if (c.type === 'history-graph') {
        const hs = el('select', {}, Object.entries(HOURS).map(([k, v]) => el('option', { value: k, textContent: v,
          selected: String(c.hours_to_show || 24) === k })));
        hs.onchange = () => { if (hs.value === '24') delete c.hours_to_show; else c.hours_to_show = +hs.value; };
        kids.push(el('label', { textContent: 'Period' }, hs),
          el('p', { className: 'hint', textContent: 'Numbers only. Lines with different units get a graph each.' }));
      }
      if (c.type === 'entities' || c.type === 'history-graph') {
        c.entities = c.entities || [];
        const chips = el('div', { className: 'chips' });
        const drawChips = () => chips.replaceChildren(...c.entities.map((e, i) => el('span', {},
          entName(typeof e === 'string' ? e : e.entity),
          el('button', { className: 'icon', type: 'button', textContent: '✕', onclick: () => { c.entities.splice(i, 1); drawChips(); } }))));
        drawChips();
        kids.push(text('Title', 'title'), el('label', { textContent: 'Add' }),
          ...entityPicker(() => '', id => { c.entities.push(id); drawChips(); }), chips);
      }
      if (ENERGY[c.type]) {
        kids.push(el('p', { className: 'hint', textContent: c.type === 'energy-distribution' ?
          'Shows how the energy of today flows between the grid, solar panels, battery and the house, with the gas and water used.' :
          'Shows today per hour.' }),
          el('p', { className: 'hint', textContent: 'The numbers come from the Energy dashboard of Home Assistant: the meters you chose there.' }),
          text('Title', 'title'));
      }
      if (c.type === 'markdown') {
        const ta = el('textarea', { value: c.content || '' });
        ta.oninput = () => { c.content = ta.value; };
        kids.push(el('label', { textContent: 'Text' }, ta));
      }
    }
    kids.push(el('button', { type: 'button', className: 'icon', textContent: raw ? 'Simple form' : 'Edit as text (advanced)',
      onclick: () => { raw = !raw; if (!raw && !KINDS[c.type]) c.type = 'tile'; draw(); } }));
    body.replaceChildren(...kids);
  };

  $('#dlg-title').textContent = isNew ? 'New card' : 'Card';
  draw();
  $('#dlg-cancel').onclick = () => dlg.close('cancel');
  $('#dlg-form').onsubmit = ev => {
    ev.preventDefault();
    if ((c.type === 'tile' || c.type === 'thermostat' || c.type === 'gauge') && !c.entity) return alert('Choose what the card shows first.');
    if (c.type === 'custom:retoon-app' && !c.app) return alert('Choose an app first.');
    if (c.type === 'custom:retoon-app' && cardInputs) {
      const out = {};
      for (const [k, get] of Object.entries(cardInputs)) {
        const v = get();
        if (typeof v === 'number' && isNaN(v)) return alert(T`${k}: not a number`);
        out[k] = v;
      }
      c.settings = out;
    }
    if (c.type !== 'tile') {
      /* a large dial keeps its buttons (climate-hvac-modes / -preset-modes) */
      if (!(c.type === 'thermostat' && ['dial', 'classic'].includes(c.style || 'dial'))) delete c.features;
      delete c.hide_state;
      for (const k of ['name_size', 'name_color', 'state_size', 'state_color', 'on_color']) delete c[k];
    }
    if (c.tap_action && c.tap_action.action === 'perform-action' && !/^[a-z_]+\.[a-z0-9_]+$/.test(c.tap_action.perform_action || ''))
      return alert('Give the action as domain.action, for example script.turn_on.');
    if (c.type !== 'thermostat' && c.type !== 'custom:toon-clock') delete c.style;
    if (c.type !== 'custom:toon-clock' || c.style !== 'analog') { for (const k of ['seconds', 'dial_color', 'hands_color', 'marks_color', 'minute_marks_color']) delete c[k]; }
    if (c.type !== 'custom:toon-clock') delete c.background;
    if (isNew) {
      /* where it was asked for (an empty cell), or the first free place */
      const sz = defaultSize(c), p = at && { x: Math.min(at.x, GRID_COLS - sz.w), y: at.y, ...sz };
      c.position = p && fits(cards, null, p) ? p : freePlace(cards, sz.w, sz.h);
      cards.push(c);
    } else cards[index] = c;
    dlg.close('ok');
    render();
    dirty();
  };
  dlg.showModal();
}

/* ---- the Toon's screen ---------------------------------------------------- */

let screenBusy = false;
async function refreshScreen() {
  if (screenBusy || $('#tab-screen').hidden || document.hidden) return;
  screenBusy = true;
  try {
    const r = await api('screen');
    const [w, h, bpp] = (r.headers.get('X-Screen') || '1024 600 32').split(' ').map(Number);
    if (w && h && (w !== screenSize[0] || h !== screenSize[1])) { screenSize = [w, h]; }
    const raw = new Uint8Array(await r.arrayBuffer()), cv = $('#screen');
    if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
    const ctx = cv.getContext('2d'), img = ctx.createImageData(w, h), d = img.data;
    if (bpp === 16) {	/* RGB565, Toon 1 */
      for (let i = 0, j = 0; i < d.length && j + 1 < raw.length; i += 4, j += 2) {
        const v = raw[j] | raw[j + 1] << 8;
        d[i] = (v >> 8 & 0xf8) | v >> 13; d[i + 1] = (v >> 3 & 0xfc) | (v >> 9 & 3);
        d[i + 2] = (v << 3 & 0xf8) | (v >> 2 & 7); d[i + 3] = 255;
      }
    } else {		/* BGRX, Toon 2 */
      for (let i = 0; i < d.length && i < raw.length; i += 4) {
        d[i] = raw[i + 2]; d[i + 1] = raw[i + 1]; d[i + 2] = raw[i]; d[i + 3] = 255;
      }
    }
    ctx.putImageData(img, 0, 0);
  } catch (e) { /* next time */ }
  screenBusy = false;
}
$('#screen').onclick = ev => {
  const r = ev.target.getBoundingClientRect();
  const cv = ev.target;
  const x = Math.round((ev.clientX - r.left) * cv.width / r.width), y = Math.round((ev.clientY - r.top) * cv.height / r.height);
  api('ui', `tap ${x} ${y}\n`).then(() => setTimeout(refreshScreen, 600)).catch(fail);
};
$('#refresh').onclick = refreshScreen;
setInterval(refreshScreen, 6000);

/* ---- import ----------------------------------------------------------------- */

$('#load-dashboards').onclick = async () => {
  const list = $('#ha-dashboards');
  list.replaceChildren(el('li', { textContent: 'Asking Home Assistant…' }));
  try {
    const boards = [{ url_path: null, title: 'Overview (the default dashboard)' }, ...await haAsk('ha/dashboards')];
    list.replaceChildren(...boards.map(b => el('li', { onclick: () => showImport(b) },
      el('span', { className: 't', textContent: b.title || b.url_path }))));
  } catch (e) { list.replaceChildren(el('li', { textContent: e.message })); }
};

function countCards(o, acc = { shown: 0, simple: {}, missing: {} }) {
  if (Array.isArray(o)) o.forEach(x => countCards(x, acc));
  else if (o && typeof o === 'object') {
    if (typeof o.type === 'string' && !('views' in o) && !('sections' in o && 'title' in o && !('cards' in o))) {
      const container = ['grid', 'vertical-stack', 'horizontal-stack', 'conditional', 'sections'].includes(o.type);
      if (!container && ('entity' in o || 'entities' in o || 'heading' in o || 'content' in o || o.type.includes('-') || o.type.includes(':'))) {
        if (SUPPORTED.includes(o.type)) acc.shown++;
        else if (shown(o)) acc.simple[o.type] = (acc.simple[o.type] || 0) + 1;
        else acc.missing[o.type] = (acc.missing[o.type] || 0) + 1;
      }
    }
    for (const k of ['sections', 'cards', 'card']) if (k in o) countCards(o[k], acc);
  }
  return acc;
}

async function showImport(board) {
  const box = $('#import-views');
  box.replaceChildren(el('p', { className: 'hint', textContent: 'Loading…' }));
  try {
    const cfg = await haAsk('ha/dashboard?path=' + encodeURIComponent(board.url_path || ''));
    if (cfg.strategy) throw new Error('This dashboard is generated by Home Assistant itself and has no pages to copy. ' +
      'Use "Take control" on it in Home Assistant first, or pick another one.');
    const checks = [];
    const rows = (cfg.views || []).map((v, i) => {
      const n = countCards(v), cb = el('input', { type: 'checkbox', checked: true });
      const list = o => Object.entries(o).map(([t, k]) => `${k}× ${t}`).join(', ');
      checks.push([cb, v]);
      return el('label', { className: 'check' }, cb, el('span', {}, el('b', { textContent: v.title || T`Page ${i + 1}` }),
        T` — ${n.shown} cards` + (list(n.simple) ? T`; shown as a plain tile or list: ${list(n.simple)}` : '') +
        (list(n.missing) ? T`; cannot be shown: ${list(n.missing)}` : '')));
    });
    const add = replace => {
      const picked = checks.filter(([cb]) => cb.checked).map(([, v]) => toGrid(JSON.parse(JSON.stringify(v))));
      if (!picked.length) return;
      if ((replace ? 0 : dash.views.length) + picked.length > MAX_VIEWS)
        return alert(T`At most ${MAX_VIEWS} pages fit on the Toon.`);
      if (replace) dash.views = picked; else dash.views.push(...picked);
      cur = replace ? 0 : dash.views.length - picked.length;
      render();
      dirty();
      $('#import-box').hidden = true;
      say('Copied. Check the pages, then press Send to ReToon.');
    };
    box.replaceChildren(el('div', { className: 'imp' }, el('h2', { textContent: board.title || 'Dashboard' }), rows,
      el('div', { className: 'row' },
        el('button', { className: 'primary', textContent: 'Add these pages', onclick: () => add(false) }),
        el('button', { textContent: 'Replace all my pages with these', onclick: () => { if (confirm(T('Replace all pages on the ReToon with the selected ones?'))) add(true); } }))));
  } catch (e) { box.replaceChildren(el('p', { className: 'hint', textContent: e.message })); }
}

/* ---- settings and status ------------------------------------------------------ */

// Until the Toon has sent its settings, the fields say so and cannot be
// used: the grey example values looked like the real ones.
const loading = [...document.querySelectorAll(['#f-ha', '#f-mqtt', '#f-broker', '#f-ui', '#f-off', '#f-home', '#f-name', '#f-boot', '#f-web', '#f-zwave', '#f-esphome', '#f-radio', '#f-sound']
  .map(f => `${f} input, ${f} select, ${f} textarea, ${f} button`).join())].filter(f => !f.disabled);
for (const f of loading) {
  f.disabled = true;
  if (f.matches('input:not([type]), input[type=text], input[type=number], input[type=password], textarea')) {
    f.dataset.placeholder = f.placeholder; f.placeholder = 'loading…';
  }
}
function settingsLoaded() {
  for (const f of loading.splice(0)) {
    f.disabled = false;
    if ('placeholder' in f.dataset) { f.placeholder = f.dataset.placeholder; delete f.dataset.placeholder; }
  }
}

async function loadSettings() {
  const s = await (await api('settings')).json();
  /* the settings come in again every few seconds while Home Assistant is
     not connected: what the user is typing or has changed must stay */
  const act = document.activeElement;
  const keep = [...document.querySelectorAll('[data-edited]'), ...(act && act.matches('input, select, textarea') ? [act] : [])]
    .map(e => [e, e.type === 'checkbox' || e.type === 'radio' ? e.checked : e.value]);
  haSet = !!s.ha_url && s.ha_auth !== 'none';
  $('#hostname').textContent = s.hostname;
  const lang = s.language === 'en' ? 'en' : 'nl';
  $('#lang').value = lang;
  if (lang !== LANG) {	/* set on the Toon or in another browser */
    let kept = false;
    try { localStorage.lang = lang; kept = localStorage.lang === lang; } catch (e) { /* private window */ }
    if (kept) location.reload();
  }
  document.title = `ReToon ${s.hostname}`;
  const ha = $('#f-ha'), mq = $('#f-mqtt'), ui = $('#f-ui');
  ha.url.value = s.ha_url || localStorage.haUrl || '';
  ha.verify.checked = s.ha_verify === '1';
  const up = s.ha_state === 'Connected', box = $('#ha-state');
  box.className = 'state ' + (s.ha_auth === 'none' ? '' : up ? 'ok' : 'warn');
  box.replaceChildren(el('b', { textContent: s.ha_auth === 'none' ? 'Not connected yet' : up ? 'Connected' : s.ha_state || 'Connecting' }));
  if (s.ha_auth !== 'none') {
    const row = (k, v) => el('div', {}, el('span', { textContent: k }), v);
    box.append(row('Address', s.ha_url));
    if (s.ha_user) box.append(row('Account', s.ha_user));
    box.append(row('Uses', s.ha_auth === 'token' ? 'a token you pasted yourself' :
      T`long-lived access token "Toon ${s.hostname}"` + (s.ha_made ? T`, made by logging in on ${s.ha_made}` : ', made by logging in')));
    if (/^https:/.test(s.ha_url)) box.append(row('Certificate', s.ha_verify === '1' ? 'checked' : 'not checked'));
    if (!up && s.ha_addr) box.append(row('The Toon tries', s.ha_addr));
    if (/not reachable/.test(s.ha_state || '') && s.ha_addr)
      box.append(el('p', { className: 'hint', textContent: T`The Toon itself cannot reach ${s.ha_addr}, even if your browser can reach Home Assistant. Check that this is the address of Home Assistant on your own network (not localhost, not the guest network) and that nothing on that machine blocks the Toon.` }));
    if (!up && !bus.connected()) setTimeout(() => { if (tabShown('connections') || tabShown('overview')) loadSettings().catch(() => {}); }, 3000);
  }
  const nm = $('#f-name').hostname;
  nm.value = s.own_hostname || '';
  nm.placeholder = s.default_hostname;
  $('#f-boot').autoboot.checked = s.autoboot === '1';
  $('#rootpw-state').textContent = s.root_pw_own ? 'You have set your own password.' : 'The password is still the standard one, "toon".';
  const web = $('#f-web');
  web.http.checked = s.web_http === 'on';
  $('#web-fp').textContent = s.web_fingerprint ? T`Certificate fingerprint (SHA-256): ${s.web_fingerprint}` :
    'No certificate could be made, so https is off.';
  mq.host.value = s.mqtt_host; mq.port.value = s.mqtt_port; mq.username.value = s.mqtt_username;
  const zw = $('#f-zwave'), zbox = $('#zwave-state');
  zw.remote.checked = s.zwave_mode === 'remote';
  zw.allow.value = s.zwave_allow || '';
  const zrow = (k, v) => el('div', {}, el('span', { textContent: k }), v);
  const zon = s.zwave_mode === 'remote', zused = s.zwave_state === 'connected';
  zbox.className = 'state ' + (zused ? 'ok' : '');
  zbox.replaceChildren(el('b', { textContent: !zon ? 'Off' : zused ? 'In use by Z-Wave JS' :
    s.zwave_state === 'waiting' ? 'Waiting for Z-Wave JS' : 'Not running' }));
  if (zon) {
    zbox.append(zrow('Address', `tcp://${s.ip || location.hostname}:6639`));
    if (zused) zbox.append(zrow('Client', s.zwave_client));
  }
  drawMeters(s);
  $('#update-auto').value = ['hourly', 'nightly', 'weekly'].includes(s.update_auto) ? s.update_auto : 'off';
  $('#apps-auto').checked = s.apps_auto === 1;
  appsAutoHint();
  const zk = (s.zwave_keys || '').trim().split(/\s+/), names = { S0: 'S0 (legacy)', S2U: 'S2 unauthenticated', S2A: 'S2 authenticated', S2C: 'S2 access control' };
  $('#zwave-keys').hidden = zk.length < 2;
  $('#zwave-keys-list').textContent = zk.length < 2 ? '' :
    zk.reduce((o, v, i) => i % 2 ? o.concat(`${names[zk[i - 1]] || zk[i - 1]}: ${v}`) : o, []).join('\n');
  const ep = $('#f-esphome'), ebox = $('#esphome-state');
  ep.hidden = s.esphome_available !== '1';
  ep.on.checked = s.esphome_mode !== 'off';
  ep.allow.value = s.esphome_allow || '';
  const eon = s.esphome_mode !== 'off', eused = s.esphome_state === 'connected';
  ebox.className = 'state ' + (eused ? 'ok' : '');
  ebox.replaceChildren(el('b', { textContent: !eon ? 'Off' : eused ? 'In use by Home Assistant' :
    s.esphome_state === 'listening' ? 'Waiting for Home Assistant' :
    s.esphome_state === 'no-bluetooth' ? 'No Bluetooth radio' : 'Not running' }));
  if (eused) {
    ebox.append(zrow('Home Assistant', s.esphome_client));
    ebox.append(zrow('Scanning', s.esphome_scanning === 'on' ? T`yes, ${s.esphome_adverts || 0} advertisements in the last minute` :
      s.esphome_scanning === 'failed' ? 'no: the radio is busy (another program scans)' : 'no'));
  }
  const sd = $('#f-sound');
  sd.hidden = s.sound_available !== '1';
  sd.volume.value = s.sound_volume || '70';
  $('#sound-state').replaceChildren(el('b', { textContent: s.sound_ready === '1' ? T`Volume ${sd.volume.value} %` :
    'Not ready: the sound driver is not running' }));
  sd.volume.oninput = () => { $('#sound-state').firstChild.textContent = T`Volume ${sd.volume.value} %`; };
  showRadio(s);
  mq.discovery_prefix.value = s.mqtt_discovery_prefix;
  const br = $('#f-broker'), bbox = $('#broker-state'), lanIps = (s.broker_listening || '').trim();
  br.lan.checked = s.broker_lan === '1';
  br.username.value = s.broker_username || '';
  bbox.className = 'state ' + (s.broker_running ? 'ok' : 'warn');
  bbox.replaceChildren(el('b', { textContent: !s.broker_running ? 'Not running' :
    s.broker_lan === '1' && lanIps ? 'Running, open to the network' : 'Running, only for this ReToon' }));
  if (s.broker_running && s.broker_lan === '1' && lanIps)
    bbox.append(zrow('Address', lanIps.split(/\s+/).map(a => a + ':1883').join(', ')));
  ui.brightness.value = s.brightness; ui.dim_after.value = s.dim_after; ui.dim_level.value = s.dim_level; ui.dim_grey.checked = s.dim_grey === '1';
  const off = $('#f-off'); off.sleep_off.checked = s.sleep_off === '1'; off.off_from.value = s.off_from || ''; off.off_to.value = s.off_to || '';
  const hp = $('#f-home'); hp.home_auto.checked = s.home_auto !== '0'; hp.home_after.value = s.home_after || 10; hp.page_anim.checked = s.page_anim !== '0';
  settingsLoaded();
  /* after settingsLoaded, which puts the first placeholders back */
  mq.password.placeholder = s.mqtt_password_set ? '(unchanged)' : '';
  br.password.placeholder = s.broker_password_set ? '(unchanged)' : '';
  for (const [e, v] of keep) {
    if (e.type === 'checkbox' || e.type === 'radio') {
      if (e.checked !== v) { e.checked = v; if (e.type === 'radio') e.dispatchEvent(new Event('change', { bubbles: true })); }
    } else e.value = v;
  }
  armForms();
}

const haUrl = () => {
  let u = $('#f-ha').url.value.trim().replace(/\/+$/, '');
  if (u && !/^https?:\/\//.test(u)) u = 'http://' + u;
  return u;
};
$('#f-ha').onsubmit = ev => {
  ev.preventDefault();
  const url = haUrl(), me = location.origin + '/';
  if (!url) return fail(new Error('Enter the address of Home Assistant'));
  localStorage.haUrl = url;
  localStorage.haVerify = $('#f-ha').verify.checked ? 1 : 0;
  location.href = `${url}/auth/authorize?client_id=${encodeURIComponent(me)}` +
    `&redirect_uri=${encodeURIComponent(me + '?auth_callback=1')}&state=toon`;
};
$('#ha-token-save').onclick = async () => {
  try {
    await api('ha/token', lines({ url: haUrl(), token: $('#f-ha').token.value.trim(), verify: $('#f-ha').verify.checked ? 1 : 0 }));
    $('#f-ha').token.value = '';
    for (const e of $('#f-ha').querySelectorAll('[data-edited]')) delete e.dataset.edited;
    say('Saved', 'ok');
    loadSettings().then(loadEntities).catch(() => {});
  } catch (e) { fail(e); }
};
$('#ha-logout').onclick = async () => { try { await api('ha/logout', ''); loadSettings(); say('Disconnected', 'ok'); } catch (e) { fail(e); } };
// The GP711 radio: Zigbee (zigbee2mqtt over TCP), Thread (border router) or off.
const threadRoles = { leader: 'Running, leader of the network', router: 'Running, router in the network',
  child: 'Joined as a child', detached: 'Looking for the network', disabled: 'Starting' };
function showRadio(s) {
  const f = $('#f-radio'), box = $('#radio-state'), mode = s.radio_mode || 'zigbee';
  f.hidden = mode === 'none';
  for (const r of f.mode) r.checked = r.value === mode;
  for (const e of f.querySelectorAll('[data-mode]')) e.hidden = e.dataset.mode !== mode;
  const row = (k, v) => el('div', {}, el('span', { textContent: k }), v);
  const host = s.ip || location.hostname;
  const up = mode === 'thread' && /^(leader|router|child)$/.test(s.thread_state);
  box.className = 'state ' + (up || mode === 'zigbee' ? 'ok' : mode === 'thread' ? 'warn' : '');
  if (mode === 'zigbee') {
    box.replaceChildren(el('b', { textContent: 'Zigbee' }), row('Address', `tcp://${host}:6638`));
  } else if (mode === 'thread') {
    box.replaceChildren(el('b', { textContent: threadRoles[s.thread_state] || s.thread_state || 'Not running' }));
    if (s.thread_name) box.append(row('Network', T`${s.thread_name}, channel ${s.thread_channel}`));
    if (up) box.append(row('Devices', T`${s.thread_children || 0} attached to this ReToon, ` +
      T`${Math.max(0, (s.thread_routers || 1) - 1)} other routers`));
    box.append(row('Home Assistant', `http://${host}:8081`));
    if (!up && !bus.connected()) setTimeout(() => { if (tabShown('connections') || tabShown('overview')) loadSettings().catch(() => {}); }, 5000);
  } else {
    box.replaceChildren(el('b', { textContent: 'Off' }));
  }
}
for (const r of $('#f-radio').mode)
  r.onchange = () => { for (const e of $('#f-radio').querySelectorAll('[data-mode]')) e.hidden = e.dataset.mode !== r.value; };
$('#f-radio').onsubmit = async ev => {
  ev.preventDefault();
  const mode = [...ev.target.mode].find(r => r.checked)?.value || 'zigbee';
  try {
    await api('settings/radio', lines({ mode }));
    say('Saved', 'ok');
    setTimeout(loadSettings, mode === 'thread' ? 15000 : 2500);
  } catch (e) { fail(e); }
};
$('#thread-join').onclick = async () => {
  const f = $('#f-radio'), dataset = f.tlv.value.replace(/\s+/g, '');
  if (!/^[0-9a-fA-F]{20,}$/.test(dataset)) return fail(new Error('Paste the dataset as hex'));
  try {
    await api('settings/thread', lines({ dataset }));
    f.tlv.value = '';
    say('Joining the network', 'ok');
    setTimeout(loadSettings, 10000);
  } catch (e) { fail(e); }
};
$('#thread-new').onclick = async () => {
  if (!confirm(T('Start a new Thread network? Devices in the current one stay there and have to be added again.'))) return;
  try {
    await api('settings/thread', lines({ action: 'new' }));
    say('New network started', 'ok');
    setTimeout(loadSettings, 10000);
  } catch (e) { fail(e); }
};
$('#update-auto').onchange = async ev => {
  appsAutoHint();
  try { await api('settings/update', lines({ auto: ev.target.value })); say('Saved', 'ok'); } catch (e) { fail(e); }
};
/* apps update by themselves at the moments ReToon itself looks for an update */
function appsAutoHint() {
  const when = { hourly: 'every hour', nightly: 'every night around 3:00', weekly: 'every week on Sunday night' }[$('#update-auto').value];
  $('#apps-auto-hint').textContent = when ?
    T`Uses the same schedule as the update of ReToon itself (System, Update and restart): now ${T(when)}. The screen restarts after an app is updated, and then says which ones.` :
    T('Uses the same schedule as the update of ReToon itself (System, Update and restart), which is now set to Never: then apps are not updated by themselves either.');
}
$('#apps-auto').onchange = async ev => {
  try { await api('settings/apps-update', lines({ on: ev.target.checked ? 1 : 0 })); say('Saved', 'ok'); } catch (e) { fail(e); }
};

/* the meter adapters seen on the Z-Wave network, each with a tick for
   reading it here, and what it reads now */
let lastSettings = {};
const meterNodes = (s = lastSettings) => (s.zwave_meters || '').split(/\s+/).filter(Boolean);
async function drawMeters(s) {
  const box = $('#zwave-meters'), found = (s.zwave_found || '').split(/\s+/).filter(Boolean);
  const on = meterNodes(s);
  let vals = {};
  lastSettings = s;
  if (on.length) try { vals = await (await api('meter')).json(); } catch (e) { /* none */ }
  const nodes = [...new Set([...found, ...on])];
  if (!nodes.length) {
    box.replaceChildren(el('p', { className: 'hint', textContent: s.zwave_mode === 'remote' ?
      'None seen yet. A meter adapter shows up here when it sends a reading, every 10 seconds while a value changes.' :
      'Turn on the Z-Wave radio above: meter adapters are found while it is on.' }));
    return;
  }
  /* a number as the page's language writes it, with the decimals it came with */
  const num = v => Number(v).toLocaleString(document.documentElement.lang,
    { minimumFractionDigits: (String(v).split('.')[1] || '').length });
  const head = $('#meters-head');
  if (!head.querySelector('.help')) {
    const icon = helpIcon();
    icon.pop.append(el('p', { textContent: 'Z-Wave JS does not support the readings of these meter adapters yet, so ReToon reads them itself, from what passes through the radio.' }),
      el('p', { textContent: 'Tick an adapter to use its readings in ReToon: on the screen, and over MQTT in Home Assistant. Untick it once Z-Wave JS supports them, or Home Assistant gets them twice.' }));
    head.append(' ', icon);
  }
  box.replaceChildren(...nodes.map(n => {
      /* a line per meter: its total, and what it does now (power, flow) */
      const mine = Object.entries(vals).filter(([k]) => k.startsWith(`n${n}_`)).map(([k, v]) => [k.replace(/^n\d+_/, ''), v]);
      const lines = mine.filter(([w]) => !/_(power|flow)$/.test(w)).map(([w, v]) => {
        const now = mine.find(([x]) => x === w + '_power' || x === w + '_flow');
        return `${meterName(w)}: ${num(v)} ${meterUnit(w)}`.trim() +
          (now ? T`, now ${`${num(now[1])} ${meterUnit(now[0])}`.trim()}` : '');
      });
      return el('div', {}, el('label', { className: 'check' }, el('input', { type: 'checkbox', value: n, checked: on.includes(n) }),
        el('span', { textContent: T`Use the meter adapter at node ${n}` })),
        lines.length ? el('p', { className: 'hint', textContent: lines.join('\n'), style: 'white-space:pre-line' }) : '');
    }));
}
$('#f-zwave').onsubmit = async ev => {
  ev.preventDefault();
  const f = ev.target;
  try {
    const meters = [...document.querySelectorAll('#zwave-meters input:checked')].map(c => c.value).join(' ');
    await api('settings/zwave', lines({ mode: f.remote.checked ? 'remote' : 'off', allow: f.allow.value.trim(), meters }));
    say('Saved', 'ok');
    setTimeout(loadSettings, 2500);
  } catch (e) { fail(e); }
};
$('#f-sound').onsubmit = async ev => {
  ev.preventDefault();
  try {
    await api('settings/sound', lines({ volume: ev.target.volume.value }));
    say('Saved', 'ok');
  } catch (e) { fail(e); }
};
$('#sound-test').onclick = async () => {
  try { await api('sound/test', lines({ volume: $('#f-sound').volume.value })); } catch (e) { fail(e); }
};
$('#f-esphome').onsubmit = async ev => {
  ev.preventDefault();
  const f = ev.target;
  try {
    await api('settings/esphome', lines({ mode: f.on.checked ? 'on' : 'off', allow: f.allow.value.trim() }));
    say('Saved', 'ok');
    setTimeout(loadSettings, 2500);
  } catch (e) { fail(e); }
};
$('#f-mqtt').onsubmit = async ev => {
  ev.preventDefault();
  const f = ev.target, o = { host: f.host.value.trim(), port: f.port.value.trim() || '1883', username: f.username.value.trim() };
  if (f.discovery_prefix.value.trim()) o.discovery_prefix = f.discovery_prefix.value.trim();
  if (f.password.value) o.password = f.password.value; else o.keep_password = 1;
  try { await api('settings/mqtt', lines(o)); f.password.value = ''; say('Saved', 'ok'); loadSettings(); setTimeout(loadEntities, 6000); } catch (e) { fail(e); }
};
$('#f-broker').onsubmit = async ev => {
  ev.preventDefault();
  const f = ev.target, o = { lan: f.lan.checked ? '1' : '0', username: f.username.value.trim() };
  if (f.password.value) o.password = f.password.value; else o.keep_password = 1;
  try { await api('settings/broker', lines(o)); f.password.value = ''; say('Saved', 'ok'); setTimeout(loadSettings, 2500); } catch (e) { fail(e); }
};
$('#f-name').onsubmit = async ev => {
  ev.preventDefault();
  const name = ev.target.hostname.value.trim().toLowerCase();
  try {
    await api('settings/hostname', lines({ hostname: name }));
    say(T`Saved. The ReToon is now at https://${name || ev.target.hostname.placeholder}.local/ (or its address); open it there.`);
  } catch (e) { fail(e); }
};
/* ---- thermostat settings ------------------------------------------------------ */

const PRESETS = { comfort: 'Comfort', home: 'Home', sleep: 'Sleep', away: 'Away' };
const DAYS = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];
const coolDefault = () => ({ enabled: false, mode: 'heat', outside_entity: '', cool_above: 24, heat_below: 18 });
const hwDefault = () => ({ setpoint: 0, program_enabled: false,
  program: DAYS.map(() => [['06:30', 60], ['22:00', 45]]) });
const curveDefault = () => ({ cold: -10, cold_flow: 45, warm: 15, warm_flow: 30 });
const thDefault = () => ({ sensors: ['toon'], presets: { comfort: 20, home: 18, sleep: 15, away: 12 },
  program_enabled: false, program: DAYS.map(() => [['07:00', 'comfort'], ['23:00', 'sleep']]),
  preheat: true, max_preheat: 180, modulation: true, max_rate: 3, heat_start: 30, onoff_period: 20, frost: 5, hot_water: hwDefault(),
  max_flow_mode: 'boiler', max_flow: 45, max_flow_curve: curveDefault(), outside_entity: '',
  presets_cool: { comfort: 23, home: 24, sleep: 25, away: 28 }, cooling: coolDefault() });
let th = thDefault();

/* a week of [time, value] steps, in place; value(step) makes the field for it */
function weekEditor(week, blank, value) {
  return DAYS.map((name, d) => {
    const steps = week[d] = week[d] || [];
    return el('div', { className: 'day' }, el('b', { textContent: name }),
      steps.map((st, i) => {
        const t = el('input', { type: 'time', value: st[0] });
        t.onchange = () => { st[0] = t.value; };
        return el('div', { className: 'step' }, t, value(st),
          el('button', { className: 'icon danger', type: 'button', textContent: '✕', title: 'Remove',
            onclick: () => { steps.splice(i, 1); drawThermostat(); } }));
      }),
      el('div', { className: 'row' },
        el('button', { type: 'button', className: 'icon', textContent: '+ Step',
          onclick: () => { thRead(); steps.push([...blank]); drawThermostat(); } }),
        el('button', { type: 'button', className: 'icon', textContent: 'All days the same',
          onclick: () => { thRead(); week.splice(0, 7, ...DAYS.map(() => JSON.parse(JSON.stringify(steps)))); drawThermostat(); } })));
  });
}

/* the Thermostat tab's own pages, and what shows only when it means something */
let thPage = 'main', thOutside = false;
try { thPage = localStorage.getItem('thPage') || thPage; } catch (e) {}
function thShow(page) {
  const b = page && $(`#th-nav button[data-sub=${page}]`);
  if (b && !b.hidden) thPage = page;
  else if ($(`#th-nav button[data-sub=${thPage}]`).hidden) thPage = 'main';
  for (const x of document.querySelectorAll('#th-nav button')) x.classList.toggle('on', x.dataset.sub === thPage);
  for (const x of document.querySelectorAll('.th-page')) x.hidden = x.dataset.page !== thPage;
  masonry();	/* a sub-page that was hidden is laid out now */
  try { localStorage.setItem('thPage', thPage); } catch (e) {}
}
/* automatic needs an outside temperature; a sensor just chosen counts */
function thAuto() {
  const can = thOutside || !!th.outside_entity;
  $('#cool-auto').disabled = !can;
  if (!can && $('#cool-mode').value === 'auto') $('#cool-mode').value = 'heat';
  $('#cool-hint').textContent = can ? '' :
    T('Automatic needs an outside temperature: from the installation, or choose a sensor on the Installation page.');
  thFold();
}
function thFold() {
  const on = $('#cool-on').checked;
  $('#th-days').hidden = !$('#th-program').checked;
  $('#hw-week').hidden = !$('#hw-program').checked;
  $('#cool-more').hidden = !on;
  $('#cool-auto-box').hidden = $('#cool-mode').value !== 'auto';
  for (const x of document.querySelectorAll('.th-cool')) x.hidden = !on || $('#th-nav-cool').hidden;
}
for (const b of document.querySelectorAll('#th-nav button')) b.onclick = () => thShow(b.dataset.sub);

/* the Screen tab's own pages: the page editor, brightness, the rest */
let scPage = 'pages';
try { scPage = localStorage.getItem('scPage') || scPage; } catch (e) {}
function scShow(page, init) {
  if (page && $(`#sc-nav button[data-sub=${page}]`)) scPage = page;
  if (!$(`#sc-nav button[data-sub=${scPage}]`)) scPage = 'pages';
  for (const x of document.querySelectorAll('#sc-nav button')) x.classList.toggle('on', x.dataset.sub === scPage);
  for (const x of document.querySelectorAll('.sc-page')) x.hidden = x.dataset.page !== scPage;
  if (!init) masonry();	/* at the start the tab lays itself out when it opens */
  try { localStorage.setItem('scPage', scPage); } catch (e) {}
}
for (const b of document.querySelectorAll('#sc-nav button')) b.onclick = () => scShow(b.dataset.sub);
scShow(null, true);
for (const id of ['th-program', 'hw-program', 'cool-on', 'cool-mode']) $('#' + id).addEventListener('change', thFold);

function drawThermostat() {
  for (const k in PRESETS) $(`#th-p-${k}`).value = th.presets[k];
  $('#th-own').checked = th.sensors.includes('toon');
  $('#th-offset').value = (th.temp_offset || 0).toFixed(1);
  $('#th-program').checked = !!th.program_enabled;
  $('#th-preheat').checked = !!th.preheat;
  $('#th-external').checked = !!th.external;
  $('#th-log').checked = !!th.log_changes;
  $('#th-measure').checked = !!th.log_measurements;
  $('#th-mod').checked = !!th.modulation;
  $('#th-flow').value = th.flow || '';
  $('#th-flow').disabled = !!th.modulation;
  $('#th-start').value = th.heat_start || 30;
  $('#th-start').disabled = !th.modulation;
  $('#th-ignored').checked = !!th.flow_ignored;
  /* a source that does not follow the temperature: pulses, none of the flow settings count */
  $('#th-mod').onchange = $('#th-ignored').onchange = () => {
    const ign = $('#th-ignored').checked;
    $('#th-mod').disabled = ign;
    $('#th-flow').disabled = ign || $('#th-mod').checked;
    $('#th-start').disabled = ign || !$('#th-mod').checked;
  };
  $('#th-mod').onchange();
  $('#th-maxpre').value = th.max_preheat;
  $('#th-maxmode').value = th.max_flow_mode || 'boiler';
  $('#th-maxflow').value = th.max_flow || 45;
  const cv = th.max_flow_curve;
  $('#cv-cold').value = cv.cold; $('#cv-cold-flow').value = cv.cold_flow;
  $('#cv-warm').value = cv.warm; $('#cv-warm-flow').value = cv.warm_flow;
  const maxShow = () => {
    $('#th-maxfixed').hidden = $('#th-maxmode').value !== 'fixed';
    $('#th-maxcurve').hidden = $('#th-maxmode').value !== 'curve';
  };
  $('#th-maxmode').onchange = maxShow;
  maxShow();
  $('#th-rate').value = th.max_rate || 3;
  $('#th-period').value = th.onoff_period || 20;
  $('#th-frost').value = th.frost;

  const others = () => th.sensors.filter(x => x !== 'toon');
  const chips = () => $('#th-sensors').replaceChildren(...others().map(id => el('span', {}, entName(id),
    el('button', { className: 'icon', type: 'button', textContent: '✕',
      onclick: () => { th.sensors = th.sensors.filter(x => x !== id); chips(); } }))));
  chips();
  // not the Toon's own (the tick box), nor its room and outside temperature
  // as Home Assistant has them: those come from what is chosen here
  const temp = e => !e.id.startsWith('toon.') && !/^sensor\.toon_[0-9a-f]{6}_(outside_)?temperature$/.test(e.id) &&
    (e.id.startsWith('mqtt.') ? (e.kind || 'sensor') === 'sensor' :
    e.id.startsWith('sensor.') && /°/.test(e.state || ''));
  $('#th-pick').replaceChildren(...entityPicker(() => '', id => {
    if (!th.sensors.includes(id)) th.sensors.push(id);
    chips();
  }, temp));

  $('#th-days').replaceChildren(...weekEditor(th.program, ['12:00', 'home'], st => {
    const p = el('select', {}, Object.entries(PRESETS).map(([k, v]) => el('option', { value: k, textContent: v, selected: st[1] === k })));
    p.onchange = () => { st[1] = p.value; };
    return p;
  }));
  const cool = th.cooling;
  for (const k in PRESETS) $(`#th-c-${k}`).value = th.presets_cool[k];
  $('#cool-on').checked = !!cool.enabled;
  $('#cool-mode').value = cool.mode;
  $('#cool-above').value = cool.cool_above;
  $('#heat-below').value = cool.heat_below;
  const outChip = () => { $('#th-out-chosen').replaceChildren(...(th.outside_entity ? [el('span', {}, entName(th.outside_entity),
    el('button', { className: 'icon', type: 'button', textContent: '✕',
      onclick: () => { th.outside_entity = ''; outChip(); thTouch({ target: $('#th-maxmode') }); } }))] : []));
    thAuto();
  };
  outChip();
  $('#th-out').replaceChildren(...entityPicker(() => '', id => { th.outside_entity = id; outChip(); }, temp));
  const hw = th.hot_water;
  $('#hw-set').value = hw.setpoint || '';
  $('#hw-program').checked = !!hw.program_enabled;
  $('#hw-keep').checked = hw.keep_warm !== false;
  $('#hw-days').replaceChildren(...weekEditor(hw.program, ['12:00', 50], st => {
    const p = el('input', { type: 'number', min: 30, max: 80, step: 1, value: st[1] });
    p.onchange = () => { st[1] = parseFloat(p.value) || st[1]; };
    return p;
  }));
  thFold();
  thShow();
}

/* take the plain fields into th (the lists are kept up to date as they change) */
function thRead() {
  for (const k in PRESETS) th.presets[k] = parseFloat($(`#th-p-${k}`).value) || th.presets[k];
  const others = th.sensors.filter(x => x !== 'toon');
  th.sensors = $('#th-own').checked || !others.length ? ['toon', ...others] : others;
  /* added to what the Toon measures: -5 .. 5 in tenths */
  const off = Math.round(Math.min(5, Math.max(-5, parseFloat($('#th-offset').value) || 0)) * 10) / 10;
  if (off) th.temp_offset = off; else delete th.temp_offset;
  th.program_enabled = $('#th-program').checked;
  th.preheat = $('#th-preheat').checked;
  th.external = $('#th-external').checked;
  th.log_changes = $('#th-log').checked;
  if ($('#th-measure').checked) th.log_measurements = true; else delete th.log_measurements;
  th.modulation = $('#th-mod').checked;
  if ($('#th-ignored').checked) th.flow_ignored = true; else delete th.flow_ignored;
  th.flow = Math.min(80, Math.max(0, parseFloat($('#th-flow').value) || 0));
  if (th.flow && th.flow < 20) th.flow = 20;
  th.max_preheat = parseInt($('#th-maxpre').value) || 0;
  const num = (id, d, lo, hi) => { const v = parseFloat($(id).value); return isNaN(v) ? d : Math.min(hi, Math.max(lo, v)); };
  th.max_flow_mode = $('#th-maxmode').value;
  th.max_flow = num('#th-maxflow', 45, 20, 80);
  th.heat_start = num('#th-start', 30, 20, 40);
  th.onoff_period = Math.round(num('#th-period', 20, 10, 30));
  th.max_flow_curve = { cold: num('#cv-cold', -10, -30, 30), cold_flow: num('#cv-cold-flow', 45, 20, 80),
    warm: num('#cv-warm', 15, -30, 30), warm_flow: num('#cv-warm-flow', 30, 20, 80) };
  th.max_rate = Math.min(10, Math.max(0.5, parseFloat($('#th-rate').value) || 3));
  th.frost = parseFloat($('#th-frost').value) || 5;
  /* in place: the editor's rows point at these lists */
  for (const k in PRESETS) th.presets_cool[k] = parseFloat($(`#th-c-${k}`).value) || th.presets_cool[k];
  th.cooling.enabled = $('#cool-on').checked;
  /* the sensor is the thermostat's now; ReToon before 1.24 read it here */
  th.cooling.outside_entity = th.outside_entity;
  th.cooling.mode = $('#cool-mode').value;
  th.cooling.cool_above = parseFloat($('#cool-above').value) || 24;
  th.cooling.heat_below = parseFloat($('#heat-below').value) || 18;
  th.hot_water.setpoint = parseFloat($('#hw-set').value) || 0;
  th.hot_water.program_enabled = $('#hw-program').checked;
  th.hot_water.keep_warm = $('#hw-keep').checked;
  for (const day of [...th.program, ...th.hot_water.program]) {
    for (let i = day.length - 1; i >= 0; i--) if (!/^\d\d:\d\d$/.test(day[i][0])) day.splice(i, 1);
    day.sort((a, b) => a[0].localeCompare(b[0]));
  }
}

async function loadThState() {
  let s;
  try { s = await (await api('thermostat/state')).json(); } catch (e) { return; }
  const box = $('#th-state'), row = (k, v) => el('div', {}, el('span', { textContent: k }), v);
  const on = s.hvac === 'heat', heating = s.heat === '1';
  $('#th-offset-now').textContent = !s.own ? '' : s.own_uncorrected && s.own_uncorrected !== s.own ?
    T`The ReToon measures ${(+s.own_uncorrected).toFixed(1)} °C; with the correction ${(+s.own).toFixed(1)} °C.` :
    T`The ReToon measures ${(+s.own).toFixed(1)} °C.`;
  box.className = 'state ' + (heating ? 'warn' : on ? 'ok' : '');
  box.replaceChildren(el('b', { textContent: !s.hvac ? 'The thermostat is not running' : s.frost === '1' ? 'Frost protection' :
    heating ? 'Heating now' : on ? 'On, not heating now' : 'Off' }));
  if (s.hvac) {
    box.append(row('Room', s.room ? `${s.room} °C` + (s.room_source === 'sensors' ? T` (average of ${s.sensors} of ${s.of} sensors)` :
      th.sensors.length > 1 || !th.sensors.includes('toon') ? ' ' + T("(the ReToon's own sensor: the others do not answer)") : '') : 'no reading'));
    box.append(row('Target', `${s.target} °C` + (s.preset && s.preset !== 'manual' ? ` (${T(PRESETS[s.preset] || s.preset)})` : T(' (set by hand)')) +
      (s.preheat === '1' ? T`, warming up to ${s.eff_target} °C` : '')));
    if (s.program === '1') box.append(row('Program', s.hold === '1' ? T`on hold until ${s.until}` :
      T`next: ${(s.next || '').replace(/^(\S+) (\S+)$/, (m, t, p) => T`${T(PRESETS[p] || p)} at ${t}`)}`));
    box.append(row('Installation', (s.link === 'ok' ? (s.mode === 'opentherm' ? 'OpenTherm' : 'on/off') : T('no boiler module')) +
      (s.external === '1' ? T(', with an external device') : '')));
  }
  /* the cooling card: for a boiler that can, over OpenTherm, or an
     external device (no boiler module, the setting on) */
  const cool = $('#cool-state');
  thOutside = s.auto_ok === '1';
  $('#cool-card').hidden = $('#th-nav-cool').hidden = s.ot_cool !== '1' && !th.external;
  cool.className = 'state ' + (s.cool === '1' ? 'ok' : '');
  cool.replaceChildren();
  const outRow = () => row('Outside', `${s.outside} °C (${s.outside_source === 'opentherm' ? 'OpenTherm' : T('sensor')})`);
  if (s.outside) cool.append(outRow());
  $('#th-outstate').replaceChildren(s.outside ? outRow() : row('Outside', T('not known')));
  $('#th-maxnow').replaceChildren(...(s.max_flow_now ? [row('Maximum now', `${+(+s.max_flow_now).toFixed(1)} °C`)] : []));
  if (s.cool_on === '1') cool.append(row('Now on', s.season === 'cool' ? 'Cooling' : 'Heating'));
  thAuto();
  /* the hot water card */
  const hw = $('#hw-state');
  hw.className = 'state ' + (s.hw_active === '1' ? 'warn' : '');
  hw.replaceChildren();
  $('#hw-card').hidden = $('#th-nav-water').hidden = s.ot_dhw !== '1';
  thFold();
  thShow();
  if (s.hw_setpoint) {
    if (s.hw_temp) hw.append(row('Now', `${s.hw_temp} °C`));
    hw.append(row('Set to', `${s.hw_setpoint} °C`));
    if (s.hw_wanted && s.hw_wanted !== s.hw_setpoint) hw.append(row('Next', T`${s.hw_wanted} °C, within a minute`));
    hw.append(row('State', s.hw_active === '1' ? 'Active' : 'Idle'));
    if (s.hw_min) hw.append(row('Limits of the installation', `${s.hw_min} – ${s.hw_max} °C`));
  }
  /* the boiler connection card */
  const conn = $('#th-conn'), ot = s.mode === 'opentherm', look = s.mode === 'detecting';
  const silent = s.link === 'ok' && ot && s.ot !== '1', esp = s.transport === 'esphome';
  if (!th.connection && th.external && s.link !== thLink) { thLink = s.link; viaSet(); }
  thLink = s.link;
  /* external without a boiler module: no fault, the way it is meant */
  const ext = !esp && s.link !== 'ok' && viaNow() === 'external';
  conn.className = 'state ' + (ext ? 'ok' : s.link !== 'ok' ? 'bad' : silent ? 'warn' : ot ? 'ok' : '');
  conn.replaceChildren(el('b', { textContent: ext ? 'External: through Home Assistant' :
    s.link !== 'ok' ? (esp ? 'ESP not connected' : 'No boiler module') :
    look ? 'Looking for OpenTherm…' : silent ? 'OpenTherm, not answering' :
    ot ? (esp ? 'OpenTherm over WiFi (ESPHome)' : 'OpenTherm') : 'On/off contact' }));
  if (s.message && !ext) conn.append(row(esp ? 'ESP' : 'Module', T(s.message)));
  $('#th-ot').disabled = $('#th-onoff').disabled = s.link !== 'ok' || look;
  $('#th-ot').hidden = $('#th-onoff').hidden = esp;
  const es = $('#esp-state');
  es.hidden = !esp;
  if (esp) {
    es.className = 'state ' + (s.link !== 'ok' ? 'bad' : silent ? 'warn' : 'ok');
    es.replaceChildren(el('b', { textContent: s.link !== 'ok' ? 'Not connected' : silent ? 'Connected, the installation does not answer' :
      'Connected' }));
    if (s.esp_name) es.append(row('Device', `${s.esp_name} · ESPHome ${s.esp_version}` +
      (s.esp_project_version ? ` · ReToon ${s.esp_project_version}` : '')));
    if (s.esp_host) es.append(row('Address', s.esp_host));
    /* package v1: no hot water setpoint or cooling, every reading sent */
    if (s.esp_project_version && parseFloat(s.esp_project_version) < 2)
      es.append(row('Configuration', T('Older version: install the configuration below again for setting the hot water temperature and for cooling')));
    /* the module's 1-wire sensor, when ticked: shows at once whether it works */
    if (s.link === 'ok' && espConf && espConf.one_wire_pin)
      es.append(row('Temperature sensor', s.esp_temperature ? `${s.esp_temperature} °C` : T('no reading')));
    if (s.message) es.append(row('Message', T(s.message)));
  }
  $('#th-hvac').checked = on;
  $('#th-resume').hidden = s.hold !== '1' || s.program !== '1';
  $('#th-learned').textContent = !s.heat_rate ? '' : (s.measured >= 3 ?
    T`Learned so far: the house warms up about ${s.heat_rate} °C an hour with the heating at full temperature` :
    T`Not learned yet how fast the house warms up (${s.measured || 0} of 3 measurements); assuming ${s.heat_rate} °C an hour with the heating at full temperature`) +
    (s.cool_per_10 ? T` and loses about ${s.cool_per_10} °C an hour without heating for every 10 °C it is colder outside` +
      (s.cool_rate_now ? T` (now ${Math.abs(s.cool_rate_now)} °C an hour).` : '.') :
      T` and loses about ${Math.abs(s.cool_rate)} °C an hour without heating.`) +
    (s.after_10 ? T` After 10 minutes of heating the room still rises about ${s.after_10} °C from the warm radiators; the thermostat stops earlier for that.` : '') +
    (s.preheat_n && Math.abs(+s.preheat_extra) >= 1 ? (+s.preheat_extra > 0 ?
      T` Starting early begins ${Math.round(+s.preheat_extra)} minutes sooner than calculated, from ${s.preheat_n} mornings.` :
      T` Starting early begins ${Math.round(-s.preheat_extra)} minutes later than calculated, from ${s.preheat_n} mornings.`) : '') +
    (s.from_vendor === '1' ? T` Started from what the Toon's original software had learned.` : '') +
    (s.flow && s.heat === '1' ? T` Flow temperature asked for now: ${Math.round(s.flow)} °C.` : '');
}

async function loadThermostat() {
  try {
    const saved = await (await api('thermostat')).json();
    th = { ...thDefault(), ...saved, presets: { ...thDefault().presets, ...(saved.presets || {}) } };
    if (!Array.isArray(th.sensors) || !th.sensors.length) th.sensors = ['toon'];
    if (!Array.isArray(th.program) || th.program.length !== 7) th.program = thDefault().program;
    th.presets_cool = { ...thDefault().presets_cool, ...(saved.presets_cool || {}) };
    th.cooling = { ...coolDefault(), ...(saved.cooling || {}) };
    th.outside_entity = saved.outside_entity || th.cooling.outside_entity || '';
    th.max_flow_curve = { ...curveDefault(), ...(saved.max_flow_curve || {}) };
    th.hot_water = { ...hwDefault(), ...(saved.hot_water || {}) };
    if (!Array.isArray(th.hot_water.program) || th.hot_water.program.length !== 7) th.hot_water.program = hwDefault().program;
  } catch (e) { th = thDefault(); }
  drawThermostat();
  loadThState();
  loadEsp();
}
$('#th-save').onclick = async () => {
  thRead();
  try {
    /* the sensor in use is chosen in Home Assistant: keep its latest choice */
    try {
      const cur = await (await api('thermostat')).json();
      if (cur.sensor_use) th.sensor_use = cur.sensor_use; else delete th.sensor_use;
    } catch (e) { /* saved as it was loaded */ }
    await api('thermostat', JSON.stringify(th));
    say('Saved', 'ok');
    $('#th-save').disabled = true;
    $('#th-dirty').textContent = '';
    drawThermostat();
    setTimeout(loadThState, 2000);	/* a new sensor correction counts within a second */
    setTimeout(loadThState, 12000);
  } catch (e) { fail(e); }
};
$('#th-hvac').onchange = async ev => {
  try { await api('thermostat/cmd', `hvac ${ev.target.checked ? 'heat' : 'off'}\n`); setTimeout(loadThState, 1500); } catch (e) { fail(e); }
};
$('#th-ot').onclick = async () => {
  try {
    await api('thermostat/cmd', 'mode ot\n'); say('Looking for OpenTherm…', 'ok');
    setTimeout(loadThState, 1500); setTimeout(loadThState, 7000);
  } catch (e) { fail(e); }
};
$('#th-onoff').onclick = async () => {
  if (!confirm(T('Use the on/off contact? The Toon then no longer looks for OpenTherm by itself.'))) return;
  try { await api('thermostat/cmd', 'mode onoff\n'); say('Done', 'ok'); setTimeout(loadThState, 1500); } catch (e) { fail(e); }
};
/* ---- the OpenTherm module on an ESP (ESPHome, ReToon-esphome) ---- */
const ESP_REPO = 'github://ToonSoftwareCollective/ReToon-esphome';
let espConf = null;
function espYaml(c) {
  const board = c.board === 'esp8266' ? 'esp8266:\n  board: d1_mini' : 'esp32:\n  board: esp32dev';
  return `# ReToon OpenTherm module for ${c.host}, made by ReToon's web interface.
# wifi_ssid and wifi_password come from ESPHome's secrets.
substitutions:
  name: ${c.name}
  in_pin: ${c.in_pin}
  out_pin: ${c.out_pin}
` + (c.one_wire_pin ? `  one_wire_pin: ${c.one_wire_pin}\n` : '') + `  api_key: "${c.key}"
  ota_password: "${c.ota}"

${board}

packages:
  retoon: ${ESP_REPO}/opentherm.yaml@v2
` + (c.one_wire_pin ? `  retoon_temperature: ${ESP_REPO}/temperature.yaml@v2\n` : '');
}
$('#esp-1w').onchange = () => {
  $('#esp-1w-row').hidden = !$('#esp-1w').checked;
  if ($('#esp-1w').checked && !$('#esp-1w-pin').value) $('#esp-1w-pin').value = 'GPIO14';
};
async function loadEsp() {
  try { espConf = await (await api('boiler/esphome')).json(); } catch (e) { return; }
  $('#esp-board').value = espConf.board || 'esp32';
  $('#esp-name').value = espConf.name || 'retoon-opentherm';
  $('#esp-in').value = espConf.in_pin || 'GPIO4';
  $('#esp-out').value = espConf.out_pin || 'GPIO5';
  $('#esp-1w').checked = !!espConf.one_wire_pin;
  $('#esp-1w-row').hidden = !espConf.one_wire_pin;
  $('#esp-1w-pin').value = espConf.one_wire_pin || '';
  $('#esp-host').value = espConf.host && espConf.host !== `${espConf.name}.local` ? espConf.host : '';
  $('#esp-yaml-box').hidden = !espConf.on;
  $('#esp-save').textContent = T(espConf.on ? 'Save' : 'Use this ESP');
  if (espConf.on) $('#esp-yaml').value = espYaml(espConf);
  viaSet();
}
/* the boiler connection: wired (the module), wireless (an ESP set up here)
   or external (Home Assistant switches a device; th.connection remembers
   that it was chosen, th.external is what toon-boilerd uses) */
let thLink = '';	/* the boiler link, from the last state */
function viaNow() {
  if (espConf && espConf.on) return 'esp';
  /* settings from before the choice: external without a boiler module */
  const ext = th.connection ? th.connection === 'external' : th.external && thLink && thLink !== 'ok';
  return ext ? 'external' : 'cable';
}
function viaSet() { $(`#via-${viaNow()}`).checked = true; viaShow(); }
/* only the part for the choice; the airco next to a boiler not for external */
function viaShow() {
  const v = (document.querySelector('input[name=boiler-via]:checked') || {}).value || 'cable';
  $('#via-cable-box').hidden = v !== 'cable';
  $('#via-esp-box').hidden = v !== 'esp';
  $('#via-external-box').hidden = v !== 'external';
  $('#th-external-row').hidden = v === 'external';
}
/* wired or external: saved at once in the thermostat settings as they were
   saved (not with other changes on the page) */
async function setConnection(conn) {
  const cur = await (await api('thermostat')).json();
  cur.connection = conn;
  cur.external = conn === 'external';
  await api('thermostat', JSON.stringify(cur));
  th.connection = conn;
  th.external = cur.external;
  $('#th-external').checked = th.external;
}
$('#esp-save').onclick = async () => {
  const name = $('#esp-name').value.trim().toLowerCase();
  try {
    /* from external: the boiler again, now through the ESP */
    if (th.connection === 'external') await setConnection('module');
    await api('boiler/esphome', lines({ name, board: $('#esp-board').value, in_pin: $('#esp-in').value.trim().toUpperCase(),
      out_pin: $('#esp-out').value.trim().toUpperCase(), host: $('#esp-host').value.trim(),
      one_wire_pin: $('#esp-1w').checked ? $('#esp-1w-pin').value.trim().toUpperCase() : '' }));
    say('Saved: the thermostat now connects to the ESP. Install the configuration below on it.', 'ok');
    await loadEsp();
    setTimeout(loadThState, 3000); setTimeout(loadThState, 12000);
  } catch (e) { fail(e); }
};
for (const r of document.querySelectorAll('input[name=boiler-via]')) r.onchange = async () => {
  const want = r.value, was = viaNow();
  if (want === was) return viaShow();
  /* wireless: its fields first; it counts from Use this ESP */
  if (want === 'esp') return viaShow();
  if (was === 'esp') {
    if (!confirm(T(want === 'external' ? 'Stop using the ESP? It then no longer gets the heating demand.' :
      'Back to the boiler module on the cable? The ESP then no longer gets the heating demand.'))) return viaSet();
    try { await api('boiler/esphome', 'off'); } catch (e) { fail(e); return viaSet(); }
  }
  try {
    await setConnection(want === 'external' ? 'external' : 'module');
    say('Done', 'ok');
    await loadEsp();
    setTimeout(loadThState, 3000);
  } catch (e) { fail(e); viaSet(); }
};
$('#esp-copy').onclick = async () => {
  try { await navigator.clipboard.writeText($('#esp-yaml').value); say('Copied', 'ok'); }
  catch (e) { $('#esp-yaml').select(); say('Select and copy it yourself (the browser does not allow it here)'); }
};
$('#esp-download').onclick = () => {
  const a = el('a', { href: URL.createObjectURL(new Blob([$('#esp-yaml').value], { type: 'text/yaml' })),
    download: `${espConf.name || 'retoon-opentherm'}.yaml` });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
};
$('#th-resume').onclick = async () => {
  try { await api('thermostat/cmd', 'program resume\n'); setTimeout(loadThState, 1500); } catch (e) { fail(e); }
};
setInterval(() => { if (tabShown('thermostat') && !bus.connected()) loadThState(); }, 10000);


$('#f-boot').onsubmit = async ev => {
  ev.preventDefault();
  const on = ev.target.autoboot.checked;
  if (!on && !confirm(T('The next restart will start the original Toon software, and ReToon will not come back by itself. Continue?'))) return;
  try {
    await api('settings/boot', lines({ autoboot: on ? 1 : 0 }));
    say(on ? 'Saved. ReToon starts at every restart.' : 'Saved. The next restart starts the original Toon software.', 'ok');
  } catch (e) { fail(e); }
};
$('#f-web').onsubmit = async ev => {
  ev.preventDefault();
  try {
    await api('settings/web', lines({ http: ev.target.http.checked ? 'on' : 'redirect' }));
    say('Saved. The web interface restarts.', 'ok');
  } catch (e) { fail(e); }
};

/* the user's own MQTT topics, each one a value that cards can show */
const KINDS = { sensor: 'Value', binary_sensor: 'On / off (shown only)', switch: 'Switch' };
const slug = t => t.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
function drawTopics() {
  const inp = (t, key, label, ph = '') => {
    const i = el('input', { value: t[key] || '', placeholder: ph });
    i.oninput = () => { t[key] = i.value.trim(); };
    return el('label', { textContent: label }, i);
  };
  $('#topics').replaceChildren(...topics.map((t, n) => {
    const kind = el('select', {}, Object.entries(KINDS).map(([k, v]) => el('option', { value: k, textContent: v, selected: (t.kind || 'sensor') === k })));
    kind.onchange = () => { t.kind = kind.value; drawTopics(); };
    const sw = t.kind && t.kind !== 'sensor';
    return el('div', { className: 'section' },
      el('div', { className: 'head' }, el('span', { textContent: t.name || 'New value' }),
        el('button', { className: 'icon danger', type: 'button', textContent: '✕', title: 'Remove', onclick: () => { topics.splice(n, 1); drawTopics(); } })),
      inp(t, 'name', 'Name'), el('label', { textContent: 'Kind' }, kind),
      inp(t, 'topic', 'Topic', 'p1/power'),
      inp(t, 'field', 'Field, when the message is JSON', 'data.power'),
      sw ? [inp(t, 'payload_on', 'Text that means on', 'ON'), inp(t, 'payload_off', 'Text that means off', 'OFF')] : inp(t, 'unit', 'Unit', 'W'),
      t.kind === 'switch' ? inp(t, 'command_topic', 'Topic to switch it', 'plug/set') : [],
      inp(t, 'icon', 'Icon (empty = automatic)', 'mdi:flash'));
  }));
}
async function loadTopics() {
  try { topics = await (await api('mqtt/topics')).json(); } catch (e) { topics = []; }
  drawTopics();
}
$('#topic-add').onclick = () => { topics.push({ kind: 'sensor' }); drawTopics(); };
$('#f-topics').onsubmit = async ev => {
  ev.preventDefault();
  const used = new Set();
  for (const t of topics) {
    if (!t.topic) return fail(new Error('Every value needs a topic'));
    if (!t.name) t.name = t.topic;
    let id = t.id || 'mqtt.' + (slug(t.name) || 'value');
    while (used.has(id)) id += '_2';
    used.add(t.id = id);
  }
  try {
    await api('mqtt/topics', JSON.stringify(topics));
    say('Saved', 'ok');
    setTimeout(loadEntities, 4000);
  } catch (e) { fail(e); }
};

/* brightness, screen off and the pages are one settings file: each Save sends all */
const saveUi = async ev => {
  ev.preventDefault();
  const f = $('#f-ui'), h = $('#f-home'), o = $('#f-off');
  try {
    await api('settings/ui', lines({ brightness: Math.min(100, Math.max(5, parseInt(f.brightness.value) || 80)), dim_after: parseInt(f.dim_after.value) || 0, dim_level: Math.min(100, Math.max(0, isNaN(parseInt(f.dim_level.value)) ? 10 : parseInt(f.dim_level.value))), dim_grey: f.dim_grey.checked ? 1 : 0, sleep_off: o.sleep_off.checked ? 1 : 0, off_from: o.off_from.value || '', off_to: o.off_to.value || '', home_auto: h.home_auto.checked ? 1 : 0, home_after: Math.min(3600, Math.max(3, parseInt(h.home_after.value) || 10)), page_anim: h.page_anim.checked ? 1 : 0,
      /* the old setting for every thermostat card (now per card): kept */
      ...(lastSettings.thermostat_buttons === '0' ? { thermostat_buttons: 0 } : {}) }));
    say('Saved', 'ok');
    setTimeout(refreshScreen, 1500);
  } catch (e) { fail(e); }
};
$('#f-ui').onsubmit = saveUi;
$('#f-home').onsubmit = saveUi;
$('#f-off').onsubmit = saveUi;
$('#f-pw').onsubmit = async ev => {
  ev.preventDefault();
  if (ev.target.password.value !== ev.target.password2.value) return fail(new Error('The two passwords are not the same'));
  try { await api('settings/password', lines({ password: ev.target.password.value })); ev.target.password.value = ev.target.password2.value = ''; say('Changed. Your browser will ask for it.', 'ok'); }
  catch (e) { fail(e); }
};
$('#f-rootpw').onsubmit = async ev => {
  ev.preventDefault();
  try {
    await api('settings/rootpw', lines({ password: ev.target.password.value }));
    ev.target.password.value = '';
    $('#rootpw-state').textContent = 'You have set your own password.';
    say('Changed', 'ok');
  } catch (e) { fail(e); }
};
$('#factory').onclick = async () => {
  if (!confirm(T('Remove ALL settings of this ReToon and restart it? This cannot be undone.'))) return;
  if (prompt(T('Type RESET to go back to factory defaults.')) !== 'RESET') return;
  try {
    await api('settings/factory', 'confirm=factory-defaults\n');
    say('All settings are removed. The ReToon restarts and shows its WiFi screen; this page will stop answering.', 'ok');
  } catch (e) { fail(e); }
};
$('#touch-cal').onclick = async () => {
  try { await api('ui', 'calibrate\n'); say("Tap the crosses on the Toon's screen", 'ok'); setTimeout(refreshScreen, 1500); } catch (e) { fail(e); }
};
$('#touch-reset').onclick = async () => {
  if (!confirm(T('Forget the calibration made on this Toon? It goes back to the one from the factory, or to the screen\'s own.'))) return;
  try { await api('ui', 'calibrate reset\n'); say('Done', 'ok'); setTimeout(refreshScreen, 1500); } catch (e) { fail(e); }
};
/* the same as a message from Home Assistant: see notify.c */
const notify = async msg => {
  try { await api('notify', JSON.stringify(msg)); say('Look at the Toon', 'ok'); setTimeout(refreshScreen, 1000); } catch (e) { fail(e); }
};
$('#note-banner').onclick = () => notify({ title: T('Test'), message: T('This is how a short message looks on the Toon.'), sound: 'beep' });
$('#note-popup').onclick = () => notify({ title: T('Test'), message: T('A message with buttons stays until one is tapped.'),
  level: 'warning', buttons: [T('OK'), T('Later')] });
$('#reset-dash').onclick = async () => {
  if (!confirm(T('Remove your pages and put back the standard ones?'))) return;
  try { await api('dashboard/reset', ''); await loadDash(); say('Done', 'ok'); } catch (e) { fail(e); }
};

async function loadStatus() {
  const s = await (await api('status')).json();
  const rows = [['ReToon', s.version || 'own build'], ['Name', s.hostname], ['Address', s.ip], ['WiFi signal', s.wifi_signal ? `${s.wifi_signal} dBm` : '-'],
    ['Home Assistant', s.ha || '-'], ['MQTT', s.mqtt || '-'], ['Installation', s.boiler_link === 'ok' ? `${s.boiler_mode} — ${s.boiler_message}` : (s.boiler_message || 'not found')],
    ['Room', `${s.temperature} °C` + (s.humidity ? `, ${s.humidity} %` : '')], ['Running for', T`${Math.floor(s.uptime / 3600)} h ${Math.floor(s.uptime % 3600 / 60)} min`],
    ['System', s.system]];
  if (s.m4) rows.splice(rows.length - 2, 0, ['Co-processor', s.m4]);
  $('#status').replaceChildren(...rows.map(([k, v]) => el('tr', {}, el('td', { textContent: k }), el('td', { textContent: v }))));
}

/* update from the newest release of ReToon-images (toon-update), and restart */
let updateTimer = null;
async function loadUpdate() {
  const u = await (await api('update')).json();
  const busy = u.state === 'checking' || u.state === 'working';
  $('#update-check').disabled = $('#update-install').disabled = $('#restart').disabled = busy;
  $('#update-install').hidden = u.state !== 'available';
  $('#update-install').textContent = T`Install ReToon ${u.app}`;
  $('#update-text').textContent = u.state === 'none' ? '' : u.text;
  $('#update-text').className = 'hint ' + ({ error: 'bad', done: 'ok', current: 'ok' }[u.state] || '');
  $('#update-notes').hidden = !u.notes || !['available', 'done'].includes(u.state);
  $('#update-notes').textContent = u.notes || '';
  clearTimeout(updateTimer);
  if (busy) updateTimer = setTimeout(() => loadUpdate().catch(fail), 2000);
  return u;
}
$('#update-check').onclick = async () => {
  try { await api('update/check', ''); await loadUpdate(); } catch (e) { fail(e); }
};
$('#update-install').onclick = async () => {
  if (!confirm(T('Fetch the new ReToon and put it in place? It is used from the next restart.'))) return;
  try { await api('update/install', ''); await loadUpdate(); } catch (e) { fail(e); }
};
$('#restart').onclick = async () => {
  // where the restart goes: as on the screen, plus once to ReToon after an update
  let back = false;
  try {
    const [s, u] = await Promise.all([api('settings').then(r => r.json()), api('update').then(r => r.json())]);
    back = s.autoboot === '1' || u.state === 'done';
  } catch (e) { /* ask with the careful text */ }
  if (!confirm(T(back ? 'Restart the Toon now? ReToon starts again.' :
    'Restart the Toon now? It will start the original Toon software, because "Always start ReToon" is off (System tab).'))) return;
  try { await api('restart', ''); say('Restarting… this page works again in about a minute.', 'ok'); } catch (e) { fail(e); }
};

/* ---- apps ------------------------------------------------------------------------ */

async function loadApps() {
  try { apps = await (await api('apps')).json(); } catch (e) { apps = []; }
}

/* -1, 0 or 1 for versions like "1.2.3" */
function cmpVersion(a, b) {
  const x = String(a).split('.').map(Number), y = String(b).split('.').map(Number);
  for (let i = 0; i < Math.max(x.length, y.length); i++)
    if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) < (y[i] || 0) ? -1 : 1;
  return 0;
}

/* install, update or remove an app from the catalogue; the screen restarts */
function appAction(what, id, name, button) {
  return async ev => {
    ev.preventDefault();
    if (what === 'remove' && !confirm(T`Remove ${name} and its settings?`)) return;
    button.disabled = true;
    button.textContent = { install: 'Installing…', update: 'Updating…', remove: 'Removing…' }[what];
    try {
      await api(`apps/${id}/${what === 'remove' ? 'remove' : 'install'}`, '');
      say({ install: T`${name} is installed`, update: T`${name} is updated`, remove: T`${name} is removed` }[what] +
        '. The screen restarts.', 'ok');
    } catch (e) { fail(e); }
    drawApps().catch(fail);
  };
}

/* one field of an app's settings form (the Apps tab, or an app card's own
   settings): s from the manifest, v its value now; inputs[s.key] gets a
   function that gives the value to save */
function settingRow(s, v, inputs) {
  /* "units": only entities whose state is in one of these units */
  const unitOk = Array.isArray(s.units) && s.units.length ?
    e => s.units.includes((e.state || '').split(' ').slice(1).join(' ')) : undefined;
  let inp;
  if (s.type === 'entity') {
    let cur = typeof v === 'string' ? v : '';
    inputs[s.key] = () => cur;
    return el('label', { textContent: s.label || s.key }, ...entityPicker(() => cur, id => { cur = id; }, unitOk));
  }
  if (s.type === 'entities') {
    /* a list of { entity, <fields> }: fields are text, number or bool */
    const list = (Array.isArray(v) ? v : []).map(x => typeof x === 'string' ? { entity: x } : { ...x });
    const fields = s.fields || [];
    const box = el('div', { className: 'app-list' });
    const draw = () => box.replaceChildren(...list.map((row, i) => el('div', { className: 'app-row' },
      ...entityPicker(() => row.entity, id => { row.entity = id; }, unitOk),
      ...fields.map(f => {
        if (f.type === 'bool') {
          const c = el('input', { type: 'checkbox', checked: !!row[f.key] });
          c.onchange = () => { row[f.key] = c.checked; };
          return el('label', { className: 'check' }, c, f.label || f.key);
        }
        const t = el('input', { value: row[f.key] ?? '', placeholder: f.label || f.key,
          inputMode: f.type === 'number' ? 'decimal' : 'text' });
        t.oninput = () => { row[f.key] = f.type === 'number' ? parseFloat(t.value.replace(',', '.')) : t.value.trim(); };
        return t;
      }),
      el('button', { className: 'icon', type: 'button', textContent: '✕', title: 'Remove',
        onclick: () => { list.splice(i, 1); draw(); } }))));
    draw();
    inputs[s.key] = () => list.filter(r => r.entity);
    return el('div', {}, el('label', { textContent: s.label || s.key }), box,
      el('button', { type: 'button', textContent: s.addLabel || 'Add',
        onclick: () => { if (!s.max || list.length < s.max) { list.push({ entity: '' }); draw(); } } }));
  }
  if (s.type === 'color') {
    /* a colour name or #rrggbb; '' (Standard): the app's own */
    const hold = { c: typeof v === 'string' ? v : '' };
    inputs[s.key] = () => hold.c || '';
    return el('label', { textContent: s.label || s.key }, colorPicker(hold, 'c'));
  }
  if (s.type === 'select') {
    /* options: values, or { value, label } */
    const opts = (Array.isArray(s.options) ? s.options : []).map(o => typeof o === 'object' && o ?
      { value: String(o.value), label: o.label || String(o.value) } : { value: String(o), label: String(o) });
    inp = el('select', {}, opts.map(o => el('option', { value: o.value, textContent: o.label, selected: String(v) === o.value })));
    inputs[s.key] = () => inp.value;
    return el('label', { textContent: s.label || s.key }, inp);
  }
  if (s.type === 'bool') {
    inp = el('input', { type: 'checkbox', checked: !!v });
    inputs[s.key] = () => inp.checked;
    return el('label', { className: 'check' }, inp, s.label || s.key);
  }
  inp = el('input', { value: v === undefined || v === null ? '' : v,
    type: s.type === 'secret' ? 'password' : 'text', inputMode: s.type === 'number' ? 'decimal' : 'text' });
  inputs[s.key] = () => s.type === 'number' ? parseFloat(inp.value.replace(',', '.')) : inp.value.trim();
  return el('label', { textContent: s.label || s.key }, inp);
}

/* the installed apps, each with a form made from the settings its manifest
   lists, and below them what the catalogue has that is not installed */
async function drawApps() {
  const cat = api('apps/catalogue').then(r => r.json()).catch(e => ({ error: e.message }));
  await loadApps();
  const offered = {}, level = (await cat).api || 1;
  for (const c of ((await cat).index || {}).apps || []) offered[c.id] = c;
  const box = $('#apps');
  if (!apps.length)
    box.replaceChildren(el('p', { className: 'hint', textContent: 'No apps are installed on this ReToon.' }));
  else box.replaceChildren(...apps.map(a => {
    const m = a.manifest, inputs = {};
    const rows = (m.settings || []).map(s => settingRow(s, s.key in a.values ? a.values[s.key] : s.default, inputs));
    const name = m.name || a.dir, c = offered[a.dir];
    const manage = el('div', { className: 'row' });
    if (c && cmpVersion(c.version, m.version) > 0) {
      const b = el('button', { type: 'button', textContent: T`Update to ${c.version}` });
      if (c.api > level) Object.assign(b, { disabled: true, title: 'Needs a newer ReToon' });
      else b.onclick = appAction('update', a.dir, name, b);
      manage.append(b);
    }
    const rm = el('button', { type: 'button', className: 'danger', textContent: 'Remove' });
    rm.onclick = appAction('remove', a.dir, name, rm);
    manage.append(rm);
    const form = el('form', { className: 'card' },
      el('h2', { textContent: `${name} ` }, el('small', { textContent: m.version || '' })),
      m.description ? el('p', { className: 'hint', textContent: m.description }) : '',
      appServices(m.services),
      appCameras(m.cameras),
      ...rows,
      rows.length ? el('button', { className: 'primary', textContent: 'Save' }) : el('p', { className: 'hint', textContent: 'This app has no settings.' }),
      manage);
    form.onsubmit = async ev => {
      ev.preventDefault();
      const out = {};
      for (const [k, get] of Object.entries(inputs)) {
        const v = get();
        if (typeof v === 'number' && isNaN(v)) return say(T`${k}: not a number`, 'bad');
        out[k] = v;
      }
      try { await api(`apps/${a.dir}/settings`, JSON.stringify(out)); say('Saved', 'ok'); } catch (e) { fail(e); }
    };
    return form;
  }));

  const more = Object.values(offered).filter(c => !apps.some(a => a.dir === c.id));
  const hint = (await cat).error ? (await cat).error :
    !more.length ? 'Every app in the catalogue is installed.' : '';
  $('#catalogue').replaceChildren(hint ? el('p', { className: 'hint', textContent: hint }) : '', ...more.map(c => {
    const b = el('button', { className: 'primary', type: 'button', textContent: 'Install' });
    if (c.api > level) Object.assign(b, { disabled: true, textContent: 'Needs a newer ReToon' });
    else b.onclick = appAction('install', c.id, c.name, b);
    return el('div', { className: 'card' },
      el('h2', { textContent: `${c.name} ` }, el('small', { textContent: c.version })),
      el('p', { className: 'hint', textContent: c.description }),
      el('p', { className: 'hint', textContent: T`By ${c.author}` }),
      appServices(c.services),
      appCameras(c.cameras),
      b);
  }));
}

/* what an app may do in Home Assistant (the manifest's "services", level 7) */
const appServices = list => Array.isArray(list) && list.length ?
  el('p', { className: 'hint', textContent: T`May use in Home Assistant: ${list.join(', ')}` }) : '';
const appCameras = list => Array.isArray(list) && list.length ?
  el('p', { className: 'hint', textContent: T`May show cameras: ${list.map(c => c === 'camera.*' ? T('all') : c).join(', ')}` }) : '';

/* ---- overview ------------------------------------------------------------------ */
const tabShown = n => !$(`#tab-${n}`).hidden;
const box = (b, cls, ...rows) => { b.className = 'state ' + cls; b.replaceChildren(rows[0], ...rows.slice(1)); };
const stRow = (k, v) => el('div', {}, el('span', { textContent: k }), v);
const signal = d => d >= -60 ? 'strong signal' : d >= -72 ? 'fair signal' : 'weak signal';

function tile(name, dot, state, detail, tab) {
  return el('button', { type: 'button', className: 'tile', onclick: () => showTab(tab) },
    el('span', { className: 'pill' }, el('span', { className: 'dot ' + dot }), state),
    el('b', { textContent: name }), el('small', { textContent: detail || '' }));
}

/* one status per connection, the same words as on the Connections tab */
function connections(s, st) {
  const out = [];
  const sig = parseInt(st.wifi_signal);
  out.push(['WiFi', isNaN(sig) ? 'bad' : sig < -72 ? 'warn' : 'ok', isNaN(sig) ? 'No signal' : 'Connected',
    isNaN(sig) ? '' : T`${T(signal(sig))} (${sig} dBm) · ${st.ip || ''}`]);
  const haUp = s.ha_state === 'Connected';
  out.push(['Home Assistant', s.ha_auth === 'none' ? 'off' : haUp ? 'ok' : 'warn',
    s.ha_auth === 'none' ? 'Not connected' : haUp ? 'Connected' : s.ha_state || 'Connecting',
    s.ha_auth === 'none' ? 'Log in under Connections' : (s.ha_url || '').replace(/^https?:\/\//, '')]);
  const mq = st.mqtt || '', own = !s.mqtt_host && /^Own broker/.test(mq);
  out.push(['MQTT', own ? 'ok' : !s.mqtt_host ? 'off' : /^Connected/.test(mq) ? 'ok' : 'warn',
    own ? 'Own broker' : !s.mqtt_host ? 'Not set up' : mq.split(',')[0] || 'Connecting',
    own ? mq.split(',').slice(1).join(',').trim() : !s.mqtt_host ? 'Optional' : mq.split(',').slice(1).join(',').trim() || s.mqtt_host]);
  const zon = s.zwave_mode === 'remote', zused = s.zwave_state === 'connected';
  out.push(['Z-Wave', !zon ? 'off' : zused ? 'ok' : 'warn', !zon ? 'Off' : zused ? 'In use' : 'Waiting',
    !zon ? 'For Z-Wave JS' : zused ? 'Z-Wave JS connected' : 'for Z-Wave JS to connect']);
  if (s.esphome_available === '1') {
    const eon = s.esphome_mode !== 'off', eused = s.esphome_state === 'connected';
    out.push(['Bluetooth proxy', !eon ? 'off' : eused ? 'ok' : 'warn', !eon ? 'Off' : eused ? 'In use' : 'Waiting',
      !eon ? 'For Home Assistant' : eused ? T`${s.esphome_adverts || 0} advertisements a minute` : 'for Home Assistant to connect']);
  }
  const mode = s.radio_mode || 'zigbee';
  if (mode !== 'none') {
    const up = /^(leader|router|child)$/.test(s.thread_state);
    out.push(['Zigbee or Thread', mode === 'off' ? 'off' : mode === 'zigbee' || up ? 'ok' : 'warn',
      mode === 'off' ? 'Off' : mode === 'zigbee' ? 'Zigbee' : 'Thread',
      mode === 'off' ? '' : mode === 'zigbee' ? 'For zigbee2mqtt' : up ? T`${s.thread_children || 0} devices attached` : 'Starting']);
  }
  return out;
}

/* the air-quality word as on the screen: measuring after a start (with the
   word from before it), a clear trend, or still learning (the first day) */
function airText(st) {
  const word = q => ({ good: T('Good'), fair: T('Fair'), moderate: T('Moderate'), poor: T('Poor') })[q];
  if (st.air === 'measuring') return word(st.air_last) ? T`${word(st.air_last)} · measuring…` : T('Measuring…');
  const w = word(st.air) || st.air;
  if (+st.air_trend) return T(+st.air_trend > 0 ? 'Getting worse' : 'Getting better');
  return +st.air_learning ? T`${w} · learning` : w;
}

async function loadOverview() {
  const get = p => api(p).then(r => r.json()).catch(() => ({}));
  const [s, st, th, up, cfg] = await Promise.all([get('settings'), get('status'), get('thermostat/state'), get('update'), get('thermostat')]);
  if (s.hostname) { $('#hostname').textContent = s.hostname; document.title = `ReToon ${s.hostname}`; }
  const on = th.hvac === 'heat', heating = th.heat === '1';
  const preset = th.preset && th.preset !== 'manual' ? T(PRESETS[th.preset] || th.preset) : T('set by hand');
  const tiles = [tile('Thermostat', !th.hvac ? 'bad' : on ? 'ok' : 'off',
    !th.hvac ? 'Not running' : th.frost === '1' ? 'Frost protection' : on ? `${th.target} °C · ${preset}` : 'Off',
    th.room ? T`Room ${th.room} °C · ${T(heating ? 'heating' : 'not heating')}` : '', 'thermostat')];
  for (const [name, dot, state, detail] of connections(s, st)) tiles.push(tile(name, dot, state, detail, 'connections'));
  $('#tiles').replaceChildren(...tiles);

  $('#update-banner').hidden = !['available', 'done'].includes(up.state);
  $('#update-banner-text').textContent = up.state === 'done' ? T`ReToon ${up.app || ''} is installed; it is used from the next restart.` :
    T`ReToon ${up.app || ''} is available. You have ${st.version || T('an own build')}.`;

  const rows = [['Room', st.temperature ? `${st.temperature} °C` + (st.humidity ? T` · ${st.humidity} % humidity` : '') : '-']];
  if (th.hvac && on) rows.push(['Thermostat', T`${th.target} °C (${preset})` + (th.program === '1' && th.next ?
    T`, next ${th.next.replace(/^(\S+) (\S+)$/, (m, t, p) => T`${T(PRESETS[p] || p)} at ${t}`)}` : '')]);
  if (st.air) rows.push(['Air quality', airText(st)]);
  rows.push(['Installation', st.boiler_link === 'ok' ? `${T(heating ? 'Heating now' : 'Not heating')} · ${st.boiler_mode === 'opentherm' ? 'OpenTherm' : T('on/off')}` :
    st.boiler_message || 'No boiler module']);
  if (st.uptime) rows.push(['Running for', (Math.floor(st.uptime / 86400) ? Math.floor(st.uptime / 86400) + ' d ' : '') + T`${Math.floor(st.uptime % 86400 / 3600)} h ${Math.floor(st.uptime % 3600 / 60)} min`]);
  rows.push(['Version', st.version || 'own build']);
  $('#now').replaceChildren(...rows.map(([k, v]) => el('tr', {}, el('td', { textContent: k }), el('td', { textContent: v }))));

  const steps = [['Connect to Home Assistant', s.ha_auth && s.ha_auth !== 'none', 'connections'],
    ['Set up the thermostat', Object.keys(cfg).length > 0, 'thermostat'],
    ['Turn on "Always start ReToon"', s.autoboot === '1', 'system']];
  let hide = false;
  try { hide = localStorage.stepsHidden === '1'; } catch (e) { /* private window */ }
  $('#getting-started').hidden = hide || steps.every(x => x[1]);
  $('#steps').replaceChildren(...steps.map(([t, done, tab], i) => el('div', { className: 'step' },
    el('span', { className: 'num ' + (done ? 'done' : 'todo'), textContent: done ? '✓' : i + 1 }),
    el('span', { textContent: t, style: 'flex:1' }),
    done ? '' : el('button', { type: 'button', textContent: 'Open', onclick: () => showTab(tab) }))));
}
$('#steps-hide').onclick = () => { try { localStorage.stepsHidden = '1'; } catch (e) { /* private window */ } $('#getting-started').hidden = true; };
for (const b of document.querySelectorAll('[data-go]')) b.onclick = () => showTab(b.dataset.go);
setInterval(() => { if (tabShown('overview') && !document.hidden && !bus.connected()) loadOverview().catch(() => {}); }, 15000);

/* ---- live: the bus over the websocket (toonbus.js) ------------------------------ */
/* A change on the bus of what a tab shows loads that tab again at once (with
   a few seconds between loads); the timers above only run while the bus is
   not connected, so the page works the same without it. */
const soon = (fn, gap) => {
  let timer = null, last = 0;
  return () => {
    if (timer) return;
    timer = setTimeout(() => { timer = null; last = Date.now(); if (!document.hidden) fn(); },
      Math.max(300, last + gap - Date.now()));
  };
};
const liveTh = soon(() => { if (tabShown('thermostat')) loadThState(); }, 3000);
const liveOverview = soon(() => { if (tabShown('overview')) loadOverview().catch(() => {}); }, 10000);
const liveSettings = soon(() => { if (tabShown('connections') || tabShown('overview') || tabShown('screen') || tabShown('system')) loadSettings().catch(() => {}); }, 3000);
const liveConn = soon(() => { if (tabShown('connections')) loadConnStatus().catch(() => {}); }, 5000);
const liveStatus = soon(() => { if (tabShown('system')) loadStatus().catch(() => {}); }, 10000);
/* only the values a tab shows count, not each new measurement */
const seen = {};
const changed = (name, data, keys) => {
  const v = JSON.stringify(data && keys.map(k => data[k]));
  if (seen[name] === v) return false;
  seen[name] = v;
  return true;
};
bus.on('retoon/+', (topic, data) => {
  const name = topic.slice('retoon/'.length), go = [];
  /* the states that come in at the connect only set what was seen */
  if (name === 'boiler') {
    if (changed('th', data, ['hvac', 'target', 'room', 'heat', 'preset', 'program', 'hold', 'frost', 'eff_target', 'link', 'message', 'outside', 'hw_active', 'season']))
      go.push(liveTh);
    if (changed('ov', data, ['hvac', 'target', 'room', 'heat', 'preset', 'frost', 'link'])) go.push(liveOverview);
  } else if (name === 'ui') {
    if (changed('ui', data, ['ha', 'mqtt', 'ha_addr'])) go.push(liveSettings, liveConn, liveOverview, liveStatus);
    if (changed('view', data, ['view']) && tabShown('screen')) go.push(() => setTimeout(refreshScreen, 300));
  } else if (['zwave', 'esphome', 'thread', 'm4', 'voc'].includes(name)) {
    if (changed(name, data, ['state', 'scanning', 'quality', 'text'])) go.push(liveOverview, liveConn, liveStatus);
  }
  if (bus.connected()) for (const fn of go) fn();
});
bus.start();

/* WiFi and MQTT on the Connections tab come from the status, not the settings */
async function loadConnStatus() {
  const st = await (await api('status')).json(), s = await (await api('settings')).json();
  for (const [name, dot, state, detail] of connections(s, st)) {
    if (name === 'WiFi') box($('#wifi-state'), dot === 'off' ? '' : dot, el('b', { textContent: state }), detail ? stRow('Signal', detail) : '');
    if (name === 'MQTT') box($('#mqtt-state'), dot === 'off' ? '' : dot, el('b', { textContent: state }), s.mqtt_host ? stRow('Broker', `${s.mqtt_host}:${s.mqtt_port || 1883}`) : '',
      s.mqtt_host && detail !== s.mqtt_host ? stRow('Found', detail) : '');
  }
}

/* a Save button lights up only when its form has changes */
const ARMED = ['#f-ui', '#f-off', '#f-home', '#f-mqtt', '#f-broker', '#f-zwave', '#f-esphome', '#f-radio', '#f-boot', '#f-name', '#f-sound', '#f-web'];
let lastForm = null;
function armForms() {
  for (const id of ARMED) {
    const f = $(id), b = f && f.querySelector('button[type=submit], button:not([type])');
    if (b && !f.dataset.dirty) b.disabled = true;
  }
}
/* a field the user changed keeps its value until its form is saved */
for (const id of ['#f-ha', ...ARMED]) {
  const f = $(id), mark = ev => { if (ev.target.matches('input, select, textarea')) ev.target.dataset.edited = '1'; };
  if (f) { f.addEventListener('input', mark); f.addEventListener('change', mark); }
}
for (const id of ARMED) {
  const f = $(id);
  if (!f) continue;
  const touch = () => { f.dataset.dirty = '1'; const b = f.querySelector('button[type=submit], button:not([type])'); if (b) b.disabled = false; };
  f.addEventListener('input', touch);
  f.addEventListener('change', touch);
  f.addEventListener('submit', () => { lastForm = f; }, true);
}
/* the thermostat has one Save for all its cards; the on/off switch acts at once */
const thTouch = ev => {
  /* the route to the boiler has buttons of its own (no thermostat setting;
     the external device next to it is one) */
  if (ev.target.closest('#th-nav, #th-hvac, #th-resume, #th-save, #via-cable, #via-esp, #via-cable-box, #via-esp-box, #th-log-show, #th-measure-get')) return;
  if (ev.type === 'click' && !ev.target.closest('button, .picker div')) return;
  $('#th-save').disabled = false;
  $('#th-dirty').textContent = 'Not saved yet';
};
for (const t of ['input', 'change', 'click']) $('#tab-thermostat').addEventListener(t, thTouch);

$('#show-import').onclick = () => { $('#import-box').hidden = false; $('#import-box').scrollIntoView({ behavior: 'smooth' }); };
$('#hide-import').onclick = () => { $('#import-box').hidden = true; };

/* Settings cards in columns, each card into the shortest column so far. Done
   when a tab opens and when the width changes, not when a card grows: opening
   "How to use this" in one card must not move the others to another column. */
const CARD_W = 330, GAP = 18;
/* A card with data-col="2" goes to that column when there are enough; with
   fewer, the cards go in their order to the shortest column. */
function masonry() {
  for (const box of document.querySelectorAll('.masonry')) {
    if (!box.offsetParent) continue;	/* on a hidden tab */
    const min = +box.dataset.min || CARD_W;
    const n = Math.max(1, Math.floor((box.clientWidth + GAP) / (min + GAP)));
    if (+box.dataset.n === n) continue;
    const cards = box.cards || (box.cards = [...box.children]);
    const cols = Array.from({ length: n }, () => el('div', { className: 'mcol' }));
    const fixed = cards.every(c => c.dataset.col && +c.dataset.col <= n);
    box.replaceChildren(...cols);
    for (const c of cards)
      (fixed ? cols[c.dataset.col - 1] : cols.reduce((a, b) => b.offsetHeight < a.offsetHeight ? b : a)).append(c);
    box.dataset.n = n;
  }
}
let resizeTimer;
window.addEventListener('resize', () => { clearTimeout(resizeTimer); resizeTimer = setTimeout(masonry, 150); });

/* ---- start ---------------------------------------------------------------------- */
function showTab(name) {
  if (!$(`#tab-${name}`)) name = 'overview';
  for (const b of document.querySelectorAll('nav button')) b.classList.toggle('on', b.dataset.tab === name);
  for (const s of document.querySelectorAll('main > section')) s.hidden = s.id !== `tab-${name}`;
  $('#savebar').hidden = name !== 'screen';
  if (name === 'overview') loadOverview().catch(fail);
  if (name === 'screen') { loadSettings().catch(fail); refreshScreen(); }
  if (name === 'thermostat') loadThermostat();
  if (name === 'connections') { loadSettings().catch(fail); loadTopics(); loadConnStatus().catch(() => {}); }
  if (name === 'apps') drawApps().catch(fail);
  if (name === 'system') { loadSettings().catch(fail); loadStatus().catch(fail); loadUpdate().catch(fail); }
  try { localStorage.tab = name; } catch (e) { /* private window */ }
  masonry();
  window.scrollTo(0, 0);
}
for (const b of document.querySelectorAll('nav button')) b.onclick = () => showTab(b.dataset.tab);
$('#lang').value = LANG;
$('#lang').onchange = async () => {
  const lang = $('#lang').value;
  try {
    await api('settings/language', lines({ lang }));
    try { localStorage.lang = lang; } catch (e) { /* private window */ }
    location.reload();
  } catch (e) { $('#lang').value = LANG; fail(e); }
};
$('#save').onclick = saveDash;
$('#revert').onclick = () => { dash = JSON.parse(saved); render(); dirty(); };
$('#add-view').onclick = () => {
  if (dash.views.length >= MAX_VIEWS) return alert(T`At most ${MAX_VIEWS} pages fit on the Toon.`);
  dash.views.push({ title: 'New page', type: 'retoon-grid', cards: [] }); cur = dash.views.length - 1; curDim = false; render(); dirty(); };
$('#add-section').onclick = () => editCard(cardsOf(view()), -1);
window.onbeforeunload = () => JSON.stringify(dash) !== saved ? 'Unsaved changes' : undefined;

/* The explanation of a setting sits behind a "?" next to it and shows when
   that is clicked: each p.hint (without an id; those are filled in by the
   code) right after a field, a checkbox, a group of fields or the button
   of a form goes to that field's label. data-help-for="<id>" names the
   field when that guess would be wrong. Introductions under a heading and
   the "How ..." boxes stay where they are. */
let helpOpen = null;
function helpClose() {
  if (!helpOpen) return;
  helpOpen.pop.hidden = true;
  helpOpen.setAttribute('aria-expanded', 'false');
  helpOpen = null;
}
function helpShow(icon) {
  const again = helpOpen === icon;
  helpClose();
  if (again) return;
  const pop = icon.pop, r = icon.getBoundingClientRect();
  pop.hidden = false;
  const left = Math.min(r.left, document.documentElement.clientWidth - pop.offsetWidth - 8);
  pop.style.left = `${Math.max(8, left) + window.scrollX}px`;
  pop.style.top = `${r.bottom + window.scrollY + 6}px`;
  icon.setAttribute('aria-expanded', 'true');
  helpOpen = icon;
}
function helpTarget(p) {
  if (p.dataset.helpFor) {
    const f = document.getElementById(p.dataset.helpFor);
    return f && f.closest('label');
  }
  const prev = p.previousElementSibling;
  if (!prev || prev.matches('.state')) return null;
  if (prev.matches('label')) return prev;
  if (prev.matches('button')) {
    for (let q = prev.previousElementSibling; q; q = q.previousElementSibling)
      if (q.matches('label')) return q;
    return null;
  }
  if (prev.matches('div, table')) return [...prev.querySelectorAll('label')].pop() || null;
  return null;
}
/* a "?" whose explanation opens when it is clicked */
function helpIcon() {
  const icon = el('span', { className: 'help', tabIndex: 0, textContent: '?', title: 'Explanation' });
  icon.setAttribute('role', 'button');
  icon.setAttribute('aria-expanded', 'false');
  icon.pop = el('div', { className: 'help-pop', hidden: true });
  document.body.append(icon.pop);
  /* a click in a label would also tick its checkbox or focus its field */
  const go = e => { e.preventDefault(); e.stopPropagation(); helpShow(icon); };
  icon.addEventListener('click', go);
  icon.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') go(e); });
  return icon;
}
function setupHelp() {
  for (const p of [...document.querySelectorAll('p.hint:not([id])')]) {
    const target = helpTarget(p);
    if (!target) continue;
    let icon = target.querySelector(':scope > .help');
    if (!icon) {
      icon = helpIcon();
      const text = [...target.childNodes].find(n => n.nodeType === 3 && n.nodeValue.trim());
      if (text) text.after(icon); else target.append(icon);
    }
    const part = el('p');
    part.append(...p.childNodes);
    icon.pop.append(part);
    p.remove();
  }
  document.addEventListener('click', e => { if (helpOpen && !helpOpen.pop.contains(e.target)) helpClose(); });
  document.addEventListener('keydown', e => { if (e.key === 'Escape') helpClose(); });
  window.addEventListener('resize', helpClose);
}
setupHelp();

(async () => {
  const q = new URLSearchParams(location.search);
  if (q.get('auth_callback') && q.get('code')) {
    /* back from the Home Assistant login page */
    history.replaceState(null, '', location.pathname);
    try {
      say('Asking Home Assistant for an access token…');
      await api('ha/login', lines({ url: localStorage.haUrl || '', code: q.get('code'), client_id: location.origin + '/',
        verify: localStorage.haVerify || 0 }));
      say('Logged in. This ReToon now uses its own access token.', 'ok');
    } catch (e) { fail(e); }
    showTab('connections');
  } else {
    let tab = 'overview';
    try { tab = localStorage.tab || tab; } catch (e) { /* private window */ }
    showTab(tab);
  }
  try { await loadApps(); await loadSettings(); await loadDash(); await api('ui', `view ${cur}\n`); } catch (e) { fail(e); }
  setTimeout(refreshScreen, 600);
  loadEntities();
})();

/* the thermostat's change log: newest at the top */
$('#th-measure-get').onclick = () => { location.href = API + '/thermostat/measurements'; };
$('#th-log-show').onclick = async () => {
  const pre = $('#th-log-text');
  try {
    const txt = (await (await api('thermostat/log')).text()).trim();
    pre.textContent = txt ? txt.split('\n').reverse().join('\n') :
      T($('#th-log').checked ? 'Nothing logged yet.' : 'The log is off.');
  } catch (e) {
    pre.textContent = e.message;
  }
  pre.hidden = false;
};

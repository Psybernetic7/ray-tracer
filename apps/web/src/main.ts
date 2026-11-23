// The page: controls, camera interaction and drawing. All the heavy lifting happens in the
// engine worker (worker.ts); this file turns clicks and slider moves into messages for it, and
// its replies into pixels and text.

import './style.css';
import type { FromWorker, Info, Mode, Pick, PlaceInfo, TerrainRequest, ToWorker } from './protocol';

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

const stage = $('stage');
const canvas = $<HTMLCanvasElement>('view');
const ctx = canvas.getContext('2d')!;
const scratch = document.createElement('canvas'); // Holds small preview frames before scaling
const scratchCtx = scratch.getContext('2d')!;

const ui = {
  source: $<HTMLSelectElement>('source'),
  fractal: $('fractal-controls'),
  anywhere: $('anywhere-controls'),
  seed: $<HTMLInputElement>('seed'),
  reseed: $<HTMLButtonElement>('reseed'),
  ridge: $<HTMLInputElement>('ridge'),
  warp: $<HTMLInputElement>('warp'),
  height: $<HTMLInputElement>('height'),
  lat: $<HTMLInputElement>('lat'),
  lon: $<HTMLInputElement>('lon'),
  extent: $<HTMLInputElement>('extent'),
  loadRegion: $<HTMLButtonElement>('load-region'),
  presets: $('region-presets'),
  resolution: $<HTMLSelectElement>('resolution'),
  modes: $('modes'),
  modeNote: $('mode-note'),
  sunControls: $('sun-controls'),
  day: $<HTMLInputElement>('day'),
  hour: $<HTMLInputElement>('hour'),
  play: $<HTMLButtonElement>('play'),
  sunOut: $('sun-out'),
  latitudeField: $('latitude-field'),
  latitude: $<HTMLInputElement>('latitude'),
  viewshed: $('viewshed-controls'),
  eye: $<HTMLInputElement>('eye'),
  gridField: $('grid-field'),
  grid: $<HTMLSelectElement>('grid'),
  quality: $<HTMLSelectElement>('quality'),
  resetView: $<HTMLButtonElement>('reset-view'),
  save: $<HTMLButtonElement>('save'),
  stats: $('stats'),
  attribution: $('attribution'),
  placeLabel: $('place-label'),
  placeDetail: $('place-detail'),
  status: $('status'),
  statusText: $('status-text'),
  spinner: $('spinner'),
  readout: $('readout'),
  legend: $('legend'),
  legendTitle: $('legend-title'),
  legendBar: $('legend-bar'),
  legendMin: $('legend-min'),
  legendMax: $('legend-max'),
  legendNote: $('legend-note'),
  hint: $('hint'),
};

// Places reached through "Anywhere on Earth", downloaded on demand.
const REGION_PRESETS = [
  { label: 'Mount Everest', lat: 27.9881, lon: 86.925, extentKm: 20 },
  { label: 'Yosemite Valley', lat: 37.7375, lon: -119.575, extentKm: 14 },
  { label: 'Mount Rainier', lat: 46.8523, lon: -121.7603, extentKm: 24 },
  { label: 'Kilimanjaro', lat: -3.0674, lon: 37.3556, extentKm: 30 },
  { label: 'Table Mountain', lat: -33.9628, lon: 18.4098, extentKm: 10 },
];

const MODE_NOTES: Record<Mode, string> = {
  render: 'Sunlight for any date and time at this latitude. Every shadow is a ray traced toward the sun’s disk.',
  'sun-hours':
    'Hours of direct sun each spot gets in a year. Each cell’s skyline is measured once in 64 directions; after that every sun position is a table lookup, not a ray.',
  irradiance:
    'Direct solar energy reaching the sloped ground in a year, under clear skies: the number solar-panel siting starts from.',
  viewshed: 'Everything in line of sight of the red marker. Click anywhere on the terrain to move it.',
};

const DEFAULT_ORBIT = { azimuth: 152, elevation: 32, distance: 1 }; // Distance 1 fits the terrain

const state = {
  ready: false,
  // ?single forces the single-threaded build, to compare the two.
  threaded: self.crossOriginIsolated === true && !new URLSearchParams(location.search).has('single'),
  threads: 1,
  mode: 'render' as Mode,
  orbit: { ...DEFAULT_ORBIT },
  info: null as Info | null,
  places: [] as PlaceInfo[],
  regionLabel: '',
  playing: 0,
  lastFrame: { width: 0, height: 0, samples: 0 },
  hover: { pending: null as null | { fx: number; fy: number }, inFlight: false },
  statusTimer: 0,
};

// --- The engine worker --------------------------------------------------------------------

const worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' });
const send = (msg: ToWorker) => worker.postMessage(msg);

const siteBase = new URL('./', document.baseURI).href;
const hardwareThreads = Math.max(1, Math.min(16, navigator.hardwareConcurrency || 4));
send({
  type: 'init',
  wasmBase: `${siteBase}wasm/`,
  placesBase: `${siteBase}places/`,
  threaded: state.threaded,
  threads: hardwareThreads,
});

worker.onmessage = (event: MessageEvent<FromWorker>) => {
  const msg = event.data;
  switch (msg.type) {
    case 'ready':
      state.ready = true;
      state.threads = msg.threads;
      state.places = msg.places;
      onReady();
      break;
    case 'status':
      setStatus(msg.text, msg.busy);
      break;
    case 'error':
      setStatus(msg.message, false, true);
      break;
    case 'info':
      state.info = msg.info;
      showInfo(msg.info, msg.legend, msg.mode);
      break;
    case 'sun':
      showSun(msg.elevation, msg.azimuth);
      break;
    case 'frame':
      drawFrame(msg.width, msg.height, msg.pixels, msg.preview);
      state.lastFrame = { width: msg.width, height: msg.height, samples: msg.samples };
      if (!msg.preview) updateStats();
      break;
    case 'pick':
      onPick(msg.purpose, msg.pick, msg.moved);
      break;
  }
};

worker.onerror = (event) => setStatus(`The engine failed to start: ${event.message || 'unknown error'}`, false, true);

function onReady() {
  // The bundled places, between the fractal option and "Anywhere".
  const anywhere = ui.source.querySelector('option[value="anywhere"]');
  for (const place of state.places) {
    const option = document.createElement('option');
    option.value = `place:${place.name}`;
    option.textContent = place.label;
    ui.source.insertBefore(option, anywhere);
  }
  if (!state.threaded || state.threads < 4) ui.quality.value = '0.5';

  sendSize();
  sendOrbit();
  sendSun();
  send({ type: 'latitude', latitude: Number(ui.latitude.value) });
  send({ type: 'observerHeight', metres: eyeMetres() });
  send({ type: 'grid', cells: Number(ui.grid.value) });
  requestTerrain();
  updateStats();
}

// --- Drawing ------------------------------------------------------------------------------

function renderSize() {
  const rect = stage.getBoundingClientRect();
  const scale = Number(ui.quality.value) * Math.min(window.devicePixelRatio || 1, 1.5);
  let width = Math.max(16, Math.round(rect.width * scale));
  let height = Math.max(16, Math.round(rect.height * scale));
  const maxWidth = 1600;
  if (width > maxWidth) {
    height = Math.round((height * maxWidth) / width);
    width = maxWidth;
  }
  return { width, height };
}

function sendSize() {
  const { width, height } = renderSize();
  send({ type: 'size', width, height });
}

function drawFrame(width: number, height: number, buffer: ArrayBuffer, preview: boolean) {
  const image = new ImageData(new Uint8ClampedArray(buffer), width, height);
  if (preview) {
    // A small frame, scaled up to fill the canvas until the full-size passes arrive.
    if (canvas.width === 0 || canvas.height === 0) return;
    scratch.width = width;
    scratch.height = height;
    scratchCtx.putImageData(image, 0, 0);
    ctx.imageSmoothingEnabled = true;
    ctx.drawImage(scratch, 0, 0, canvas.width, canvas.height);
    return;
  }
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  ctx.putImageData(image, 0, 0);
}

let resizeTimer = 0;
new ResizeObserver(() => {
  clearTimeout(resizeTimer);
  resizeTimer = window.setTimeout(() => {
    const { width, height } = renderSize();
    if (canvas.width === 0) {
      canvas.width = width;
      canvas.height = height;
    }
    if (state.ready) sendSize();
  }, 120);
}).observe(stage);
{
  const { width, height } = renderSize();
  canvas.width = width;
  canvas.height = height;
}

// --- Status and read-outs -----------------------------------------------------------------

function setStatus(text: string, busy: boolean, error = false) {
  clearTimeout(state.statusTimer);
  ui.statusText.textContent = text;
  ui.spinner.classList.toggle('idle', !busy);
  ui.status.classList.toggle('error', error);
  if (error) state.statusTimer = window.setTimeout(() => ui.status.classList.remove('error'), 8000);
}

const fmt = (value: number) => Math.round(value).toLocaleString('en-US');
const ms = (seconds: number) => (seconds >= 1 ? `${seconds.toFixed(2)} s` : `${Math.round(seconds * 1000)} ms`);

function latLon(lat: number, lon: number) {
  return `${Math.abs(lat).toFixed(4)}°${lat >= 0 ? 'N' : 'S'} ${Math.abs(lon).toFixed(4)}°${lon >= 0 ? 'E' : 'W'}`;
}

function compass(azimuth: number) {
  const names = ['north', 'north-east', 'east', 'south-east', 'south', 'south-west', 'west', 'north-west'];
  return names[Math.round(azimuth / 45) % 8];
}

function showInfo(info: Info, legend: number[][], mode: Mode) {
  if (!info.hasTerrain) return;
  const range = `${fmt(info.minElevation)}–${fmt(info.maxElevation)} m`;
  if (info.real) {
    ui.placeLabel.textContent = info.label;
    ui.placeDetail.textContent = `${info.sizeKm.toFixed(0)} km square · ${range} · ${latLon(info.latitude, 0).split(' ')[0]}`;
  } else {
    ui.placeLabel.textContent = `Fractal range · seed ${ui.seed.value}`;
    ui.placeDetail.textContent = `${info.sizeKm.toFixed(0)} km square · ${range}`;
  }
  ui.attribution.hidden = !info.real;
  updateModeControls();

  // The legend, drawn from the engine's own colormap.
  if (mode === 'render' || !info.hasResult || legend.length === 0) {
    ui.legend.hidden = true;
  } else {
    ui.legend.hidden = false;
    const stops = legend.map(([r, g, b], k) => `rgb(${r},${g},${b}) ${((100 * k) / (legend.length - 1)).toFixed(1)}%`);
    ui.legendBar.style.background = `linear-gradient(to right, ${stops.join(', ')})`;
    if (mode === 'viewshed') {
      ui.legendTitle.textContent = 'Line of sight from the marker';
      ui.legendMin.textContent = 'Hidden';
      ui.legendMax.textContent = 'Visible';
      ui.legendNote.textContent = `${((info.mean ?? 0) * 100).toFixed(1)}% of the area is in view`;
    } else {
      const unit = mode === 'sun-hours' ? 'hours' : 'kWh/m²';
      ui.legendTitle.textContent = mode === 'sun-hours' ? 'Direct sun, hours per year' : 'Direct solar energy, kWh/m² per year';
      ui.legendMin.textContent = fmt(info.min ?? 0);
      ui.legendMax.textContent = fmt(info.max ?? 0);
      ui.legendNote.textContent = `Mean ${fmt(info.mean ?? 0)} ${unit} · latitude ${info.latitude.toFixed(1)}°`;
    }
  }
  updateStats();
}

function showSun(elevation: number, azimuth: number) {
  ui.sunOut.textContent =
    elevation > 0
      ? `Sun ${elevation.toFixed(0)}° above the horizon, in the ${compass(azimuth)}`
      : 'The sun is below the horizon';
}

function threadNote() {
  if (state.threaded) return '';
  return self.crossOriginIsolated ? ' (single-threaded build)' : ' (page not cross-origin isolated)';
}

function updateStats() {
  const info = state.info;
  const rows: [string, string][] = [
    ['Engine', `${state.threads} thread${state.threads === 1 ? '' : 's'}${threadNote()}`],
  ];
  if (info?.hasTerrain) {
    rows.push(['Triangles', fmt(info.triangles)]);
    rows.push(['Terrain + mesh', ms(info.terrainSeconds)]);
    rows.push(['BVH build', ms(info.bvhSeconds)]);
    if (info.gridSeconds > 0) rows.push(['Analysis grid', ms(info.gridSeconds)]);
    if (info.horizonRays > 0) rows.push(['Horizon map', `${ms(info.horizonSeconds)} · ${fmt(info.horizonRays)} rays`]);
    if (info.hasResult && state.mode !== 'render') {
      rows.push(
        state.mode === 'viewshed'
          ? ['Viewshed', `${ms(info.analysisSeconds)} · ${fmt(info.analysisRays)} rays`]
          : ['Sun lookups', `${ms(info.analysisSeconds)} · ${fmt(info.sunPositions)} sun positions`],
      );
    }
  }
  if (state.lastFrame.width > 0)
    rows.push(['Image', `${state.lastFrame.width}×${state.lastFrame.height} · ${state.lastFrame.samples} spp`]);

  ui.stats.replaceChildren(
    ...rows.flatMap(([term, value]) => {
      const dt = document.createElement('dt');
      dt.textContent = term;
      const dd = document.createElement('dd');
      dd.textContent = value;
      return [dt, dd];
    }),
  );
}

function onPick(purpose: 'hover' | 'click', pick: Pick, moved: boolean) {
  if (purpose === 'hover') {
    state.hover.inFlight = false;
    showReadout(pick);
    if (state.hover.pending) {
      const next = state.hover.pending;
      state.hover.pending = null;
      requestHover(next.fx, next.fy);
    }
  } else if (moved) {
    setStatus('Tracing sightlines…', true);
  }
}

function showReadout(pick: Pick) {
  if (!pick.onTerrain) {
    ui.readout.hidden = true;
    return;
  }
  const parts: string[] = [];
  if (!Number.isNaN(pick.value) && state.mode !== 'render') {
    if (state.mode === 'sun-hours') parts.push(`<strong>${fmt(pick.value)}</strong> sun-hours a year`);
    else if (state.mode === 'irradiance') parts.push(`<strong>${fmt(pick.value)}</strong> kWh/m² a year`);
    else parts.push(pick.value >= 0.5 ? '<strong>Visible</strong> from the marker' : '<strong>Hidden</strong> from the marker');
  }
  parts.push(`${fmt(pick.elevation)} m`);
  if (pick.geo) parts.push(latLon(pick.lat, pick.lon));
  ui.readout.innerHTML = parts.join(' · ');
  ui.readout.hidden = false;
}

// --- Camera interaction -------------------------------------------------------------------

function sendOrbit() {
  send({ type: 'orbit', ...state.orbit });
}

let orbitFrame = 0;
function queueOrbit() {
  if (orbitFrame) return;
  orbitFrame = requestAnimationFrame(() => {
    orbitFrame = 0;
    sendOrbit();
  });
}

function requestHover(fx: number, fy: number) {
  if (!state.ready) return;
  if (state.hover.inFlight) {
    state.hover.pending = { fx, fy };
    return;
  }
  state.hover.inFlight = true;
  send({ type: 'pick', purpose: 'hover', fx, fy });
}

const drag = { active: false, moved: false, x: 0, y: 0, azimuth: 0, elevation: 0, id: -1 };

// Two fingers on a touch screen pinch to zoom.
const touches = new Map<number, { x: number; y: number }>();
const pinch = { active: false, spread: 0, distance: 1 };
const spread = () => {
  const [a, b] = [...touches.values()];
  return Math.hypot(a.x - b.x, a.y - b.y);
};

canvas.addEventListener('pointerdown', (e) => {
  if (e.button !== 0) return;
  canvas.setPointerCapture(e.pointerId);
  touches.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (touches.size === 2) {
    drag.active = false; // A second finger turns the orbit into a pinch
    Object.assign(pinch, { active: true, spread: spread(), distance: state.orbit.distance });
    return;
  }
  Object.assign(drag, { active: true, moved: false, x: e.clientX, y: e.clientY, id: e.pointerId, ...state.orbit });
});

canvas.addEventListener('pointermove', (e) => {
  const rect = canvas.getBoundingClientRect();
  if (touches.has(e.pointerId)) touches.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (pinch.active && touches.size === 2) {
    state.orbit.distance = Math.min(3, Math.max(0.25, (pinch.distance * pinch.spread) / Math.max(spread(), 1)));
    queueOrbit();
    return;
  }
  if (drag.active && e.pointerId === drag.id) {
    const dx = e.clientX - drag.x;
    const dy = e.clientY - drag.y;
    if (!drag.moved && Math.hypot(dx, dy) < 4) return;
    drag.moved = true;
    canvas.classList.add('dragging');
    ui.readout.hidden = true;
    state.orbit.azimuth = (((drag.azimuth - dx * 0.35) % 360) + 360) % 360;
    state.orbit.elevation = Math.min(89, Math.max(3, drag.elevation + dy * 0.3));
    queueOrbit();
    fadeHint();
    return;
  }
  if (e.pointerType === 'mouse')
    requestHover((e.clientX - rect.left) / rect.width, (e.clientY - rect.top) / rect.height);
});

const endDrag = (e: PointerEvent) => {
  touches.delete(e.pointerId);
  if (touches.size < 2) pinch.active = false;
  if (!drag.active || e.pointerId !== drag.id) return;
  drag.active = false;
  canvas.classList.remove('dragging');
  if (!drag.moved && state.ready) {
    const rect = canvas.getBoundingClientRect();
    const fx = (e.clientX - rect.left) / rect.width;
    const fy = (e.clientY - rect.top) / rect.height;
    send({ type: 'pick', purpose: 'click', fx, fy });
    requestHover(fx, fy);
  }
};
canvas.addEventListener('pointerup', endDrag);
canvas.addEventListener('pointercancel', endDrag);
canvas.addEventListener('pointerleave', () => {
  if (!drag.active) ui.readout.hidden = true;
});

canvas.addEventListener(
  'wheel',
  (e) => {
    e.preventDefault();
    state.orbit.distance = Math.min(3, Math.max(0.25, state.orbit.distance * Math.exp(e.deltaY * 0.0012)));
    queueOrbit();
    fadeHint();
  },
  { passive: false },
);

// Keyboard orbit, for anyone not using a pointer.
canvas.tabIndex = 0;
canvas.addEventListener('keydown', (e) => {
  const step = e.shiftKey ? 15 : 5;
  if (e.key === 'ArrowLeft') state.orbit.azimuth = (state.orbit.azimuth + step) % 360;
  else if (e.key === 'ArrowRight') state.orbit.azimuth = (state.orbit.azimuth - step + 360) % 360;
  else if (e.key === 'ArrowUp') state.orbit.elevation = Math.min(89, state.orbit.elevation + step);
  else if (e.key === 'ArrowDown') state.orbit.elevation = Math.max(3, state.orbit.elevation - step);
  else if (e.key === '+' || e.key === '=') state.orbit.distance = Math.max(0.25, state.orbit.distance / 1.1);
  else if (e.key === '-') state.orbit.distance = Math.min(3, state.orbit.distance * 1.1);
  else return;
  e.preventDefault();
  queueOrbit();
});

let hintTimer = window.setTimeout(fadeHint, 9000);
function fadeHint() {
  clearTimeout(hintTimer);
  ui.hint.classList.add('faded');
}

// --- Terrain controls ---------------------------------------------------------------------

function currentTerrain(): TerrainRequest | null {
  const resolution = Number(ui.resolution.value);
  const source = ui.source.value;
  if (source === 'fractal') {
    return {
      kind: 'fractal',
      seed: Math.max(0, Math.floor(Number(ui.seed.value) || 0)),
      ridge: Number(ui.ridge.value),
      warp: Number(ui.warp.value),
      height: Number(ui.height.value),
      resolution,
    };
  }
  if (source.startsWith('place:')) {
    const name = source.slice('place:'.length);
    const place = state.places.find((p) => p.name === name);
    return { kind: 'place', name, label: place?.label ?? name, resolution };
  }
  const lat = Number(ui.lat.value);
  const lon = Number(ui.lon.value);
  if (!Number.isFinite(lat) || !Number.isFinite(lon) || Math.abs(lat) > 80 || Math.abs(lon) > 180) {
    setStatus('Enter a latitude between -80 and 80 and a longitude between -180 and 180', false, true);
    return null;
  }
  return {
    kind: 'region',
    lat,
    lon,
    extentKm: Number(ui.extent.value),
    label: state.regionLabel || latLon(lat, lon),
    resolution,
  };
}

function requestTerrain() {
  if (!state.ready) return;
  const terrain = currentTerrain();
  if (!terrain) return;
  ui.fractal.hidden = terrain.kind !== 'fractal';
  ui.anywhere.hidden = ui.source.value !== 'anywhere';
  state.orbit = { ...DEFAULT_ORBIT };
  sendOrbit();
  send({ type: 'terrain', terrain });
}

ui.source.addEventListener('change', requestTerrain);
ui.resolution.addEventListener('change', requestTerrain);
ui.seed.addEventListener('change', requestTerrain);
ui.reseed.addEventListener('click', () => {
  ui.seed.value = String(Math.floor(Math.random() * 100000));
  requestTerrain();
});

for (const [input, digits] of [[ui.ridge, 2], [ui.warp, 2], [ui.height, 1]] as const) {
  const output = $(`${input.id}-out`);
  const show = () => (output.textContent = Number(input.value).toFixed(digits));
  show();
  input.addEventListener('input', show);
  input.addEventListener('change', requestTerrain); // On release: a new mesh is a big job
}

const extentOut = $('extent-out');
const showExtent = () => (extentOut.textContent = `${ui.extent.value} km square`);
showExtent();
ui.extent.addEventListener('input', showExtent);
for (const input of [ui.lat, ui.lon, ui.extent]) input.addEventListener('input', () => (state.regionLabel = ''));
ui.loadRegion.addEventListener('click', requestTerrain);

for (const preset of REGION_PRESETS) {
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'btn';
  button.textContent = preset.label;
  button.addEventListener('click', () => {
    ui.lat.value = String(preset.lat);
    ui.lon.value = String(preset.lon);
    ui.extent.value = String(preset.extentKm);
    showExtent();
    state.regionLabel = preset.label;
    requestTerrain();
  });
  ui.presets.append(button);
}

// --- Mode and sun controls ----------------------------------------------------------------

function setMode(mode: Mode) {
  state.mode = mode;
  for (const button of ui.modes.querySelectorAll<HTMLButtonElement>('button')) {
    button.setAttribute('aria-checked', String(button.dataset.mode === mode));
    button.tabIndex = button.dataset.mode === mode ? 0 : -1;
  }
  updateModeControls();
  ui.hint.textContent =
    mode === 'viewshed' ? 'Click the terrain to move the observer · drag to orbit' : 'Drag to orbit · scroll to zoom';
  ui.hint.classList.remove('faded');
  clearTimeout(hintTimer);
  hintTimer = window.setTimeout(fadeHint, 7000);
  ui.readout.hidden = true;
  if (mode !== 'render') stopPlaying();
  send({ type: 'mode', mode });
}

function updateModeControls() {
  const mode = state.mode;
  const real = state.info?.real ?? false;
  ui.modeNote.textContent = MODE_NOTES[mode];
  ui.sunControls.hidden = mode !== 'render';
  ui.latitudeField.hidden = real || mode === 'viewshed';
  ui.viewshed.hidden = mode !== 'viewshed';
  ui.gridField.hidden = mode === 'render';
  stage.classList.toggle('pick-mode', mode === 'viewshed');
}

ui.modes.addEventListener('click', (e) => {
  const button = (e.target as HTMLElement).closest('button');
  if (button?.dataset.mode) setMode(button.dataset.mode as Mode);
});
ui.modes.addEventListener('keydown', (e) => {
  const order: Mode[] = ['render', 'sun-hours', 'irradiance', 'viewshed'];
  const k = order.indexOf(state.mode);
  let next = -1;
  if (e.key === 'ArrowRight' || e.key === 'ArrowDown') next = (k + 1) % order.length;
  if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') next = (k + order.length - 1) % order.length;
  if (next < 0) return;
  e.preventDefault();
  setMode(order[next]);
  ui.modes.querySelector<HTMLButtonElement>(`[data-mode="${order[next]}"]`)?.focus();
});

const dayOut = $('day-out');
const hourOut = $('hour-out');
function showTime() {
  const date = new Date(Date.UTC(2025, 0, Number(ui.day.value)));
  dayOut.textContent = date.toLocaleDateString('en-GB', { day: 'numeric', month: 'short', timeZone: 'UTC' });
  const hour = Number(ui.hour.value);
  const minutes = Math.round(hour * 60) % (24 * 60);
  hourOut.textContent = `${String(Math.floor(minutes / 60)).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')} solar time`;
}
showTime();

let sunFrame = 0;
function sendSun() {
  if (sunFrame) return;
  sunFrame = requestAnimationFrame(() => {
    sunFrame = 0;
    send({ type: 'sun', day: Number(ui.day.value), hour: Number(ui.hour.value) });
  });
}
for (const input of [ui.day, ui.hour])
  input.addEventListener('input', () => {
    showTime();
    sendSun();
  });

function stopPlaying() {
  if (!state.playing) return;
  clearInterval(state.playing);
  state.playing = 0;
  ui.play.textContent = '▶';
  ui.play.setAttribute('aria-label', 'Play the day');
}
ui.play.addEventListener('click', () => {
  if (state.playing) return stopPlaying();
  ui.play.textContent = '❚❚';
  ui.play.setAttribute('aria-label', 'Pause');
  state.playing = window.setInterval(() => {
    ui.hour.value = String((Number(ui.hour.value) + 0.1) % 24);
    showTime();
    sendSun();
  }, 90);
});

const latitudeOut = $('latitude-out');
const showLatitude = () => {
  const lat = Number(ui.latitude.value);
  latitudeOut.textContent = `${Math.abs(lat).toFixed(1)}° ${lat >= 0 ? 'N' : 'S'}`;
};
showLatitude();
ui.latitude.addEventListener('input', () => {
  showLatitude();
  send({ type: 'latitude', latitude: Number(ui.latitude.value) });
});

// Observer height on a log scale: 2 m (standing) to 500 m (a mast on a hill).
const eyeMetres = () => Math.round(2 * Math.pow(250, Number(ui.eye.value) / 100));
const eyeOut = $('eye-out');
const showEye = () => (eyeOut.textContent = `${eyeMetres()} m`);
showEye();
ui.eye.addEventListener('input', showEye);
ui.eye.addEventListener('change', () => send({ type: 'observerHeight', metres: eyeMetres() }));

ui.grid.addEventListener('change', () => send({ type: 'grid', cells: Number(ui.grid.value) }));

// --- View controls ------------------------------------------------------------------------

ui.quality.addEventListener('change', sendSize);
ui.resetView.addEventListener('click', () => {
  state.orbit = { ...DEFAULT_ORBIT };
  sendOrbit();
});
ui.save.addEventListener('click', () => {
  canvas.toBlob((blob) => {
    if (!blob) return;
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = `terrain-${state.mode}.png`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  }, 'image/png');
});

setMode('render');
updateStats();

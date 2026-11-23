/// <reference lib="webworker" />
// The engine worker: loads the WebAssembly build of the C++ engine and runs it off the page's
// main thread. The page sends settings; this sends back frames, status and results.
//
// Rendering is progressive. After any change, a quick low-resolution frame goes out first, so
// dragging the camera or the sun feels immediate; then full-resolution passes keep adding
// samples, and each one is shown, so the noise visibly melts away. Messages are handled
// between passes, so a new setting interrupts the refinement within one pass.

import type { FromWorker, Info, Mode, Pick, PlaceInfo, TerrainRequest, ToWorker } from './protocol';
import { MODE_INDEX } from './protocol';

// The parts of the embind module this uses (see apps/web/native/bindings.cc).
interface Studio {
  generate(seed: number, ridge: number, warp: number, height: number, resolution: number): void;
  loadPlace(name: string, png: Uint8Array, resolution: number): boolean;
  beginRegion(lat: number, lon: number, extentKm: number, resolution: number): { z: number; x: number; y: number }[];
  addTile(x: number, y: number, png: Uint8Array): boolean;
  finishRegion(label: string): boolean;
  error(): string;
  setSize(width: number, height: number): void;
  setOrbit(azimuth: number, elevation: number, distance: number): void;
  setSunTime(day: number, hour: number): { elevation: number; azimuth: number };
  sun(): { elevation: number; azimuth: number };
  setMode(mode: number): void;
  setLatitude(degrees: number): void;
  setGridSize(cells: number): void;
  setObserver(u: number, v: number): boolean;
  setObserverHeight(metres: number): void;
  analysisPending(): boolean;
  runAnalysis(): void;
  renderPass(samples: number): void;
  width(): number;
  height(): number;
  samples(): number;
  pixels(): Uint8Array;
  pick(fx: number, fy: number): Pick;
  info(): Info;
  legend(stops: number): number[][];
}

interface StudioModule {
  Studio: { new (threads: number): Studio; places(): PlaceInfo[] };
}

const TILE_URL = 'https://s3.amazonaws.com/elevation-tiles-prod/terrarium';
const MAX_TILES = 64;          // A guard against an enormous region
const PREVIEW_DIVISOR = 4;     // The first frame after a change is a quarter of the size
const MAX_SAMPLES = 256;       // Stop refining here and let the CPU rest
const PASS_TARGET_SECONDS = 0.12; // Aim for passes this long, so input is answered quickly

const scope = self as unknown as DedicatedWorkerGlobalScope;
const post = (msg: FromWorker, transfer: Transferable[] = []) => scope.postMessage(msg, transfer);

let studio: Studio | null = null;
let placesBase = '';
let mode: Mode = 'render';
let size = { width: 640, height: 360 };
let hasTerrain = false;
let loadSeq = 0;        // Every terrain request gets a number; only the latest one may finish
let loading = false;
let previewDue = true;  // The next frame should be the quick low-resolution one
let samplesPerPass = 1;
let ticking = false;
let idleReported = false;

scope.onmessage = (event: MessageEvent<ToWorker>) => {
  const msg = event.data;
  if (msg.type === 'init') {
    init(msg.wasmBase, msg.threaded, msg.threads).catch(fail);
    placesBase = msg.placesBase;
    return;
  }
  if (msg.type === 'terrain') {
    loadTerrain(msg.terrain);
    return;
  }
  if (!studio) return;

  try {
    switch (msg.type) {
      case 'size':
        size = { width: msg.width, height: msg.height };
        restart();
        break;
      case 'orbit':
        studio.setOrbit(msg.azimuth, msg.elevation, msg.distance);
        restart();
        break;
      case 'sun':
        post({ type: 'sun', ...studio.setSunTime(msg.day, msg.hour) });
        if (mode === 'render') restart();
        break;
      case 'mode':
        mode = msg.mode;
        studio.setMode(MODE_INDEX[mode]);
        sendInfo();
        restart();
        break;
      case 'latitude':
        studio.setLatitude(msg.latitude);
        post({ type: 'sun', ...studio.sun() });
        restart();
        break;
      case 'grid':
        studio.setGridSize(msg.cells);
        restart();
        break;
      case 'observerHeight':
        studio.setObserverHeight(msg.metres);
        restart();
        break;
      case 'pick':
        handlePick(msg.purpose, msg.fx, msg.fy);
        break;
    }
  } catch (err) {
    fail(err);
  }
  schedule();
};

async function init(wasmBase: string, threaded: boolean, threads: number) {
  post({ type: 'status', text: 'Loading the engine…', busy: true });
  const url = `${wasmBase}studio-${threaded ? 'mt' : 'st'}.js`;
  const { default: createStudio } = (await import(/* @vite-ignore */ url)) as {
    default: () => Promise<StudioModule>;
  };
  const module = await createStudio();
  studio = new module.Studio(threaded ? threads : 1);
  post({ type: 'ready', threads: threaded ? threads : 1, places: module.Studio.places() });
}

function fail(err: unknown) {
  const message = err instanceof Error ? err.message : String(err);
  post({ type: 'error', message });
}

function restart() {
  previewDue = true;
  samplesPerPass = 1;
  idleReported = false;
}

function schedule() {
  if (ticking) return;
  ticking = true;
  setTimeout(tick, 0);
}

function sendInfo() {
  if (!studio) return;
  const info = studio.info();
  post({ type: 'info', info, legend: info.hasResult ? studio.legend(32) : [], mode });
}

function postFrame(preview: boolean) {
  if (!studio) return;
  // The view points into WebAssembly memory, so copy it before handing it to the page.
  const pixels = new Uint8ClampedArray(studio.pixels());
  post(
    { type: 'frame', width: studio.width(), height: studio.height(), samples: studio.samples(), preview, pixels: pixels.buffer },
    [pixels.buffer],
  );
}

function analysisText(): string {
  if (mode === 'viewshed') return 'Tracing sightlines…';
  return 'Measuring horizons for a year of sun…';
}

// One step of work: an analysis if one is due, otherwise one render pass.
function tick() {
  ticking = false;
  if (!studio || loading || !hasTerrain) return;

  try {
    if (studio.analysisPending()) {
      post({ type: 'status', text: analysisText(), busy: true });
      studio.runAnalysis();
      sendInfo();
      restart();
    }

    if (previewDue) {
      studio.setSize(Math.max(1, Math.round(size.width / PREVIEW_DIVISOR)),
                     Math.max(1, Math.round(size.height / PREVIEW_DIVISOR)));
      studio.renderPass(1);
      postFrame(true);
      studio.setSize(size.width, size.height);
      previewDue = false;
      schedule();
      return;
    }

    if (studio.samples() >= MAX_SAMPLES) {
      if (!idleReported) {
        post({ type: 'status', text: `Done · ${studio.samples()} samples per pixel`, busy: false });
        idleReported = true;
      }
      return;
    }

    const start = performance.now();
    studio.renderPass(samplesPerPass);
    const seconds = (performance.now() - start) / 1000;

    // Size the next pass so it takes about PASS_TARGET_SECONDS: more samples per pass is
    // more efficient, fewer keeps the page responsive.
    const perSample = seconds / samplesPerPass;
    samplesPerPass = Math.max(1, Math.min(16, Math.floor(PASS_TARGET_SECONDS / Math.max(perSample, 1e-4))));
    samplesPerPass = Math.min(samplesPerPass, MAX_SAMPLES - studio.samples()) || 1;

    postFrame(false);
    post({ type: 'status', text: `Refining · ${studio.samples()} samples per pixel`, busy: true });
    schedule();
  } catch (err) {
    fail(err);
  }
}

function handlePick(purpose: 'hover' | 'click', fx: number, fy: number) {
  if (!studio || !hasTerrain || loading) return;
  const pick = studio.pick(fx, fy);
  let moved = false;
  if (purpose === 'click' && mode === 'viewshed' && pick.onTerrain) {
    moved = studio.setObserver(pick.u, pick.v);
    if (moved) restart();
  }
  post({ type: 'pick', purpose, pick, moved });
}

async function fetchBytes(url: string): Promise<Uint8Array> {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`could not download ${url} (${response.status})`);
  return new Uint8Array(await response.arrayBuffer());
}

async function loadTerrain(request: TerrainRequest) {
  if (!studio) return;
  const id = ++loadSeq;
  const current = () => id === loadSeq;
  loading = true;

  try {
    if (request.kind === 'fractal') {
      post({ type: 'status', text: 'Generating terrain…', busy: true });
      // Let the status message reach the page before the long synchronous call.
      await new Promise((resolve) => setTimeout(resolve, 0));
      if (!current()) return;
      studio.generate(request.seed, request.ridge, request.warp, request.height, request.resolution);
    } else if (request.kind === 'place') {
      post({ type: 'status', text: `Loading ${request.label}…`, busy: true });
      const png = await fetchBytes(`${placesBase}${request.name}.png`);
      if (!current()) return;
      post({ type: 'status', text: 'Building the mesh…', busy: true });
      if (!studio.loadPlace(request.name, png, request.resolution)) throw new Error(studio.error());
    } else {
      const tiles = studio.beginRegion(request.lat, request.lon, request.extentKm, request.resolution);
      if (tiles.length > MAX_TILES)
        throw new Error(`That area needs ${tiles.length} elevation tiles; make it smaller or lower the detail.`);

      // A few downloads at a time; each tile is about 100 KB, and the browser caches them.
      let done = 0;
      let next = 0;
      const worker = async () => {
        while (next < tiles.length && current()) {
          const t = tiles[next++];
          const png = await fetchBytes(`${TILE_URL}/${t.z}/${t.x}/${t.y}.png`);
          if (!current()) return;
          if (!studio!.addTile(t.x, t.y, png)) throw new Error(studio!.error());
          post({ type: 'status', text: `Downloading elevation · ${++done}/${tiles.length} tiles`, busy: true });
        }
      };
      await Promise.all(Array.from({ length: Math.min(6, tiles.length) }, worker));
      if (!current()) return;
      post({ type: 'status', text: 'Building the mesh…', busy: true });
      await new Promise((resolve) => setTimeout(resolve, 0));
      if (!current()) return;
      if (!studio.finishRegion(request.label)) throw new Error(studio.error());
    }

    hasTerrain = true;
    sendInfo();
    post({ type: 'sun', ...studio.sun() });
    restart();
  } catch (err) {
    if (current()) fail(err);
  } finally {
    if (current()) {
      loading = false;
      schedule();
    }
  }
}

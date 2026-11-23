// Messages between the page (main.ts) and the engine worker (worker.ts).
//
// The engine runs in a Web Worker so that a long computation (building a mesh, measuring
// horizons, a render pass) never freezes the page. The two sides only talk through these
// messages.

export type Mode = 'render' | 'sun-hours' | 'irradiance' | 'viewshed';
export const MODE_INDEX: Record<Mode, number> = { render: 0, 'sun-hours': 1, irradiance: 2, viewshed: 3 };

export type TerrainRequest =
  | { kind: 'fractal'; seed: number; ridge: number; warp: number; height: number; resolution: number }
  | { kind: 'place'; name: string; label: string; resolution: number }
  | { kind: 'region'; lat: number; lon: number; extentKm: number; label: string; resolution: number };

export interface PlaceInfo {
  name: string;
  label: string;
  lat: number;
  lon: number;
  extentKm: number;
}

// Page -> worker
export type ToWorker =
  | { type: 'init'; wasmBase: string; placesBase: string; threaded: boolean; threads: number }
  | { type: 'terrain'; terrain: TerrainRequest }
  | { type: 'size'; width: number; height: number }
  | { type: 'orbit'; azimuth: number; elevation: number; distance: number }
  | { type: 'sun'; day: number; hour: number }
  | { type: 'mode'; mode: Mode }
  | { type: 'latitude'; latitude: number }
  | { type: 'grid'; cells: number }
  | { type: 'observerHeight'; metres: number }
  | { type: 'pick'; purpose: 'hover' | 'click'; fx: number; fy: number };

// What the engine reports about the terrain, the analysis and its timings.
export interface Info {
  threads: number;
  hasTerrain: boolean;
  real: boolean;
  label: string;
  sizeKm: number;
  minElevation: number;
  maxElevation: number;
  latitude: number;
  triangles: number;
  terrainSeconds: number;
  bvhSeconds: number;
  gridSeconds: number;
  horizonSeconds: number;
  horizonRays: number;
  analysisSeconds: number;
  analysisRays: number;
  sunPositions: number;
  passSeconds: number;
  hasResult: boolean;
  min?: number;
  max?: number;
  mean?: number;
  observerU?: number;
  observerV?: number;
}

export interface Pick {
  hit: boolean;
  onTerrain: boolean;
  u: number;
  v: number;
  elevation: number;
  geo: boolean;
  lat: number;
  lon: number;
  value: number; // NaN without an analysis
}

// Worker -> page
export type FromWorker =
  | { type: 'ready'; threads: number; places: PlaceInfo[] }
  | { type: 'status'; text: string; busy: boolean }
  | { type: 'error'; message: string }
  | { type: 'info'; info: Info; legend: number[][]; mode: Mode }
  | { type: 'sun'; elevation: number; azimuth: number }
  | { type: 'frame'; width: number; height: number; samples: number; preview: boolean; pixels: ArrayBuffer }
  | { type: 'pick'; purpose: 'hover' | 'click'; pick: Pick; moved: boolean };

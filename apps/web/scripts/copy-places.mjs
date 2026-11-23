// Copies the bundled elevation data (data/places/*.png) into public/places/, so the dev server
// serves it and the production build ships it. The repository keeps a single copy.
import { copyFileSync, mkdirSync, readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const source = join(here, '..', '..', '..', 'data', 'places');
const target = join(here, '..', 'public', 'places');

mkdirSync(target, { recursive: true });
const files = readdirSync(source).filter((name) => name.endsWith('.png'));
for (const name of files) copyFileSync(join(source, name), join(target, name));
console.log(`Copied ${files.length} places into public/places/`);

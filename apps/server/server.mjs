import { createServer } from 'node:http';
import { createReadStream, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, statSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { timingSafeEqual } from 'node:crypto';

const HERE = dirname(fileURLToPath(import.meta.url));
const DEFAULT_ASSETS = resolve(HERE, '../assets/birds');
const DISPLAY_HTML = readFileSync(join(HERE, 'display.html'));
const REQUIRED_FIELDS = ['title', 'artist', 'license', 'source_url'];

export function loadCatalog(assetDirectory) {
  const metadataPath = join(assetDirectory, 'metadata.json');
  const document = JSON.parse(readFileSync(metadataPath, 'utf8'));
  if (!Array.isArray(document.images)) throw new Error(`${metadataPath}: images must be an array`);

  // Discover JPEGs first: stale or malformed metadata must never bring a
  // deliberately curated-away image back into the catalog.
  const images = readdirSync(assetDirectory)
    .filter((file) => /\.jpe?g$/i.test(file))
    .sort();
  const imageSet = new Set(images);
  const metadata = new Map();
  for (const [index, entry] of document.images.entries()) {
    if (!entry || typeof entry.file !== 'string') {
      console.warn(`Rejected metadata row ${index + 1}: missing filename`);
      continue;
    }
    if (!imageSet.has(entry.file)) {
      console.warn(`Rejected stale metadata: ${entry.file} (JPEG is absent)`);
      continue;
    }
    if (metadata.has(entry.file)) throw new Error(`Duplicate metadata for ${entry.file}`);
    metadata.set(entry.file, entry);
  }

  const catalog = images.map((file) => {
    const entry = metadata.get(file);
    if (!entry) throw new Error(`Existing JPEG lacks metadata: ${file}`);
    const missing = REQUIRED_FIELDS.filter((field) => typeof entry[field] !== 'string' || !entry[field].trim());
    if (missing.length) throw new Error(`${file} lacks attribution fields: ${missing.join(', ')}`);
    let source;
    try { source = new URL(entry.source_url); } catch { throw new Error(`${file} has invalid source_url`); }
    if (!['http:', 'https:'].includes(source.protocol)) throw new Error(`${file} has unsafe source_url`);
    return Object.freeze({
      file,
      name: entry.title,
      scientific_name: '',
      image_url: `/birds/${encodeURIComponent(file)}`,
      credit: entry.artist,
      license: entry.license,
      source_url: source.href,
    });
  });
  if (!catalog.length) throw new Error('Bird catalog is empty');

  return catalog;
}

function sendJson(response, status, value) {
  const body = JSON.stringify(value);
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
  });
  response.end(body);
}

function authenticated(request, expectedToken) {
  const authorization = request.headers.authorization ?? '';
  const supplied = authorization.startsWith('Bearer ') ? authorization.slice(7) : request.headers['x-bird-token'];
  if (typeof supplied !== 'string') return false;
  const actual = Buffer.from(supplied);
  const expected = Buffer.from(expectedToken);
  return actual.length === expected.length && timingSafeEqual(actual, expected);
}

function initialState(catalog, statePath) {
  if (statePath && existsSync(statePath)) {
    try {
      const stored = JSON.parse(readFileSync(statePath, 'utf8'));
      const index = catalog.findIndex((bird) => bird.file === stored.file);
      if (index >= 0 && Number.isSafeInteger(stored.version) && stored.version > 0) {
        return { index, version: stored.version };
      }
      console.warn('Rejected saved state: bird no longer exists or version is invalid');
    } catch (error) {
      console.warn(`Rejected saved state: ${error.message}`);
    }
  }
  return { index: Math.floor(Math.random() * catalog.length), version: 1 };
}

function persistState(statePath, state, catalog) {
  if (!statePath) return;
  mkdirSync(dirname(statePath), { recursive: true });
  const temporary = `${statePath}.tmp`;
  writeFileSync(temporary, `${JSON.stringify({ file: catalog[state.index].file, version: state.version })}\n`, { mode: 0o600 });
  renameSync(temporary, statePath);
}

export function createBirdServer({ assetDirectory = DEFAULT_ASSETS, token, statePath, random = Math.random } = {}) {
  if (!token) throw new Error('BIRD_REMOTE_TOKEN is required');
  const catalog = loadCatalog(assetDirectory);
  const state = initialState(catalog, statePath);
  const clients = new Set();
  const snapshot = () => ({ ...catalog[state.index], version: state.version });
  const publish = () => {
    const message = `event: bird\ndata: ${JSON.stringify(snapshot())}\n\n`;
    for (const client of clients) client.write(message);
  };

  const server = createServer((request, response) => {
    const url = new URL(request.url, 'http://bird-remote.local');
    if (request.method === 'GET' && url.pathname === '/health') {
      return sendJson(response, 200, { status: 'ok' });
    }
    if (request.method === 'GET' && url.pathname === '/') {
      response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': DISPLAY_HTML.length, 'Cache-Control': 'no-store' });
      return response.end(DISPLAY_HTML);
    }
    if (request.method === 'GET' && url.pathname === '/api/current') return sendJson(response, 200, snapshot());
    if (request.method === 'GET' && url.pathname === '/api/events') {
      response.writeHead(200, {
        'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive',
        'X-Accel-Buffering': 'no',
      });
      response.write(`retry: 1000\nevent: bird\ndata: ${JSON.stringify(snapshot())}\n\n`);
      clients.add(response);
      request.on('close', () => clients.delete(response));
      return;
    }
    if (request.method === 'POST' && url.pathname === '/api/next') {
      if (!authenticated(request, token)) return sendJson(response, 401, { error: 'unauthorized' });
      if (catalog.length > 1) {
        let next = Math.floor(random() * (catalog.length - 1));
        if (next >= state.index) next += 1;
        state.index = next;
      }
      state.version += 1;
      try { persistState(statePath, state, catalog); } catch (error) {
        console.error(`Could not persist state: ${error.message}`);
      }
      publish();
      return sendJson(response, 200, snapshot());
    }
    if (request.method === 'GET' && url.pathname.startsWith('/birds/')) {
      let filename;
      try { filename = decodeURIComponent(url.pathname.slice('/birds/'.length)); } catch { filename = ''; }
      if (!catalog.some((bird) => bird.file === filename) || basename(filename) !== filename) {
        return sendJson(response, 404, { error: 'not found' });
      }
      const path = join(assetDirectory, filename);
      const size = statSync(path).size;
      response.writeHead(200, { 'Content-Type': 'image/jpeg', 'Content-Length': size, 'Cache-Control': 'public, max-age=31536000, immutable' });
      return createReadStream(path).pipe(response);
    }
    return sendJson(response, 404, { error: 'not found' });
  });
  server.on('close', () => { for (const client of clients) client.end(); clients.clear(); });
  return { server, catalog, snapshot };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const host = process.env.BIRD_REMOTE_HOST || '0.0.0.0';
  const port = Number(process.env.BIRD_REMOTE_PORT || 8080);
  const assetDirectory = resolve(process.env.BIRD_ASSET_DIR || DEFAULT_ASSETS);
  const statePath = resolve(process.env.BIRD_STATE_PATH || join(process.env.HOME || HERE, '.local/state/bird-remote/state.json'));
  const app = createBirdServer({ assetDirectory, token: process.env.BIRD_REMOTE_TOKEN, statePath });
  app.server.listen(port, host, () => {
    console.log(`Bird server listening on http://${host}:${port}; catalog=${app.catalog.length}; state=${statePath}`);
  });
}

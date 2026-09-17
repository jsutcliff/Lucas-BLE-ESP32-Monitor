// Load-time smoke test for web/dashboard.html.
//
// `node --check` only validates syntax. It cannot catch a temporal-dead-zone
// ReferenceError from declaration order, which throws at load and leaves the
// whole dashboard blank. This evaluates the page's script against a minimal DOM
// stub, so that class of bug fails the build instead of the board.
const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync(process.argv[2] || 'web/dashboard.html', 'utf8');
const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.error('smoke: no <script> block found'); process.exit(1); }

const ids = new Set([...html.matchAll(/id="([^"]+)"/g)].map(x => x[1]));

const makeEl = (id) => {
  const el = {
    id,
    hidden: false,
    textContent: '',
    innerHTML: '',
    value: '',
    disabled: false,
    dataset: {},
    style: {},
    clientWidth: 600,
    clientHeight: 200,
    files: [{}],
    offsetWidth: 80,
    setAttribute() {}, getAttribute() { return null; },
    appendChild() {}, querySelectorAll() { return []; },
    addEventListener() {},
    getBoundingClientRect() { return { left: 0, top: 0, width: 600, height: 200 }; },
  };
  return el;
};

const els = new Map([...ids].map(id => [id, makeEl(id)]));
const missing = [];

const document = {
  body: makeEl('body'),
  documentElement: {
    _attrs: {},
    setAttribute(k, v) { this._attrs[k] = v; },
    getAttribute(k) { return this._attrs[k] || null; },
    style: { getPropertyValue: () => '#000000' },
  },
  getElementById(id) {
    if (!els.has(id)) { missing.push(id); els.set(id, makeEl(id)); }
    return els.get(id);
  },
  querySelectorAll() { return []; },
  createElement: () => makeEl('created'),
};

const sandbox = {
  document,
  console,
  Math, JSON, Date, Number, String, Object, Array, Promise, isFinite, parseInt, parseFloat,
  localStorage: { _d: {}, getItem(k) { return this._d[k] ?? null; }, setItem(k, v) { this._d[k] = v; } },
  matchMedia: () => ({ matches: false }),
  getComputedStyle: () => ({ getPropertyValue: () => '#000000' }),
  addEventListener() {},
  setInterval() { return 0; }, clearInterval() {}, setTimeout() { return 0; },
  fetch: () => Promise.reject(new Error('offline in smoke test')),
  FormData: class { append() {} },
  confirm: () => false,
  encodeURIComponent,
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

try {
  vm.createContext(sandbox);
  vm.runInContext(m[1], sandbox, { filename: 'dashboard.js', timeout: 5000 });
} catch (err) {
  console.error('smoke: dashboard script threw at load:\n  ' + err.message);
  process.exit(1);
}

// Every element the script reaches for must actually exist in the markup.
if (missing.length) {
  console.error('smoke: script looked up ids that are not in the HTML: ' +
                [...new Set(missing)].join(', '));
  process.exit(1);
}
console.log('smoke: dashboard script loads cleanly');

// Reads the Doodle Champion Island Games (the web version) and writes what the
// port needs as JSON: every symbol of its 15 Adobe Animate libraries, baked
// frame by frame, and its dialogue trees.
//
//   node tools/extract.js SRC OUT
//
// SRC is a copy of the doodle (github.com/potherca-blog/Google-Doodle-Champion-Island).
// It runs the doodle in headless Chromium through Playwright, starts the game
// so that every library is loaded, then instantiates each symbol.
const http = require('http');
const fs = require('fs');
const path = require('path');
let chromium;
try { ({ chromium } = require('playwright')); } catch (e) { ({ chromium } = require('/opt/node22/lib/node_modules/playwright')); }

const SRC = path.resolve(process.argv[2] || '../../../potherca-blog/google-doodle-champion-island');
const OUT = path.resolve(process.argv[3] || 'build/ex');
fs.mkdirSync(OUT, { recursive: true });

const TYPES = { '.html': 'text/html', '.js': 'application/javascript', '.json': 'application/json', '.png': 'image/png', '.mp3': 'audio/mpeg', '.mp4': 'video/mp4', '.ttf': 'font/ttf' };
const server = http.createServer((req, res) => {
  const f = path.join(SRC, decodeURIComponent(req.url.split('?')[0]));
  if (!f.startsWith(SRC) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream' });
  fs.createReadStream(f).pipe(res);
});

// The dialogue trees are one array literal in kitsune20.js ("var Ok = [...]").
function dialogues() {
  const s = fs.readFileSync(path.join(SRC, 'kitsune20.js'), 'utf8');
  const i = s.indexOf('var Ok = [') + 'var Ok = '.length;
  let depth = 0, k = i, q = null;
  for (; ; k++) {
    const ch = s[k];
    if (q) { if (ch === '\\') k++; else if (ch === q) q = null; }
    else if (ch === '"' || ch === "'") q = ch;
    else if (ch === '[' || ch === '{') depth++;
    else if (ch === ']' || ch === '}') { if (--depth === 0) break; }
  }
  return Function('return ' + s.slice(i, k + 1))();
}

// Runs in the page: bakes one library.
function bake(id) {
  const o = Sk.Kf[id], lib = o.getLibrary(), cj = createjs;
  const symName = new Map();
  for (const k of Object.keys(lib)) { const v = lib[k]; if (typeof v === 'function' && v.prototype instanceof cj.DisplayObject) symName.set(v, k); }
  const protoName = new Map(); for (const [v, k] of symName) protoName.set(v.prototype, k);
  const r2 = x => Math.round(x * 1000) / 1000;
  const G = cj.Graphics;
  const kinds = [[G.MoveTo, 'M'], [G.LineTo, 'L'], [G.QuadraticCurveTo, 'Q'], [G.BezierCurveTo, 'B'], [G.ClosePath, 'Z'], [G.Rect, 'R'],
    [G.Fill, 'F'], [G.Stroke, 'S'], [G.BeginPath, 'P'], [G.Circle, 'C'], [G.StrokeStyle, 'SS'], [G.RoundRect, 'RR'], [G.Ellipse, 'E'], [G.Arc, 'A']];
  function gfx(g) {
    try { g._updateInstructions && g._updateInstructions(); } catch (e) {}
    return (g._instructions || []).map(i => {
      const d = {};
      for (const k of Object.keys(i)) { const v = i[k]; if (typeof v === 'number' || typeof v === 'string' || typeof v === 'boolean') d[k] = v; }
      if (i.style !== undefined) d.style = String(i.style);
      const kind = kinds.find(([c]) => Object.getPrototypeOf(i) === c.prototype);
      return [kind ? kind[1] : 'X', d];
    });
  }
  function childDesc(c, idx) {
    const n = { i: idx };
    const sym = protoName.get(Object.getPrototypeOf(c));
    if (sym) n.sym = sym;
    if (c instanceof cj.Bitmap) n.t = 'b';
    else if (c instanceof cj.Sprite) n.t = 'sp';
    else if (c instanceof cj.Shape) { n.t = 's'; n.g = gfx(c.graphics); }
    else if (c instanceof cj.Text) { n.t = 'x'; n.text = c.text; n.font = c.font; n.color = c.color; n.align = c.textAlign; n.base = c.textBaseline; n.lw = c.lineWidth; n.lh = c.lineHeight; }
    else if (c instanceof cj.MovieClip) { n.t = 'm'; n.mode = c.mode; n.sp = c.startPosition; n.loop = c.loop; n.cf = c.currentFrame; }
    else if (c instanceof cj.Container) n.t = 'c';
    else n.t = '?';
    const tr = { x: r2(c.x), y: r2(c.y) };
    if (c.scaleX !== 1) tr.sx = r2(c.scaleX); if (c.scaleY !== 1) tr.sy = r2(c.scaleY);
    if (c.rotation) tr.r = r2(c.rotation); if (c.skewX) tr.kx = r2(c.skewX); if (c.skewY) tr.ky = r2(c.skewY);
    if (c.regX) tr.rx = r2(c.regX); if (c.regY) tr.ry = r2(c.regY);
    if (c.alpha !== 1) tr.a = r2(c.alpha); if (!c.visible) tr.v = 0;
    if (c.compositeOperation) tr.co = c.compositeOperation;
    if (c.filters && c.filters.length) tr.fl = c.filters.map(f => f.constructor.name);
    if (c.name) tr.nm = c.name;
    n.tr = tr;
    if (c.mask) n.mask = 1;
    if (c.hitArea) n.hit = 1;
    return n;
  }
  const syms = {};
  for (const [ctor, name] of symName) {
    let inst;
    try { inst = new ctor(); } catch (e) { syms[name] = { err: String(e) }; continue; }
    const s = {};
    if (inst instanceof cj.Bitmap) { s.t = 'b'; const m = ctor.toString().match(/initialize\(\w+\.([\w$]+)\)/); if (m) s.img = m[1]; }
    else if (inst instanceof cj.Shape) { s.t = 's'; s.g = gfx(inst.graphics); }
    else if (inst instanceof cj.MovieClip) {
      s.t = 'm';
      s.tf = inst.totalFrames;
      try { s.labels = (inst.labels || []).map(l => [l.label, l.position]); } catch (e) {}
      const nb = inst.nominalBounds; if (nb) s.nb = [nb.x, nb.y, nb.width, nb.height];
      if (inst.frameBounds) s.fb = inst.frameBounds.map(r => r ? [r.x, r.y, r.width, r.height] : null);
      const acts = [];
      const own = (inst.timeline._tweens || []).find(t => t.target === inst);
      for (const a of (own ? own._actions || [] : [])) acts.push([a.t, a.f ? a.f.toString() : '']);
      s.acts = acts;
      for (const a of (own ? own._actions || [] : [])) if (a.f && a.f.toString().includes('this.T')) { try { a.f.call(inst); } catch (e) {} }
      if (inst.T !== undefined) s.T = inst.T;
      const kidIdx = new Map(), frames = [];
      /* the property each child is kept in (code reaches children by it) */
      const prop = new Map();
      for (const key of Object.keys(inst)) { const v = inst[key]; if (v instanceof cj.DisplayObject && v !== inst && !prop.has(v) && key !== 'parent' && key !== 'oc') prop.set(v, key); }
      inst.actionsEnabled = false;
      for (let f = 0; f < inst.totalFrames; f++) {
        try { inst.gotoAndStop(f); } catch (e) { s.gerr = String(e); }
        frames.push(inst.children.map(c => {
          if (!kidIdx.has(c)) kidIdx.set(c, kidIdx.size);
          const d = childDesc(c, kidIdx.get(c));
          if (prop.has(c)) d.pk = prop.get(c);
          return d;
        }));
      }
      s.frames = frames;
    } else if (inst instanceof cj.Container) { s.t = 'c'; s.kids = inst.children.map((c, i) => childDesc(c, i)); }
    else s.t = '?';
    syms[name] = s;
  }
  const props = lib.properties || {};
  const manifest = (props.aB || props.manifest || []).map(m => [m.id, m.src]);
  return JSON.stringify({ id, w: props.width, h: props.height, fps: props.fps, manifest, syms }, (k, v) => {
    if (v && typeof v === 'object' && v instanceof cj.DisplayObject) return '<DO>';
    if (typeof v === 'function') { const n = symName.get(v); return n ? '<sym:' + n + '>' : '<fn>'; }
    return v;
  });
}

(async () => {
  await new Promise(r => server.listen(0, r));
  const url = `http://localhost:${server.address().port}/index.html`;
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 960, height: 540 } });
  await page.goto(url);
  await page.waitForTimeout(5000);
  await page.mouse.click(480, 270);            // start: loads the deferred libraries
  await page.waitForFunction(() => window.Sk && Object.keys(Sk.Kf).length >= 15, null, { timeout: 60000 });
  for (const id of await page.evaluate(() => Object.keys(Sk.Kf))) {
    const json = await page.evaluate(bake, id);
    fs.writeFileSync(path.join(OUT, id + '.json'), json);
    console.log(id, json.length);
  }
  fs.writeFileSync(path.join(OUT, 'dialog.json'), JSON.stringify(dialogues()));
  await browser.close();
  server.close();
})();

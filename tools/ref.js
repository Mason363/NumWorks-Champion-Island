// Plays the real doodle in headless Chromium and saves reference screenshots
// at the game's own 320x180, for comparing the port against the original.
//
//   node tools/ref.js SRC OUT "script"
//
// The script is a list of steps separated by ';':
//   wait 500        wait 500 ms
//   key Space 100   hold a key (KeyboardEvent key name) for 100 ms
//   down ArrowUp / up ArrowUp
//   shot name       save OUT/name.png (320x180)
//   big name        save OUT/name.png (960x540)
//   eval js         run JavaScript in the page
//   store K V       set the game's saved value K (JSON V) before it starts
//   click X Y       click the page at stage pixel (X, Y) of 960 x 540
const http = require('http');
const fs = require('fs');
const path = require('path');
let chromium;
try { ({ chromium } = require('playwright')); } catch (e) { ({ chromium } = require('/opt/node22/lib/node_modules/playwright')); }

const SRC = path.resolve(process.argv[2]);
const OUT = path.resolve(process.argv[3]);
const SCRIPT = (process.argv[4] || '').split(';').map(s => s.trim()).filter(Boolean);
const QUERY = process.argv[5] || '';   /* e.g. game=pingpong:hard */
fs.mkdirSync(OUT, { recursive: true });

const server = http.createServer((req, res) => {
  const f = path.join(SRC, decodeURIComponent(req.url.split('?')[0]));
  if (!f.startsWith(SRC) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
  fs.createReadStream(f).pipe(res);
});

(async () => {
  await new Promise(r => server.listen(0, r));
  const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
  const page = await browser.newPage({ viewport: { width: 960, height: 540 } });
  const stores = SCRIPT.filter(s => s.startsWith('store ')).map(s => s.split(' '));
  await page.addInitScript((stores) => {
    for (const [, k, ...v] of stores) localStorage.setItem('KITSUNE_' + k, v.join(' '));
  }, stores);
  // the doodle's pixel font comes from gstatic.com: serve the copy next to it
  // (Playwright tries the last route first)
  await page.route(/^https?:\/\/(?!localhost)/, r => r.abort());
  await page.route('**/PixelMplus10-Regular.ttf', r => r.fulfill({ path: path.join(SRC, 'PixelMplus10-Regular.ttf'), contentType: 'font/ttf' }));
  await page.goto(`http://localhost:${server.address().port}/index.html${QUERY ? '?' + QUERY : ''}`);
  await page.waitForTimeout(4000);
  await page.mouse.click(480, 270);
  await page.waitForTimeout(300);
  await page.focus('#hplogo2').catch(() => {});
  const canvas = async () => page.evaluate(() => document.getElementById('hpcanvas').toDataURL('image/png'));
  for (const step of SCRIPT) {
    const [cmd, ...a] = step.split(' ');
    if (cmd === 'wait') await page.waitForTimeout(+a[0]);
    else if (cmd === 'key') { await page.keyboard.down(a[0]); await page.waitForTimeout(+(a[1] || 80)); await page.keyboard.up(a[0]); }
    else if (cmd === 'down') await page.keyboard.down(a[0]);
    else if (cmd === 'up') await page.keyboard.up(a[0]);
    else if (cmd === 'shot' || cmd === 'big') {
      const url = await canvas();
      const buf = Buffer.from(url.split(',')[1], 'base64');
      fs.writeFileSync(path.join(OUT, a[0] + (cmd === 'big' ? '.big' : '') + '.png'), buf);
    } else if (cmd === 'eval') console.log(await page.evaluate(a.join(' ')));
  }
  await browser.close();
  server.close();
})();

// Render the visualization frame by frame and assemble docs/img/exchange.gif.
// Needs Node with Playwright (and its Chromium) and ffmpeg on the PATH.
//   node scripts/capture_gif.js [startSeconds] [lengthSeconds] [fps] [widthPx]
const { chromium } = require('playwright');
const { execFileSync } = require('child_process');
const fs = require('fs'), os = require('os'), path = require('path');

(async () => {
  const start = Number(process.argv[2] || 8), length = Number(process.argv[3] || 14);
  const fps = Number(process.argv[4] || 15), width = Number(process.argv[5] || 960);
  const root = path.resolve(__dirname, '..');
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mxgif-'));
  const browser = await chromium.launch();
  const page = await browser.newPage({ viewport: { width: 1242, height: 900 }, deviceScaleFactor: 1 });
  await page.goto('file://' + path.join(root, 'docs/visualization/index.html'));
  await page.waitForFunction(() => typeof window.renderAt === 'function');
  const frames = Math.round(length * fps);
  for (let i = 0; i < frames; i++) {
    await page.evaluate(ms => window.renderAt(ms), (start + i / fps) * 1000);
    await page.locator('#stage').screenshot({ path: path.join(tmp, `f${String(i).padStart(4, '0')}.png`) });
  }
  await browser.close();
  const out = path.join(root, 'docs/img/exchange.gif');
  const filter = `scale=${width}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle`;
  execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-framerate', String(fps), '-i', path.join(tmp, 'f%04d.png'),
                          '-filter_complex', filter, '-loop', '0', out]);
  fs.rmSync(tmp, { recursive: true });
  console.log(`${frames} frames -> ${out} (${(fs.statSync(out).size / 1e6).toFixed(2)} MB)`);
})();

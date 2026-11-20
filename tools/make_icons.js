// Render the scriptlet icons (scriptlets/icons/*.png) from CrossPoint's logo
// (firmware src/images/logo.svg, MIT). Needs Node with Playwright and
// Chromium; run once when the logo changes, then check in the PNGs:
//   node tools/make_icons.js <out dir> ../crosspoint-reader/src/images/logo.svg
// The checked-in PNGs were then reduced to 16 grays (PIL quantize) to keep the
// scriptlets small; tools/package.sh embeds them (scriptlets/*.sh "# Icon:").
const { chromium } = require('playwright');
const fs = require('fs');
const svg = fs.readFileSync(process.argv[3], 'utf8');
const page = (extra) => `<html><body style="margin:0;background:#fff">
<div style="width:600px;height:600px;position:relative;background:#fff">
 <div style="position:absolute;left:120px;top:100px;width:360px;height:400px">${svg.replace('<svg ', '<svg style="width:100%;height:100%" ')}</div>
 ${extra}
</div></body></html>`;
const glass = `<svg style="position:absolute;left:330px;top:330px;width:240px;height:240px" viewBox="0 0 100 100">
 <circle cx="40" cy="40" r="28" fill="#fff" stroke="#fff" stroke-width="22"/><line x1="61" y1="61" x2="92" y2="92" stroke="#fff" stroke-width="26" stroke-linecap="round"/><circle cx="40" cy="40" r="28" fill="#fff" stroke="#000" stroke-width="10"/>
 <line x1="61" y1="61" x2="92" y2="92" stroke="#000" stroke-width="14" stroke-linecap="round"/></svg>`;
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 600, height: 600 }, deviceScaleFactor: 1 });
  for (const [name, extra] of [['reader', ''], ['probe', glass]]) {
    await p.setContent(page(extra));
    await p.screenshot({ path: process.argv[2] + '/' + name + '.png' });
  }
  await b.close();
})();

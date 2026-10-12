// Records the browser version of the game for the call to action: headless Edge, driven over the Chrome
// DevTools protocol. Plays a move, lets the engine answer, browses its analysis, then solves a late position.
// Saves every repainted frame with its time, and where the (invisible) mouse was, for record_demo.py.
//
//   node record_demo.js [url]        -> assets/demo/raw/*.jpg, assets/demo/raw.json
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const URL_ = process.argv[2] || 'https://boltkey.github.io/snekagon-lab/';
const EDGE = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const PORT = 9333, W = 1280, H = 720, SCALE = 1.25;  // frames of 1600 x 900 with a readable UI
const OUT = path.join(__dirname, 'assets', 'demo');
const RAW = path.join(OUT, 'raw');
const LATE = '1:60 4B:16,27,28,39 3V:40,41,50 4C:49,59,70,71 4I:52,62,72,82 4I:46,57,68,79 4B:80,81,89,90';  // the draw game, move 7

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const now = () => Date.now() / 1000;

async function main() {
  fs.rmSync(RAW, { recursive: true, force: true });
  fs.mkdirSync(RAW, { recursive: true });
  const profile = path.join(require('os').tmpdir(), 'snek-demo-edge');
  const edge = spawn(EDGE, ['--headless=new', '--remote-debugging-port=' + PORT, '--user-data-dir=' + profile,
    '--window-size=' + W * SCALE + ',' + H * SCALE, '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
  let targets;
  for (let k = 0; k < 50 && !targets; k++) {
    await sleep(200);
    try { targets = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json(); } catch (e) {}
  }
  const page = targets.find((t) => t.type === 'page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r));
  let id = 0;
  const waiting = new Map();
  const frames = [];
  ws.addEventListener('message', (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && waiting.has(msg.id)) { waiting.get(msg.id)(msg.result); waiting.delete(msg.id); return; }
    if (msg.method === 'Page.screencastFrame') {
      const file = String(frames.length).padStart(5, '0') + '.jpg';
      fs.writeFileSync(path.join(RAW, file), Buffer.from(msg.params.data, 'base64'));
      frames.push({ file, t: msg.params.metadata.timestamp });
      send('Page.screencastFrameAck', { sessionId: msg.params.sessionId });
    }
  });
  const send = (method, params = {}) => new Promise((r) => { const k = ++id; waiting.set(k, r); ws.send(JSON.stringify({ id: k, method, params })); });
  const js = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result.value;

  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: SCALE, mobile: false });
  await send('Page.navigate', { url: URL_ });
  for (let k = 0; k < 100 && !(await js('typeof st !== "undefined" && !!st && !busy')); k++) await sleep(200);
  // the latest UI has no engine selector; a quick engine for the demo (the real page offers 10 s and more)
  await js(`(() => { const e = document.getElementById('eng'); if (e) e.closest('label').remove();
    const s = document.getElementById('secs'); const o = new Option('2', '2'); s.add(o, 0); s.value = '2';
    // as in the current ui/index.html: the page itself never scrolls when the game history updates
    Element.prototype.scrollIntoView = function () {};
    // the whole board inside a 720 px tall frame
    const css = document.createElement('style'); css.textContent = 'svg#board { max-height: 58vh; }'; document.head.appendChild(css);
    return true; })()`);
  await sleep(2500);  // live evaluation of the start position settles

  const cursor = [];
  let at = [W * 0.55, H * 0.75];
  const move = async (x, y, ms = 700) => {
    const [x0, y0] = at, steps = Math.max(2, Math.round(ms / 25));
    for (let k = 1; k <= steps; k++) {
      const u = k / steps, e = u * u * (3 - 2 * u);  // ease in and out
      at = [x0 + (x - x0) * e, y0 + (y - y0) * e];
      cursor.push({ t: now(), x: at[0], y: at[1] });
      await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: at[0], y: at[1] });
      await sleep(ms / steps);
    }
  };
  const click = async () => {
    cursor.push({ t: now(), x: at[0], y: at[1], click: true });
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: at[0], y: at[1], button: 'left', clickCount: 1 });
    await sleep(80);
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: at[0], y: at[1], button: 'left', clickCount: 1 });
  };
  const centre = async (sel) => js(`(() => { const r = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect();
    return [r.x + r.width / 2, r.y + r.height / 2]; })()`);
  const idle = async () => { for (let k = 0; k < 150 && (await js('busy || pendingEngine')); k++) await sleep(100); };

  await send('Page.startScreencast', { format: 'jpeg', quality: 92, maxWidth: W * SCALE, maxHeight: H * SCALE, everyNthFrame: 1 });
  const t0 = now();
  await sleep(600);
  // 1. red's opening hex in the centre; the engine answers for blue
  await move(...(await centre('polygon[data-i="60"]')), 900);
  await sleep(500);
  await click();
  await sleep(400);
  await idle();
  await sleep(1800);  // the live evaluation comes in
  // 2. browse the engine's analysis: hovering a row previews the move on the board
  for (const k of [0, 1, 2]) {
    const sel = `#anl tr.mv[data-k="${k}"]`;
    if (!(await js(`!!document.querySelector('${sel}')`))) break;
    await move(...(await centre(sel)), 600);
    await sleep(700);
  }
  // 3. a late position, solved exactly
  await js(`(async () => { game = []; await apply(await cmd('set ${LATE}')); render(); return true; })()`);
  await sleep(900);
  await move(...(await centre('#bSolve')), 800);
  await sleep(300);
  await click();
  await idle();
  await sleep(900);
  for (const k of [0, 3]) {
    const sel = `#anl tr.mv[data-k="${k}"]`;
    if (!(await js(`!!document.querySelector('${sel}')`))) break;
    await move(...(await centre(sel)), 600);
    await sleep(900);
  }
  await sleep(600);
  await send('Page.stopScreencast');
  await sleep(300);
  fs.writeFileSync(path.join(OUT, 'raw.json'), JSON.stringify({ t0, t1: now(), width: W, height: H, scale: SCALE, frames, cursor }, null, 0));
  console.log('frames', frames.length, 'seconds', (now() - t0).toFixed(1), 'cursor points', cursor.length);
  ws.close();
  edge.kill();
}

main().catch((e) => { console.error(e); process.exit(1); });

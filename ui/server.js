// Tiny dependency-free server: serves the UI and forwards commands to the engine.
//   node ui/server.js [port] [engineThreads]
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const port = +process.argv[2] || 8080;
const threads = process.argv[3] || '8';
const exe = path.join(__dirname, '..', 'build', process.platform === 'win32' ? 'snek.exe' : 'snek');

const engine = spawn(exe, ['serve', threads], { stdio: ['pipe', 'pipe', 'inherit'] });
engine.on('exit', (c) => { console.error('engine exited', c); process.exit(1); });

let buf = '';
const waiting = [];
engine.stdout.on('data', (d) => {
  buf += d.toString();
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, i);
    buf = buf.slice(i + 1);
    const w = waiting.shift();
    if (w) w(line);
  }
});
let chain = Promise.resolve();
function ask(cmd) {
  const p = chain.then(() => new Promise((res) => {
    waiting.push(res);
    engine.stdin.write(cmd.replace(/[\r\n]/g, ' ') + '\n');
  }));
  chain = p.catch(() => {});
  return p;
}

http.createServer((req, res) => {
  if (req.method === 'POST' && req.url === '/cmd') {
    let body = '';
    req.on('data', (d) => (body += d));
    req.on('end', async () => {
      const out = await ask(body.trim());
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(out);
    });
    return;
  }
  const urlPath = decodeURIComponent(req.url.split('?')[0]);
  const file = urlPath === '/' ? 'index.html' : urlPath.slice(1);
  const fp = path.join(__dirname, path.normalize(file).replace(/^(\.\.[\/\\])+/, ''));
  fs.readFile(fp, (err, data) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    const type = fp.endsWith('.js') ? 'text/javascript' : fp.endsWith('.css') ? 'text/css' : 'text/html';
    res.writeHead(200, { 'Content-Type': type + '; charset=utf-8' });
    res.end(data);
  });
}).listen(port, () => console.log(`Snekagon UI on http://localhost:${port}`));

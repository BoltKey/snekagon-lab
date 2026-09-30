// Tiny dependency-free server: serves the UI, forwards commands to the engine and runs
// long exact verifications ("jobs") as separate solver processes.
//   node ui/server.js [port] [engineThreads] [jobThreads]
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawn } = require('child_process');

const port = +process.argv[2] || 8080;
const threads = process.argv[3] || '8';
const jobThreads = process.argv[4] || String(Math.max(1, os.cpus().length - 4));
const exe = path.join(__dirname, '..', 'build', process.platform === 'win32' ? 'snek.exe' : 'snek');
const jobDir = path.join(__dirname, '..', 'results', 'jobs');
fs.mkdirSync(jobDir, { recursive: true });

// ---- interactive engine (one command at a time) ----
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

// ---- exact verification jobs: `snek solvepos` with a resumable progress log ----
let job = null;  // {id, moves, proc, started}
const norm = (moves) => moves.trim().split(/\s+/).filter(Boolean).join(' ');
const jobId = (moves) => crypto.createHash('sha1').update(norm(moves)).digest('hex').slice(0, 12);
const jobFiles = (id) => ({ log: path.join(jobDir, id + '.log'), out: path.join(jobDir, id + '.out'), moves: path.join(jobDir, id + '.moves') });
const read = (f) => { try { return fs.readFileSync(f, 'utf8'); } catch { return ''; } };

function jobStatus(moves) {
  if (moves === undefined) {
    if (!job) {  // most recent job on disk
      const files = fs.readdirSync(jobDir).filter((f) => f.endsWith('.moves'))
        .map((f) => ({ f, t: fs.statSync(path.join(jobDir, f)).mtimeMs })).sort((a, b) => b.t - a.t);
      if (!files.length) return { exists: false };
      moves = read(path.join(jobDir, files[0].f));
    } else moves = job.moves;
  }
  moves = norm(moves);
  const id = jobId(moves);
  const f = jobFiles(id);
  if (!fs.existsSync(f.moves)) return { exists: false, id, moves };
  const res = { exists: true, id, moves, running: !!(job && job.id === id), wins: [], draws: [], losses: 0, solved: 0, threads: +jobThreads };
  let secSum = 0;
  for (const line of read(f.log).split('\n')) {
    const [m, v, , sec] = line.trim().split(/\s+/);
    if (!m || !v) continue;
    res.solved++;
    secSum += +sec || 0;
    if (v === 'win') res.wins.push(m); else if (v === 'draw') res.draws.push(m); else res.losses++;
  }
  const out = read(f.out);
  const t = out.match(/root moves: (\d+)\s+ply (\d+) stm (\d+)/);
  if (t) { res.total = +t[1]; res.ply = +t[2]; res.stm = +t[3]; }
  const fins = [...out.matchAll(/^ply \d+ stm \d+: value (-?\d+|unknown)/gm)];
  const fin = fins.length ? fins[fins.length - 1] : null;  // last run's verdict
  if (fin && !res.running) { res.done = true; res.value = fin[1] === 'unknown' ? null : +fin[1]; }
  const timed = [...read(f.log).split('\n')].filter((l) => l.trim().split(/\s+/)[3]).length;
  res.avgSec = timed ? secSum / timed : null;
  if (job && job.id === id) res.elapsed = (Date.now() - job.started) / 1000;
  return res;
}

function startJob(moves) {
  moves = norm(moves);
  const id = jobId(moves);
  if (job) return job.id === id ? { ok: true, already: true } : { ok: false, error: 'Už běží jiné ověření.' };
  const st = jobStatus(moves);
  if (st.done && st.value !== null) return { ok: true, cached: true };
  const f = jobFiles(id);
  fs.writeFileSync(f.moves, moves);
  const out = fs.openSync(f.out, 'a');
  const proc = spawn(exe, ['solvepos', '864000', jobThreads, ...moves.split(' ')], {
    env: { ...process.env, PROGRESS: f.log }, stdio: ['ignore', out, out],
  });
  job = { id, moves, proc, started: Date.now() };
  proc.on('exit', () => { fs.closeSync(out); if (job && job.proc === proc) job = null; });
  return { ok: true, started: true };
}

function body(req) {
  return new Promise((resolve) => { let b = ''; req.on('data', (d) => (b += d)); req.on('end', () => resolve(b)); });
}
function json(res, obj) {
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}

http.createServer(async (req, res) => {
  const urlPath = decodeURIComponent(req.url.split('?')[0]);
  const query = new URLSearchParams(req.url.split('?')[1] || '');
  if (req.method === 'POST' && urlPath === '/cmd') {
    const out = await ask((await body(req)).trim());
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(out);
    return;
  }
  if (req.method === 'POST' && urlPath === '/job/start') return json(res, startJob(await body(req)));
  if (req.method === 'POST' && urlPath === '/job/stop') {
    if (job) job.proc.kill();
    return json(res, { ok: true });
  }
  if (urlPath === '/job/status') return json(res, jobStatus(query.has('moves') ? query.get('moves') : undefined));
  const file = urlPath === '/' ? 'index.html' : urlPath.slice(1);
  const fp = path.join(__dirname, path.normalize(file).replace(/^(\.\.[\/\\])+/, ''));
  fs.readFile(fp, (err, data) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    const type = fp.endsWith('.js') ? 'text/javascript' : fp.endsWith('.css') ? 'text/css' : 'text/html';
    res.writeHead(200, { 'Content-Type': type + '; charset=utf-8' });
    res.end(data);
  });
}).listen(port, () => console.log(`Snekagon UI on http://localhost:${port} (engine ${threads} threads, jobs ${jobThreads} threads)`));

process.on('exit', () => { if (job) job.proc.kill(); });

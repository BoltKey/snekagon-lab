#!/usr/bin/env python3
"""Coordinator for the distributed exact solve of the Snekagon centre opening.

Goal: prove that after  1:60 4B:16,27,28,39  (red's centre 1-hex and blue's reply) blue wins,
i.e. for EVERY red reply r2 there is a blue move b3 such that EVERY red move r4 loses.

Work is split into units = positions after 5 moves  (base r2 b3 r4, blue to move); a worker
returns the exact value for blue (1 win / 0 draw / -1 loss).
  * all units of a candidate b3 come back 1        -> r2 is refuted ("proven")
  * any unit of a candidate comes back 0 or -1     -> that b3 fails, the next candidate is tried
  * no candidate works                             -> r2 "holds": blue's first move is not proven to win

Workers:  build/snek worker <this-ip>:8090 <threads>
Status:   http://<this-ip>:8091/
State is saved continuously in results/dist/ - restarting resumes where it stopped.
"""
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
EXE = os.path.abspath(os.environ.get('SNEK_EXE') or os.path.join(ROOT, 'build', 'snek.exe' if os.name == 'nt' else 'snek'))
OUT = os.path.join(ROOT, 'results', 'dist')
STATE = os.path.join(OUT, 'state.json')
RESULTS = os.path.join(OUT, 'results.log')
JOBS = os.path.join(ROOT, 'results', 'jobs')
BASE = ['1:60', '4B:16,27,28,39']
WORKER_PORT = int(os.environ.get('WORKER_PORT', 8090))
HTTP_PORT = int(os.environ.get('HTTP_PORT', 8091))
MAX_ACTIVE = 6        # red replies worked on at the same time
MAX_CANDIDATES = 40   # blue replies tried per red reply before giving up
REASSIGN_AFTER = 8 * 3600

lock = threading.RLock()


def log(msg):
    line = time.strftime('%Y-%m-%d %H:%M:%S ') + msg
    print(line, flush=True)
    with open(os.path.join(OUT, 'events.log'), 'a') as f:
        f.write(line + '\n')


class Engine:
    """Persistent `snek serve 1` used for move lists."""
    def __init__(self):
        self.p = subprocess.Popen([EXE, 'serve', '1'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  universal_newlines=True, bufsize=1)

    def children(self, moves):
        self.p.stdin.write('children ' + ' '.join(moves) + '\n')
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        if not r.get('ok'):
            raise RuntimeError('engine: ' + str(r))
        return [m['s'] for m in r['moves']]


engine = Engine()


_jobs = None


def known_job_value(moves):
    """Exact value of a position solved earlier by the UI verification jobs (results/jobs), or None."""
    global _jobs
    if _jobs is None:
        _jobs = {}
        if os.path.isdir(JOBS):
            for fn in os.listdir(JOBS):
                if not fn.endswith('.moves'):
                    continue
                try:
                    key = open(os.path.join(JOBS, fn)).read().strip()
                    out = open(os.path.join(JOBS, fn[:-6] + '.out')).read()
                except OSError:
                    continue
                for line in reversed(out.splitlines()):
                    if ': value ' in line and line.startswith('ply'):
                        v = line.split(': value ')[1].split()[0]
                        if v != 'unknown':
                            _jobs[key] = int(v)
                        break
    return _jobs.get(' '.join(moves))


# ---------------------------------------------------------------- state ----
def new_state():
    r2 = engine.children(BASE)
    log('red has %d distinct replies to %s' % (len(r2), ' '.join(BASE)))
    return {'base': BASE, 'created': time.time(), 'units_done': 0, 'unit_seconds': 0.0,
            'entries': [{'r2': m, 'status': 'todo', 'cands': None, 'ci': 0, 'r4': None, 'res': {}}
                        for m in r2]}


def load_state():
    if os.path.exists(STATE):
        with open(STATE) as f:
            st = json.load(f)
        if st.get('base') == BASE:
            return st
    return new_state()


state = None
assigned = {}   # unit id -> (worker name, time)
workers = {}    # connection id -> dict
dirty = True


def save_state():
    global dirty
    tmp = STATE + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(state, f)
    os.replace(tmp, STATE)
    dirty = False


def activate(ei):
    """Prepare entry ei: blue's candidate replies and red's moves for the current candidate."""
    e = state['entries'][ei]
    if e['cands'] is None:
        e['cands'] = engine.children(BASE + [e['r2']])[:MAX_CANDIDATES]
        # a candidate already proven by an earlier verification job settles the entry at once
        for b3 in engine.children(BASE + [e['r2']]):
            if known_job_value(BASE + [e['r2'], b3]) == -1:
                e['status'] = 'proven'
                e['by'] = b3
                log('red reply %s refuted by %s (earlier verification)' % (e['r2'], b3))
                return
    while True:
        if e['ci'] >= len(e['cands']):
            e['status'] = 'holds'
            log('!!! red reply %s: no blue reply found among %d candidates' % (e['r2'], len(e['cands'])))
            return
        b3 = e['cands'][e['ci']]
        p4 = BASE + [e['r2'], b3]
        v = known_job_value(p4)
        if v == -1:  # red to move loses -> candidate proven by an earlier verification job
            e['status'] = 'proven'
            e['by'] = b3
            log('red reply %s refuted by %s (earlier verification)' % (e['r2'], b3))
            return
        if v is not None:
            e['ci'] += 1
            continue
        e['r4'] = engine.children(p4)
        e['res'] = {}
        e['status'] = 'active'
        if not e['r4']:  # red has no move at all: cannot happen this early, but be safe
            e['status'] = 'proven'
            e['by'] = b3
        return


def ensure_active():
    act = [i for i, e in enumerate(state['entries']) if e['status'] == 'active']
    for i, e in enumerate(state['entries']):
        if len(act) >= MAX_ACTIVE:
            break
        if e['status'] == 'todo':
            activate(i)
            if e['status'] == 'active':
                act.append(i)


def next_units(n, worker):
    out = []
    now = time.time()
    for uid, (w, t) in list(assigned.items()):
        if now - t > REASSIGN_AFTER:
            del assigned[uid]
    for _ in range(2):
        for ei, e in enumerate(state['entries']):
            if e['status'] != 'active':
                continue
            for ri, r4 in enumerate(e['r4']):
                if len(out) >= n:
                    return out
                if r4 in e['res']:
                    continue
                uid = '%d.%d.%d' % (ei, e['ci'], ri)
                if uid in assigned:
                    continue
                assigned[uid] = (worker, now)
                out.append((uid, BASE + [e['r2'], e['cands'][e['ci']], r4]))
        if len(out) >= n:
            break
        ensure_active()
    return out


def on_result(uid, value, nodes, sec, worker):
    global dirty
    assigned.pop(uid, None)
    try:
        ei, ci, ri = map(int, uid.split('.'))
    except ValueError:
        return
    e = state['entries'][ei]
    with open(RESULTS, 'a') as f:
        f.write('%s %s %s %s %s\n' % (uid, value, nodes, sec, worker))
    state['units_done'] += 1
    state['unit_seconds'] += float(sec)
    dirty = True
    if e['status'] != 'active' or e['ci'] != ci:
        return  # stale result of an abandoned candidate
    r4 = e['r4'][ri]
    if value == 'bad':
        log('worker %s could not parse unit %s' % (worker, uid))
        return
    v = int(value)
    if v == 1:
        e['res'][r4] = 1
        if len(e['res']) == len(e['r4']):
            e['status'] = 'proven'
            e['by'] = e['cands'][ci]
            e['r4'] = None
            e['res'] = {}
            done = sum(1 for x in state['entries'] if x['status'] == 'proven')
            log('red reply %s refuted by %s  (%d/%d)' % (e['r2'], e['by'], done, len(state['entries'])))
            ensure_active()
    else:
        log('red reply %s: blue candidate %s fails (%s after %s)' % (e['r2'], e['cands'][ci], 'draw' if v == 0 else 'loss', r4))
        e['ci'] += 1
        e['r4'] = None
        activate(ei)


# ------------------------------------------------------------ workers ----
class WorkerHandler(socketserver.StreamRequestHandler):
    def handle(self):
        cid = id(self)
        name = self.client_address[0]
        with lock:
            workers[cid] = {'name': name, 'ip': self.client_address[0], 'threads': 0, 'done': 0,
                            'since': time.time(), 'last': time.time(), 'secs': 0.0}
        try:
            for raw in self.rfile:
                parts = raw.decode(errors='replace').split()
                if not parts:
                    continue
                with lock:
                    w = workers[cid]
                    w['last'] = time.time()
                    if parts[0] == 'hello' and len(parts) >= 3:
                        w['name'], w['threads'] = parts[1], int(parts[2])
                        log('worker %s connected from %s with %s threads' % (parts[1], w['ip'], parts[2]))
                    elif parts[0] == 'get':
                        units = next_units(int(parts[1]), w['name'] + '#' + str(cid))
                        msg = ''.join('unit %s %s\n' % (u, ' '.join(m)) for u, m in units) + 'end\n'
                        self.wfile.write(msg.encode())
                        self.wfile.flush()
                    elif parts[0] == 'result' and len(parts) >= 5:
                        w['done'] += 1
                        w['secs'] += float(parts[4])
                        on_result(parts[1], parts[2], parts[3], parts[4], w['name'])
        except (ConnectionError, OSError):
            pass
        finally:
            with lock:
                w = workers.pop(cid, None)
                tag = '#' + str(cid)
                for uid, (wn, t) in list(assigned.items()):
                    if wn.endswith(tag):
                        del assigned[uid]
                if w:
                    log('worker %s disconnected (%d units)' % (w['name'], w['done']))


class ThreadedTCP(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


class ThreadedHTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):  # (ThreadingHTTPServer needs 3.7)
    daemon_threads = True


# ------------------------------------------------------------- status ----
def status():
    with lock:
        ents = state['entries']
        counts = {}
        for e in ents:
            counts[e['status']] = counts.get(e['status'], 0) + 1
        active = []
        for e in ents:
            if e['status'] == 'active':
                active.append({'r2': e['r2'], 'cand': e['cands'][e['ci']], 'ci': e['ci'],
                               'done': len(e['res']), 'total': len(e['r4'])})
        now = time.time()
        ws = [{'name': w['name'], 'ip': w['ip'], 'threads': w['threads'], 'done': w['done'],
               'avg': (w['secs'] / w['done']) if w['done'] else None, 'minutes': (now - w['since']) / 60}
              for w in workers.values()]
        threads = sum(w['threads'] for w in workers.values())
        units = state['units_done']
        avg = state['unit_seconds'] / units if units else None
        proven = [e for e in ents if e['status'] == 'proven']
        # rough ETA: units per entry observed so far (≈ red moves per candidate)
        per_entry = 950.0
        remaining_units = sum((len(e['r4']) - len(e['res'])) if e['status'] == 'active' else 0 for e in ents) + \
            per_entry * counts.get('todo', 0)
        eta_h = (remaining_units * avg / threads / 3600) if (avg and threads) else None
        return {'base': ' '.join(BASE), 'entries': len(ents), 'counts': counts, 'active': active,
                'workers': ws, 'threads': threads, 'units_done': units, 'avg_unit_sec': avg,
                'eta_hours': eta_h, 'holds': [e['r2'] for e in ents if e['status'] == 'holds'],
                'recent': [(e['r2'], e.get('by')) for e in proven[-10:]]}


PAGE = '''<!doctype html><html lang="cs"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Snekagon – distribuovaný výpočet</title>
<style>body{font:14px system-ui,sans-serif;margin:16px;max-width:900px;background:#f4f1ea;color:#2b2620}
table{border-collapse:collapse;width:100%;margin:8px 0}td,th{border-bottom:1px solid #d9d0c2;padding:4px;text-align:left}
.bar{height:14px;background:#e8e2d6;border-radius:7px;overflow:hidden}.bar div{height:100%;background:#2e7d32}
.muted{color:#7a7064}.bad{color:#c62828;font-weight:600}</style></head><body>
<h2>Snekagon – exaktní řešení otevření doprostřed</h2><div id="c">…</div>
<script>
async function tick(){
  const s = await (await fetch('status')).json();
  const n = s.entries, p = s.counts.proven||0;
  let h = `<p>Dokazuje se: po <b>${s.base}</b> (na tahu červený) vyhrává modrý.<br>
  Vyvráceno <b>${p}</b> z ${n} odpovědí červeného.</p>
  <div class="bar"><div style="width:${(100*p/n).toFixed(2)}%"></div></div>
  <p class="muted">Úloh (pozic po 5 tazích) vyřešeno: ${s.units_done}, průměr ${s.avg_unit_sec? s.avg_unit_sec.toFixed(1):'?'} s/úlohu/vlákno.
  Vláken: ${s.threads}. Odhad do konce: ${s.eta_hours? (s.eta_hours>48? (s.eta_hours/24).toFixed(1)+' dní' : s.eta_hours.toFixed(1)+' h') : '?'}</p>`;
  if (s.holds.length) h += `<p class="bad">Odpovědi červeného, na které se nenašla vítězná odpověď modrého: ${s.holds.join(', ')}</p>`;
  h += '<h3>Workery</h3><table><tr><th>jméno</th><th>IP</th><th>vlákna</th><th>úloh</th><th>s/úlohu</th><th>připojen</th></tr>' +
    s.workers.map(w=>`<tr><td>${w.name}</td><td>${w.ip}</td><td>${w.threads}</td><td>${w.done}</td><td>${w.avg? w.avg.toFixed(1):''}</td><td>${w.minutes.toFixed(0)} min</td></tr>`).join('') + '</table>';
  h += '<h3>Právě se řeší</h3><table><tr><th>odpověď červeného</th><th>kandidát modrého</th><th>průběh</th></tr>' +
    s.active.map(a=>`<tr><td>${a.r2}</td><td>${a.cand}${a.ci? ' <span class="muted">(pokus '+(a.ci+1)+')</span>':''}</td><td>${a.done}/${a.total}</td></tr>`).join('') + '</table>';
  h += '<h3>Poslední vyvrácené</h3><table>' + s.recent.reverse().map(r=>`<tr><td>${r[0]}</td><td>→ ${r[1]}</td></tr>`).join('') + '</table>';
  document.getElementById('c').innerHTML = h;
}
tick(); setInterval(tick, 10000);
</script></body></html>'''


class StatusHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/status'):
            body = json.dumps(status()).encode()
            ctype = 'application/json'
        elif self.path == '/' or self.path.startswith('/index'):
            body = PAGE.encode()
            ctype = 'text/html; charset=utf-8'
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def saver():
    while True:
        time.sleep(10)
        with lock:
            if dirty:
                save_state()


def main():
    global state
    os.makedirs(OUT, exist_ok=True)
    with lock:
        state = load_state()
        ensure_active()
        save_state()
    threading.Thread(target=saver, daemon=True).start()
    httpd = ThreadedHTTP(('0.0.0.0', HTTP_PORT), StatusHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    srv = ThreadedTCP(('0.0.0.0', WORKER_PORT), WorkerHandler)
    log('coordinator: workers on port %d, status page on port %d' % (WORKER_PORT, HTTP_PORT))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        with lock:
            save_state()


if __name__ == '__main__':
    main()

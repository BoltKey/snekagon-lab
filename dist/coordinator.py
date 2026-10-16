#!/usr/bin/env python3
"""Coordinator for the distributed exact solve of the Snekagon centre opening.

Goal: prove that after  1:60 4B:16,27,28,39  (red's centre 1-hex and blue's reply) blue wins,
i.e. for EVERY red reply r2 there is a blue move b3 such that EVERY red move r4 loses.

Work is split into units = positions after 5 moves (base r2 b3 r4, blue to move); a worker
returns the value for blue (1 win / 0 draw / -1 loss, -9 = node budget exceeded). A worker
started with SOLVER_WIN_TEST=1 only asks whether blue wins: its 0 means no win (a draw or a loss).

Per red reply r2 ("entry"):
  * blue candidates b3 are ranked by an alpha-beta search (precomputed in the background),
    then by the 1-ply evaluation
  * red moves that refuted earlier candidates ("killers") are tried first for the next one
  * phase 1: every unit gets a node budget; units that run out are postponed ("hard").
    A killer running out of budget marks the candidate as suspect and the next one is tried.
  * phase 2: the hard units of a candidate are solved without budget
  * all units of a candidate come back 1        -> r2 is refuted ("proven")
  * a unit comes back 0 or -1                   -> that candidate fails, the next one is tried
  * candidates exhausted                        -> suspects are retried without budget
  * nothing works                               -> r2 "holds": blue's first move is not proven to win
A candidate is only accepted when every unit is an exact blue win, so budgets never affect soundness.

Workers:  build/snek worker <this-ip>:8090 <threads>
Status:   http://<this-ip>:8091/   (also /state and /events)
State is saved continuously in results/dist/ - restarting resumes where it stopped.
"""
import http.server
import json
import os
import socketserver
import subprocess
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
EXE = os.path.abspath(os.environ.get('SNEK_EXE') or os.path.join(ROOT, 'build', 'snek.exe' if os.name == 'nt' else 'snek'))
OUT = os.path.abspath(os.environ.get('OUT_DIR') or os.path.join(ROOT, 'results', 'dist'))
STATE = os.path.join(OUT, 'state.json')
RESULTS = os.path.join(OUT, 'results.log')
EVENTS = os.path.join(OUT, 'events.log')
JOBS = os.path.join(ROOT, 'results', 'jobs')
BASE = ['1:60', '4B:16,27,28,39']
WORKER_PORT = int(os.environ.get('WORKER_PORT', 8090))
HTTP_PORT = int(os.environ.get('HTTP_PORT', 8091))
MAX_ACTIVE = 6            # red replies worked on at the same time
MAX_CANDIDATES = 40       # blue replies tried per red reply
PROBE_NODES = int(os.environ.get('PROBE_NODES', 150000000))  # phase-1 node budget per unit
ANALYZE_SECS = float(os.environ.get('ANALYZE_SECS', 10))      # alpha-beta time for ranking blue's candidates
ANALYZE_THREADS = os.environ.get('ANALYZE_THREADS', '3')
REASSIGN_AFTER = 12 * 3600

lock = threading.RLock()


def log(msg):
    line = time.strftime('%Y-%m-%d %H:%M:%S ') + msg
    print(line, flush=True)
    with open(EVENTS, 'a') as f:
        f.write(line + '\n')


class Engine:
    """Persistent `snek serve` process."""
    def __init__(self, threads='1'):
        self.p = subprocess.Popen([EXE, 'serve', threads], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  universal_newlines=True, bufsize=1)

    def ask(self, cmd):
        self.p.stdin.write(cmd + '\n')
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def children(self, moves):
        r = self.ask('children ' + ' '.join(moves))
        if not r.get('ok'):
            raise RuntimeError('engine: ' + str(r))
        return [m['s'] for m in r['moves']]

    def ranked(self, moves):
        """Candidate moves: alpha-beta's top moves first, then the 1-ply evaluation order."""
        self.ask('set ' + ' '.join(moves))
        a = self.ask('analyze %g' % ANALYZE_SECS)
        top = [m['m']['s'] for m in a.get('moves', [])[:8]] if a.get('ok') else []
        rest = self.children(moves)
        out = []
        for m in top + rest:
            if m not in out and m != 'pass':
                out.append(m)
        return out


engine = Engine('1')           # quick move lists (used under the lock)
ranker = Engine(ANALYZE_THREADS)  # alpha-beta ranking (background thread only)

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
DEFAULTS = {'killers': [], 'failed': [], 'suspects': [], 'hard': [], 'phase': 1, 'gen': 1,
            'revisit': False, 'ranked': False}


def new_entry(m):
    e = {'r2': m, 'status': 'todo', 'cands': None, 'ci': 0, 'r4': None, 'res': {}}
    for k, v in DEFAULTS.items():
        e[k] = json.loads(json.dumps(v))
    return e


def new_state():
    r2 = engine.children(BASE)
    log('red has %d distinct replies to %s' % (len(r2), ' '.join(BASE)))
    return {'base': BASE, 'version': 2, 'created': time.time(), 'units_done': 0, 'unit_seconds': 0.0,
            'entries': [new_entry(m) for m in r2]}


def migrate(st):
    """Version-1 state: add the new fields and rebuild killers/failures from results.log."""
    old_fail = {}
    spent = {}
    if os.path.exists(RESULTS):
        for line in open(RESULTS):
            p = line.split()
            if len(p) >= 4:
                try:
                    k = int(p[0].split('.')[0])
                    u, sec = spent.get(k, (0, 0.0))
                    spent[k] = (u + 1, sec + float(p[3]))
                except ValueError:
                    pass
            if len(p) < 2 or p[1] not in ('0', '-1'):
                continue
            ids = p[0].split('.')
            if len(ids) == 3:
                old_fail.setdefault(int(ids[0]), []).append((int(ids[1]), int(ids[2]), int(p[1])))
    for ei, e in enumerate(st['entries']):
        for k, v in DEFAULTS.items():
            e.setdefault(k, json.loads(json.dumps(v)))
        if ei in spent:
            e['units'], e['secs'] = spent[ei]
        if e['status'] == 'proven' and 'win_rank' not in e and e['cands'] and e.get('by') in e['cands']:
            e['win_rank'] = e['cands'].index(e['by'])
        if e['cands'] is None:
            continue
        for ci, ri, v in old_fail.get(ei, []):
            if ci >= len(e['cands']):
                continue
            cand = e['cands'][ci]
            r4 = engine.children(BASE + [e['r2'], cand])[ri]
            e['failed'].append([cand, r4, v])
            if r4 not in e['killers']:
                e['killers'].insert(0, r4)
        if e['status'] == 'active':
            # keep the candidate in progress first; the rest gets re-ranked by alpha-beta
            cur = e['cands'][e['ci']]
            done_fail = set(f[0] for f in e['failed'])
            rest = [m for m in ranker.ranked(BASE + [e['r2']]) if m != cur and m not in done_fail]
            e['cands'] = ([cur] + rest)[:MAX_CANDIDATES]
            e['ci'] = 0
            e['ranked'] = True
            e['r4'] = order_r4(e, e['r4'])
            log('migrated active entry %s: %d failed candidates, killers %s' % (e['r2'], len(e['failed']), e['killers'][:4]))
    st['version'] = 2
    return st


def load_state():
    if os.path.exists(STATE):
        with open(STATE) as f:
            st = json.load(f)
        if st.get('base') == BASE:
            return st if st.get('version') == 2 else migrate(st)
    return new_state()


state = None
assigned = {}   # unit id -> (worker tag, time)
workers = {}    # connection id -> dict
dirty = True


def save_state():
    global dirty
    tmp = STATE + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(state, f)
    os.replace(tmp, STATE)
    dirty = False


def order_r4(e, r4):
    """Killers (red moves that refuted earlier candidates) first."""
    if not r4:
        return r4
    first = [m for m in e['killers'] if m in r4]
    return first + [m for m in r4 if m not in first]


def setup_candidate(ei):
    """Prepare the current candidate of entry ei, skipping ones settled by earlier jobs."""
    e = state['entries'][ei]
    while True:
        if e['ci'] >= len(e['cands']):
            if e['suspects'] and not e['revisit']:
                log('red reply %s: retrying %d suspect candidates without budget' % (e['r2'], len(e['suspects'])))
                e['revisit'] = True
                e['cands'] = e['suspects']
                e['suspects'] = []
                e['ci'] = 0
                continue
            e['status'] = 'holds'
            e['r4'] = None
            log('!!! red reply %s: no winning blue reply found (%d failed, %d candidates)' % (e['r2'], len(e['failed']), len(e['cands'])))
            return
        b3 = e['cands'][e['ci']]
        p4 = BASE + [e['r2'], b3]
        v = known_job_value(p4)
        if v == -1:  # red to move loses -> proven by an earlier verification job
            e['status'] = 'proven'
            e['by'] = b3
            e['win_rank'] = e['ci']
            e['proven_at'] = time.time()
            e['r4'] = None
            log('red reply %s refuted by %s (earlier verification)' % (e['r2'], b3))
            return
        if v is not None:
            e['ci'] += 1
            continue
        e['r4'] = order_r4(e, engine.children(p4))
        e['res'] = {}
        e['hard'] = []
        e['phase'] = 2 if e['revisit'] else 1
        e['status'] = 'active'
        if not e['r4']:
            e['status'] = 'proven'
            e['by'] = b3
            e['proven_at'] = time.time()
        return


def next_candidate(ei):
    e = state['entries'][ei]
    e['gen'] += 1
    e['ci'] += 1
    setup_candidate(ei)


def ensure_active():
    act = sum(1 for e in state['entries'] if e['status'] == 'active')
    for i, e in enumerate(state['entries']):
        if act >= MAX_ACTIVE:
            break
        if e['status'] == 'todo' and e['ranked']:
            setup_candidate(i)
            if e['status'] == 'active':
                act += 1


def pending_units(ei, e):
    """(r4 index, budget) of units still to hand out for the current candidate."""
    out = []
    if e['phase'] == 1:
        for ri, r4 in enumerate(e['r4']):
            if r4 not in e['res'] and r4 not in e['hard']:
                out.append((ri, PROBE_NODES))
    else:
        for ri, r4 in enumerate(e['r4']):
            if r4 not in e['res']:
                out.append((ri, 0))
    return out


def next_units(n, worker):
    out = []
    now = time.time()
    for uid, (w, t) in list(assigned.items()):
        if now - t > REASSIGN_AFTER:
            del assigned[uid]
    for attempt in range(2):
        for ei, e in enumerate(state['entries']):
            if e['status'] != 'active':
                continue
            for ri, budget in pending_units(ei, e):
                if len(out) >= n:
                    return out
                uid = '%d.%d.%d.%d' % (ei, e['gen'], ri, e['phase'])
                if uid in assigned:
                    continue
                assigned[uid] = (worker, now)
                moves = BASE + [e['r2'], e['cands'][e['ci']], e['r4'][ri]]
                out.append((uid, ([str(budget)] if budget else []) + moves))
        if len(out) >= n:
            break
        ensure_active()
    return out


def check_progress(ei):
    e = state['entries'][ei]
    if len(e['res']) == len(e['r4']):
        e['status'] = 'proven'
        e['by'] = e['cands'][e['ci']]
        e['win_rank'] = -1 if e['revisit'] else e['ci']  # position in the candidate ranking
        e['proven_at'] = time.time()
        e['r4'] = None
        e['res'] = {}
        e['hard'] = []
        done = sum(1 for x in state['entries'] if x['status'] == 'proven')
        log('red reply %s refuted by %s  (%d/%d)' % (e['r2'], e['by'], done, len(state['entries'])))
        ensure_active()
    elif e['phase'] == 1 and len(e['res']) + len(e['hard']) == len(e['r4']):
        e['phase'] = 2
        log('red reply %s, candidate %s: %d hard units left, solving them without budget' % (e['r2'], e['cands'][e['ci']], len(e['hard'])))


def on_result(uid, value, nodes, sec, worker):
    global dirty
    assigned.pop(uid, None)
    with open(RESULTS, 'a') as f:
        f.write('%s %s %s %s %s\n' % (uid, value, nodes, sec, worker))
    state['units_done'] += 1
    state['unit_seconds'] += float(sec)
    dirty = True
    ids = uid.split('.')
    if len(ids) != 4:
        return  # result of the previous protocol version
    ei, gen, ri, phase = map(int, ids)
    e = state['entries'][ei]
    e['units'] = e.get('units', 0) + 1  # compute spent on this red reply (incl. abandoned candidates)
    e['secs'] = e.get('secs', 0.0) + float(sec)
    if e['status'] != 'active' or e['gen'] != gen:
        return  # stale result of an abandoned candidate
    if value == 'bad':
        log('worker %s could not parse unit %s (old worker binary?)' % (worker, uid))
        return
    r4 = e['r4'][ri]
    cand = e['cands'][e['ci']]
    v = int(value)
    if v == 1:
        e['res'][r4] = 1
        if r4 in e['hard']:
            e['hard'].remove(r4)
        check_progress(ei)
    elif v in (0, -1):
        log('red reply %s: blue candidate %s fails (%s after %s)' % (e['r2'], cand, 'no win' if v == 0 else 'loss', r4))
        e['failed'].append([cand, r4, v])
        if r4 in e['killers']:
            e['killers'].remove(r4)
        e['killers'].insert(0, r4)
        del e['killers'][12:]
        next_candidate(ei)
    elif v == -9:
        if phase == 1 and r4 in e['killers'] and not e['revisit']:
            log('red reply %s: candidate %s suspect (killer %s exceeded the budget), next candidate' % (e['r2'], cand, r4))
            e['suspects'].append(cand)
            next_candidate(ei)
        else:
            if r4 not in e['hard']:
                e['hard'].append(r4)
            check_progress(ei)


# ------------------------------------------------------------ ranking ----
def ranking_thread():
    """Ranks blue's candidates for the next entries with alpha-beta, ahead of time."""
    while True:
        todo = None
        with lock:
            k = 0
            for e in state['entries']:
                if e['status'] == 'todo':
                    k += 1
                    if not e['ranked']:
                        todo = e
                        break
                    if k > MAX_ACTIVE + 4:
                        break
        if todo is None:
            time.sleep(5)
            continue
        cands = ranker.ranked(BASE + [todo['r2']])[:MAX_CANDIDATES]
        with lock:
            todo['cands'] = cands
            todo['ranked'] = True
            ensure_active()


# ------------------------------------------------------------ workers ----
class WorkerHandler(socketserver.StreamRequestHandler):
    def handle(self):
        cid = id(self)
        tag = '#' + str(cid)
        with lock:
            workers[cid] = {'name': self.client_address[0], 'ip': self.client_address[0], 'threads': 0, 'done': 0,
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
                        units = next_units(int(parts[1]), w['name'] + tag)
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
                active.append({'r2': e['r2'], 'cand': e['cands'][e['ci']], 'ci': e['ci'], 'phase': e['phase'],
                               'revisit': e['revisit'], 'done': len(e['res']), 'hard': len(e['hard']),
                               'total': len(e['r4']), 'failed': e['failed'][-3:], 'suspects': len(e['suspects'])})
        now = time.time()
        ws = [{'name': w['name'], 'ip': w['ip'], 'threads': w['threads'], 'done': w['done'],
               'avg': (w['secs'] / w['done']) if w['done'] else None, 'minutes': (now - w['since']) / 60}
              for w in workers.values()]
        threads = sum(w['threads'] for w in workers.values())
        units = state['units_done']
        proven = [e for e in ents if e['status'] == 'proven']
        recent = [e for e in proven if e.get('proven_at') and now - e['proven_at'] < 12 * 3600]
        rate = None
        if len(recent) >= 3:
            span = max(now - min(e['proven_at'] for e in recent), 3600.0)
            rate = len(recent) / (span / 3600.0)
        remaining = counts.get('todo', 0) + counts.get('active', 0)
        return {'base': ' '.join(BASE), 'entries': len(ents), 'counts': counts, 'active': active,
                'workers': ws, 'threads': threads, 'units_done': units,
                'avg_unit_sec': state['unit_seconds'] / units if units else None,
                'rate_per_hour': rate, 'eta_hours': remaining / rate if rate else None,
                'ranked_ahead': sum(1 for e in ents if e['status'] == 'todo' and e['ranked']),
                'holds': [e['r2'] for e in ents if e['status'] == 'holds'],
                'recent': [(e['r2'], e.get('by'), len(e['failed'])) for e in proven[-12:]]}


def table():
    """Per red reply: winning blue reply and how hard it was to find."""
    with lock:
        out = []
        for e in state['entries']:
            if e['status'] == 'todo':
                continue
            out.append({'r2': e['r2'], 'status': e['status'], 'by': e.get('by'),
                        'cand': e['cands'][e['ci']] if e['status'] == 'active' else None,
                        'failed': len(e['failed']), 'suspects': len(e['suspects']),
                        'win_rank': e.get('win_rank'), 'units': e.get('units', 0),
                        'hours': e.get('secs', 0.0) / 3600.0, 'refuted': [[f[0], f[1]] for f in e['failed']][-6:],
                        'proven_at': e.get('proven_at')})
        return {'base': ' '.join(BASE), 'rows': out}


PAGE = '''<!doctype html><html lang="cs"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Snekagon – distribuovaný výpočet</title>
<style>body{font:14px system-ui,sans-serif;margin:16px;max-width:980px;background:#f4f1ea;color:#2b2620}
table{border-collapse:collapse;width:100%;margin:8px 0}td,th{border-bottom:1px solid #d9d0c2;padding:4px;text-align:left;vertical-align:top}
.bar{height:14px;background:#e8e2d6;border-radius:7px;overflow:hidden}.bar div{height:100%;background:#2e7d32}
.muted{color:#7a7064}.bad{color:#c62828;font-weight:600}a{color:#1e5bd8}</style></head><body>
<h2>Snekagon – exaktní řešení otevření doprostřed</h2><div id="c">…</div>
<p class="muted"><a href="events">deník událostí</a> · <a href="state">stav (JSON)</a></p>
<script>
async function tick(){
  const s = await (await fetch('status')).json();
  const n = s.entries, p = s.counts.proven||0;
  const eta = s.eta_hours ? (s.eta_hours > 48 ? (s.eta_hours/24).toFixed(1)+' dní' : s.eta_hours.toFixed(1)+' h') : 'zatím nelze odhadnout';
  let h = `<p>Dokazuje se: po <b>${s.base}</b> (na tahu červený) vyhrává modrý.<br>
  Vyvráceno <b>${p}</b> z ${n} odpovědí červeného${s.rate_per_hour ? ` – tempo ${s.rate_per_hour.toFixed(1)} za hodinu` : ''}.</p>
  <div class="bar"><div style="width:${(100*p/n).toFixed(2)}%"></div></div>
  <p class="muted">Odhad do konce: ${eta}. Vyřešeno úloh: ${s.units_done}. Vláken: ${s.threads}.
  Připraveno (seřazení kandidátů alfa-betou): ${s.ranked_ahead}.</p>`;
  if (s.holds.length) h += `<p class="bad">Odpovědi červeného bez nalezené výhry modrého: ${s.holds.join(', ')}</p>`;
  h += '<h3>Workery</h3><table><tr><th>jméno</th><th>IP</th><th>vlákna</th><th>úloh</th><th>s/úlohu</th><th>připojen</th></tr>' +
    s.workers.map(w=>`<tr><td>${w.name}</td><td>${w.ip}</td><td>${w.threads}</td><td>${w.done}</td><td>${w.avg? w.avg.toFixed(1):''}</td><td>${w.minutes.toFixed(0)} min</td></tr>`).join('') + '</table>';
  h += '<h3>Právě se řeší</h3><table><tr><th>odpověď červeného</th><th>kandidát modrého</th><th>průběh</th><th>neúspěšní kandidáti</th></tr>' +
    s.active.map(a=>`<tr><td>${a.r2}</td><td>${a.cand} <span class="muted">(${a.ci+1}. ${a.revisit?'– opakovaně bez limitu':''}${a.phase==2&&!a.revisit?'– dořešení těžkých úloh':''})</span></td>
      <td>${a.done}/${a.total}${a.hard? ' <span class="muted">('+a.hard+' těžkých)</span>':''}</td>
      <td class="muted">${a.failed.map(f=>f[0]+' ✗ '+f[1]).join('<br>')}${a.suspects? '<br>podezřelí: '+a.suspects:''}</td></tr>`).join('') + '</table>';
  h += '<h3>Poslední vyvrácené</h3><table>' + s.recent.reverse().map(r=>`<tr><td>${r[0]}</td><td>→ ${r[1]}</td><td class="muted">${r[2]? r[2]+' neúspěšných kandidátů':''}</td></tr>`).join('') + '</table>';
  document.getElementById('c').innerHTML = h;
}
tick(); setInterval(tick, 10000);
</script></body></html>'''


class StatusHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/status'):
            body, ctype = json.dumps(status()).encode(), 'application/json'
        elif self.path.startswith('/state'):
            with lock:
                body, ctype = json.dumps(state).encode(), 'application/json'
        elif self.path.startswith('/table'):
            body, ctype = json.dumps(table()).encode(), 'application/json'
        elif self.path.startswith('/events'):
            try:
                lines = open(EVENTS).read().splitlines()[-400:]
            except OSError:
                lines = []
            body, ctype = '\n'.join(lines).encode(), 'text/plain; charset=utf-8'
        elif self.path == '/' or self.path.startswith('/index'):
            try:  # read on every request: the page can be updated without restarting the computation
                body = open(os.path.join(HERE, 'status.html'), 'rb').read()
            except OSError:
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
    threading.Thread(target=ranking_thread, daemon=True).start()
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

"""Collects the real positions behind the big grids of the video (chapters 10-12) into data/zoom_data.json:
the coordinator's list of red replies with the proven ones, the 40 refuted blue answers to the stubborn
reply, and move lists from the engine ('children' = symmetry-unique legal moves, best rated first).

    .venv/Scripts/python.exe zoom_data.py <coordinator state.json>
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'build', 'snek_w2.exe')
BASE = ['1:60', '4B:16,27,28,39']
STUBBORN = '3V:40,41,50'


def children(moves):
    out = subprocess.run([ENGINE, 'serve'], input='children %s\nquit\n' % ' '.join(moves), capture_output=True, text=True).stdout
    reply = json.loads(out.splitlines()[0])
    assert reply['ok'], (moves, reply)
    return [m['s'] for m in reply['moves']]


def main(state_path):
    state = json.load(open(state_path, encoding='utf-8'))
    assert state['base'] == BASE
    entries = state['entries']
    replies = [e['r2'] for e in entries]
    assert sorted(replies) == sorted(children(BASE)), 'coordinator list differs from the engine'
    proven = [[k, e['r2'], e['by'], round(e['secs'] / 3600, 1)] for k, e in enumerate(entries) if e['status'] == 'proven']
    hold = next(e for e in entries if e['r2'] == STUBBORN)
    first = proven[0]
    followups = children(BASE + [first[1], first[2]])
    blue_moves = children(BASE + [STUBBORN])
    tried = [t[0] for t in hold['failed']]
    data = {
        'replies': replies,
        'proven': proven,
        'stubborn_index': replies.index(STUBBORN),
        'followups': {'r2': first[1], 'by': first[2], 'moves': followups},
        'stubborn_tries': hold['failed'],  # [blue move, red refutation, value for blue: 0 draw, -1 loss]
        'stubborn_open': [m for m in blue_moves if m not in tried],
        'stubborn_total': len(blue_moves),
    }
    json.dump(data, open(os.path.join(HERE, 'data', 'zoom_data.json'), 'w', encoding='utf-8'), indent=1)
    print('replies', len(replies), 'proven', len(proven), 'stubborn at', data['stubborn_index'])
    print('follow-ups of #1:', len(followups), ' blue moves after the stubborn reply:', len(blue_moves),
          ' tried', len(tried), ' draws', sum(1 for t in hold['failed'] if t[2] == 0))


if __name__ == '__main__':
    main(sys.argv[1])

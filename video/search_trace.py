"""What a solver tries, for the 'rapidly changing moves' shots: every recorded state is a whole continuation
of the start position, to the end of the game, and a different one every time: each move is the engine's
favourite, second or third choice ('children'), weighted 50 / 30 / 20 %, so every line looks like a real game.
Shown in quick succession, the pieces that are not part of the start position keep changing, as the solver
keeps trying other lines.

    .venv/Scripts/python.exe search_trace.py      # writes data/search_traces.json
"""
import json
import os
import random
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, '..', 'build', 'snek_w2.exe')
ZOOM = json.load(open(os.path.join(HERE, 'data', 'zoom_data.json'), encoding='utf-8'))

STARTS = {
    'pilot': ['1:60', '4B:16,27,28,39', '4A:7,8,9,20', '4I:15,25,35,45'],  # chapter 9, the 11-billion pilot
    'job': ['1:60', '4B:16,27,28,39', ZOOM['followups']['r2'], ZOOM['followups']['by'], ZOOM['followups']['moves'][0]],
    # chapter 12, what each level of the proof has to search: one red reply, the opening, the whole game
    'stubborn': ['1:60', '4B:16,27,28,39', '3V:40,41,50'],
    'opening': ['1:60', '4B:16,27,28,39'],
    'whole': [],
}
LINES = 90                # one whole continuation per state
WEIGHTS = (0.5, 0.3, 0.2)  # chances of the engine's first, second and third choice


class Engine:
    def __init__(self):
        self.p = subprocess.Popen([ENGINE, 'serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def children(self, moves):
        self.p.stdin.write('children %s\n' % ' '.join(moves))
        self.p.stdin.flush()
        reply = json.loads(self.p.stdout.readline())
        assert reply['ok'], moves
        return reply

    def close(self):
        self.p.stdin.write('quit\n')
        self.p.stdin.flush()
        self.p.wait(timeout=10)


def trace(engine, start, rnd):
    states = []
    for _ in range(LINES):
        path = []
        while True:
            node = engine.children(start + path)
            if node['over']:
                break
            best = [m['s'] for m in node['moves']][:len(WEIGHTS)]
            path.append(rnd.choices(best, weights=WEIGHTS[:len(best)])[0])
        states.append(path)
    return states


def main():
    engine = Engine()
    out = {}
    for name, start in STARTS.items():
        states = trace(engine, start, random.Random(name))
        out[name] = {'start': start, 'states': states}
        print(name, len(states), 'states, deepest', max(len(s) for s in states), 'moves')
    engine.close()
    json.dump(out, open(os.path.join(HERE, 'data', 'search_traces.json'), 'w', encoding='utf-8'))


if __name__ == '__main__':
    main()

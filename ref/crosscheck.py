"""Play random games with the Python reference and compare the legal-move sets
and snake lengths with the C++ engine (`snek serve`) at every ply."""
import json
import random
import subprocess
import sys
import os

from snek_ref import State, move_str, longest

ENGINE = os.path.join(os.path.dirname(__file__), '..', 'build', 'snek.exe')


def main(games=30, seed=1):
    rng = random.Random(seed)
    eng = subprocess.Popen([ENGINE, 'serve', '1'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(cmd):
        eng.stdin.write(cmd + '\n')
        eng.stdin.flush()
        return json.loads(eng.stdout.readline())

    checked = 0
    for g in range(games):
        st = State()
        hist = []
        while not st.over():
            ref = sorted(move_str(m) for m in st.legal_moves())
            e = ask('set ' + ' '.join(hist))
            got = sorted(m['s'] for m in e['legal'])
            if ref != got:
                print('MISMATCH game', g, 'ply', st.ply)
                print(' only ref:', sorted(set(ref) - set(got))[:10])
                print(' only eng:', sorted(set(got) - set(ref))[:10])
                sys.exit(1)
            if e['longest'] != [longest(st.own[0]), longest(st.own[1])]:
                print('LENGTH MISMATCH', g, st.ply)
                sys.exit(1)
            checked += 1
            moves = st.legal_moves()
            mv = rng.choice(moves) if moves else None
            st.play(mv)
            hist.append(move_str(mv))
        print(f'game {g}: ok ({st.ply} plies, lengths {longest(st.own[0])}-{longest(st.own[1])})')
    print(f'all {checked} positions match')
    ask('quit') if False else None
    eng.stdin.close()


if __name__ == '__main__':
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 30)

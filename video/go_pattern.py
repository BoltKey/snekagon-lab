"""The Go game at the end of chapter 13: AlphaGo vs Lee Sedol, game 4 (Seoul, 13 March 2016), up to Lee Sedol's
move 78. Reads the game record (SGF, e.g. LeeSedol/4.sgf from homepages.cwi.nl/~aeb/go/games/games/AlphaGo/),
plays it with captures and writes the stones still on the board after move 78, in the order they were played,
to data/go_pattern.json.

    .venv/Scripts/python.exe -I go_pattern.py <game record .sgf>
"""
import json
import os
import re
import sys

N = 19
LAST = 78  # Lee Sedol's move 78 at L11
HERE = os.path.dirname(os.path.abspath(__file__))


def neighbours(p):
    x, y = p
    return [(a, b) for a, b in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)) if 0 <= a < N and 0 <= b < N]


def group(board, p):
    colour, seen, todo, libs = board[p], {p}, [p], set()
    while todo:
        q = todo.pop()
        for n in neighbours(q):
            if n not in board:
                libs.add(n)
            elif board[n] == colour and n not in seen:
                seen.add(n)
                todo.append(n)
    return seen, libs


def play(board, p, colour):
    """Places a stone and removes the opponent's groups left without liberties; returns the captured stones."""
    board[p] = colour
    captured = []
    for n in neighbours(p):
        if board.get(n) == 1 - colour:
            stones, libs = group(board, n)
            if not libs:
                captured += list(stones)
                for s in stones:
                    del board[s]
    return captured


def main():
    text = open(sys.argv[1], encoding='utf-8', errors='replace').read()
    moves = re.findall(r';([BW])\[([a-s]{2})\]', text)[:LAST]
    board, order = {}, []
    for colour, (a, b) in moves:
        p = (ord(a) - 97, ord(b) - 97)  # SGF: column from the left, row from the top
        captured = play(board, p, 0 if colour == 'B' else 1)
        order = [o for o in order if (o[0], o[1]) not in captured]
        order.append([p[0], p[1], 0 if colour == 'B' else 1])
    json.dump({'size': N, 'game': 'AlphaGo vs Lee Sedol, game 4, 2016', 'last': order[-1], 'moves': len(moves), 'stones': order},
              open(os.path.join(HERE, 'data', 'go_pattern.json'), 'w'))
    print('moves', len(moves), 'stones on the board', len(order), 'last', order[-1])


if __name__ == '__main__':
    main()

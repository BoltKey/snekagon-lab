# Snekagon Lab

Engine, exact solver and a play/analysis UI for **Snekagon** (2-player abstract snake-building game).

## Quick start

```bash
# build (g++ 13, static so the exe runs without MinGW DLLs on PATH)
g++ -O3 -march=native -std=c++20 -pthread -static -o build/snek.exe src/main.cpp

# UI: http://localhost:8080  (second arg = engine threads)
node ui/server.js 8080 8
```

UI: pick a piece (buttons or keys `1`–`8`), hover a cell, and cycle through all legal placements of that
piece covering the cell with the mouse wheel / `R` / right click; click to play. "Engine táhni" = engine move,
"Analyzuj" = ranked list of moves (hover = preview on board, click = play). Checkboxes let the engine play
either side. Click a move in the history to jump back.

## Rules as implemented (see [RULES.md](RULES.md))

## Layout

| Path | What |
|---|---|
| `src/core.hpp` | board (radius-5 hexagon, 91 cells, padded 11×11 → 128-bit bitboards), pieces, legality, Zobrist hashing incl. the 12 board symmetries |
| `src/solver.hpp` | exact win/draw/loss solver: negamax α-β, TT on the symmetry-canonical key, snake-length bound cutoffs, TT-move + history ordering |
| `src/mcts.hpp` | UCT MCTS with guided random playouts, root-parallel over threads, symmetric root moves deduplicated |
| `src/main.cpp` | CLI: `serve` (UI protocol), `perft`, `solvebench`, `selfplay`, `info` |
| `ref/snek_ref.py` | independent naive Python implementation of the rules |
| `ref/crosscheck.py` | plays random games in Python and compares legal-move sets + lengths with the engine at every ply |
| `ui/` | dependency-free Node server + single-page UI |
| `results/` | benchmark outputs |

### Engine protocol (`snek serve [threads]`, one line in → one JSON line out)

- `new`, `state`, `undo`
- `set <m1> <m2> …` – position from the start; `play <m> …` – append moves
- move notation: `pass` or `<piece>:<cell>,<cell>,…`, cell = `(r+5)*11 + (q+5)` (axial q,r; centre = 60). Pieces: `1 2 3I 3V 4I 4A 4B 4C`.
- `analyze <sec>` – first tries to solve every root move exactly (if ≤ 400 moves, within half the budget), otherwise MCTS.
- `go <sec>` – analyze and play the best move.

## Verification

- Placement counts on the empty board match the independent Python enumeration (2602 placements total).
- `ref/crosscheck.py 25`: legal-move sets and longest-snake lengths identical to the Python reference in all 398 positions of 25 random games.
- `perft`: 1 → 91, 2 → 194 388, 3 → 315 706 848.

## Solving – first measurements

`snek solvebench <plies> <n> <timeout> <threads>`: play `<plies>` uniformly random moves from the start (the
1-hex counts as ply 0… i.e. `plies=5` means the 1-hex + 4 real moves are on the board), then solve exactly.
Single-threaded solver per position, ~0.5–0.9 M nodes/s.

| plies already played | typical solve time | nodes |
|---|---|---|
| 11 | < 1 ms | 10–100 |
| 10 | ~1 ms | 10²–10³ |
| 9 | ~10 ms | 10³–10⁴ |
| 8 | 5–75 ms | 10³–6·10⁴ |
| 7 | 0.2–2 s | 10⁵–2·10⁶ |
| 6 | 1–90 s (1 of 12 > 120 s) | 6·10⁵–10⁸ |
| 5 | 30 s → many > 15 min | 2·10⁷ – ? |

Growth is roughly ×10–×30 per ply towards the start. Straight extrapolation to the empty board (5 more plies)
is far out of reach **with the current solver** – solving the game needs algorithmic gains, not just CPU.
Random positions are also not typical positions (random play leaves lots of scattered snakes).

Observations: random positions at plies 6–11 are overwhelmingly wins for the side to move.

## Next steps (ideas, roughly by expected payoff)

1. **Better bounds.** The upper bound (sum of growable components + remaining cells) is weak early. Tighter:
   count only pieces that still fit somewhere, reachable free space per snake end (flood fill of the free area
   around ends), and whether components can actually be joined.
2. **Null-window / proof-number search.** Solve "does P1 win?" and "does P1 at least draw?" separately
   (two binary searches) – df-pn handles the huge, uneven branching much better than α-β.
3. **Speed.** Incremental move generation (per-cell placement lists, only re-check placements near the last
   move), cheaper bounds (incremental components). 5–10× looks realistic.
4. **Move ordering** from MCTS priors / killer moves; enhanced transposition cutoffs.
5. **Region decomposition** in the endgame: once free space splits into independent regions, solve them
   separately.
6. **Engine strength:** MCTS playouts are light (random, prefer growing an own snake). A heuristic playout
   policy or a small learned value network would improve play a lot; the exact solver already takes over
   when ~7–8 plies are played.
7. **Parallel solving** of the 12 symmetry-distinct first moves (and their replies) across cores/machines.

# Snekagon Lab

Engine, exact solver and a play/analysis UI for **Snekagon** (2-player abstract snake-building game).

## Quick start

```bash
# build (g++ 13, static so the exe runs without MinGW DLLs on PATH)
g++ -O3 -march=native -std=c++20 -pthread -static -o build/snek.exe src/main.cpp

# UI: http://localhost:8080  (second arg = engine threads)
node ui/server.js 8080 10
```

The browser UI only draws; every computation runs in the C++ engine (`ui/server.js` starts `snek.exe serve`
and forwards each request to its stdin, one JSON line back per command).

UI: pick a piece (buttons or keys `1`–`8`), hover a cell, and cycle through all legal placements of that
piece covering the cell with the mouse wheel / `R` / right click; click to play.
- **Engine táhni** – engine move (engine selectable: *alfa-beta* = current engine, *MCTS* = first version).
- **Analyzuj** – ranked moves with win probability for the side to move (hover = preview, click = play).
- **Vyřešit** – exact parallel solve of the current position (win/draw/loss per move) within a time limit.
- **živé hodnocení** – every shown position (also while browsing the game) gets a 1 s analysis, cached per
  position, so going back and forth is instant; "Analyzuj" with a longer time replaces the cached one.
- game navigation: ⏮ ◀ ▶ ⏭, arrow keys, Home/End, or click a move in the history. The whole game is kept;
  playing a different move in an earlier position starts a new branch.
- checkboxes let the engine play either side.

## Rules as implemented: [RULES.md](RULES.md)

## Engine (v2)

| Component | What it does |
|---|---|
| move generation (`core.hpp`) | for each of the 40 piece orientations the legality of **all anchor cells at once** is an AND/OR of shifted 128-bit boards (no-overlap, isolation for new snakes, own-degree ≤ 2, no sharp turns, touching own). ~2.6 µs for a full move list, perft 3 in 0.95 s (4.3× faster than v1). |
| length bounds (`snakeBounds`) | **sound** lower/upper bound of each player's final longest snake: a player can never again occupy a cell next to its own snake body, so the final snake lies in one cluster of *own cells ∪ growable cells*; new cells ≤ subset-sum fit of the remaining pieces into the growable regions. Detects enclosed snakes immediately. |
| evaluation (`eval.hpp`) | features from a Tron-style Voronoi split (who reaches which cell first from its growable snake ends), per-snake territory, bounds, number of snakes…; **logistic regression fitted on ~68 000 positions whose exact result was computed by the solver** (`tools/fit_eval.py`). Hold-out: predicts the winner in 87–90 % of decisive positions (hand-made eval: 72 %). |
| search (`search.hpp`) | iterative-deepening PVS α-β, TT on symmetry-canonical keys, killers/history, forward pruning (best 24 by 1-ply eval at depth ≥ 2), bound cutoffs, the exact solver once ≤ 6 pieces remain; parallel over root moves with multi-PV. Depth 4–5 in 3 s early in the game. |
| exact solver (`solver.hpp`) | negamax over {loss, draw, win}, TT, bound cutoffs, TT/killer/history ordering + learned-eval ordering when ≥ 7 pieces remain; `parallelSolve` splits root moves over threads. |

**Strength:** `snek arena` (random opening = random 1-hex + random reply, each opening played with both
colours, 1 s/move):

| match | result |
|---|---|
| α-β with the hand-made eval vs MCTS (v1) | 2 : 10 |
| α-β with the learned eval vs MCTS (v1) | **16 : 0** (weights v1), **7 : 0, 1 draw** (current weights) |

The hand-made eval lost because it treated all own tiles in one region as if they would be joined into one
snake – the engine scattered pieces. The fitted weights confirm it: *number of separate snakes* has a large
negative weight, *best single snake + its own territory* and the *sound upper bound* are the strongest positive
features, and the side to move has a big tempo advantage.

### Side to move, "jumping" evaluation

The evaluation used to swing from ~90 % to ~10 % between consecutive plies. Diagnosis (`snek evaltrace`,
`snek evalexplain`):

1. **Tempo in the static evaluation.** A linear model cannot measure what a move just gained, so it carries the
   value of having the move in explicit terms (bias for the side to move, `stmIsP1`, remaining-cells difference):
   ≈ +0.6 … +1 logit for whoever is to move. Part of it is real (even in good play the side to move wins 57 %),
   part was an artefact of the first training data (random moves → the move before a position was often a
   blunder → side to move won 69 %). Data from (almost) greedy play reduced the tempo terms.
2. **Odd/even search depth.** Leaves at even and odd depth have the opposite side to move, so the root value
   flipped with the depth reached. The displayed value of the position is now the average of the best
   scores of the last two iterations.
3. **Opening out of the training range** (labels only from ply 5–6 on). Large open territories there
   make the Voronoi head start of the side to move worth dozens of cells; player 2 before its first move had
   "no snake" features (fixed). A per-ply calibration on 245 exactly solved ply-5 positions found *no*
   over-confidence at ply 5 (best scale 1.18), so no shrinking is applied (`EVAL_PLY_SCALE` = 1).
4. A real bug found on the way: forward pruning could report a **false forced win** (all searched replies
   lost, but a pruned one saved the game). Fixed.

Result on the self-play game used for diagnosis (red's win %, what the UI shows after 3 s vs exact):
ply 4 66 % (red wins), ply 5 51 % (draw; before: a false "forced blue win"), ply 6+ 100 % (red wins).
**Remaining:** in the opening (plies 1–4) the value still moves by about ±15 points depending on who is to
move – fixing that needs a richer evaluation (e.g. a small network on the board) and opening labels.

Training data now: 68k solver-labelled positions (`data/labels_*.csv`, newer files also store the position,
so `snek refeature` can recompute features without re-solving).

## Solving – measurements

`snek solvebench <plies> <n> <timeout> <threads>` solves random positions after `<plies>` random moves
(ply 0 = the 1-hex). Nodes on the **same positions**, v1 → v2 solver:

| plies played | v1 (nodes, all 12 positions) | v2 |
|---|---|---|
| 7 | 5.5 M | 0.94 M |
| 6 | > 299 M (1 timeout) | 60 M |
| 5 (8 pos.) | 1.75 G, median ~7 min | **162 M, median ~18 s** (same values) |

Positions from **good play** (`snek solvepos`, engine self-play game in `results/selfplay_*`, 6–14 threads):

| position | result | cost |
|---|---|---|
| ply 6 (red to move) | red wins | 1.6 M nodes, 0.7 s |
| ply 5 (blue to move) | **draw** – all 336 moves solved; the engine's actual move there loses | 173 M nodes, 73 s (6 threads) |
| ply 4 (red to move) | **red wins** with `4B:93,94,102,103` (of 38 moves solved: 1 win, 4 draws, 33 losses); the engine played a drawing move | 1.2 G nodes, 157 s (14 threads) |

### Is a full solution feasible?

The cost of a proof is dominated by the side that has to answer *every* move. If red (P1) wins, the proof
needs one good red move at plies 0, 2, 4 and all of blue's replies at plies 1 and 3: about 180 (ply 1, after
symmetry) × ~940 (ply 3) ≈ 1.7·10⁵ ply-4 positions. The one ply-4 position measured took ~37 core-minutes
→ order of **10⁵ core-hours** for the whole proof with today's solver (less with shared TT, transpositions
and the many blue moves that are easy to refute; more if the game is a draw, since then both "red cannot
win" and "blue cannot win" have to be proven).

So: not a laptop job, but within reach of engineering + a rented cluster (10⁴–10⁵ core-hours). Path:
1. strengthen the engine (more solver-labelled data, richer eval) so the principal line is right,
2. another ~10× in the solver: proof-number search (df-pn) at the top, shared TT, tighter bounds
   (reachable space per snake end, geometric packing), endgame decomposition into independent regions,
3. prove bottom-up along the principal line: solve ply-4 positions in bulk as independent jobs, store results,
4. distribute – every (first move, blue reply) pair is an independent job.

## Tools

| Command | Purpose |
|---|---|
| `snek serve [threads]` | UI protocol (below) |
| `snek arena <pairs> <sec> <threads> <A> <B> [seed]` | engine match; engines `ab`, `abhand`, `mcts`, `ab:<weights.txt>` |
| `snek gendata <games> <plyMin> <plyMax> <solveSec> <threads> <seed>` | labelled positions (features + exact value) as CSV |
| `python tools/fit_eval.py data/*.csv` | fit the evaluation → `src/eval_weights.hpp`, `data/weights_latest.txt` |
| `snek solvepos <sec> <threads> <moves…>` | parallel exact solve of a position |
| `snek solvebench <plies> <n> <sec> <threads>` | solver benchmark on random positions |
| `snek selfplay <games> <sec> <threads>` | engine vs itself, prints the game |
| `snek verify [games]` | fast move generator == reference checker, bounds never violated |
| `snek perft <d>`, `snek speed`, `snek info` | counting / micro-benchmarks |
| `python ref/crosscheck.py [games]` | engine vs independent Python rules implementation |

### Engine protocol (`snek serve [threads]`, one line in → one JSON line out)

- `new`, `state`, `undo`, `set <m1> <m2> …` (position from the start), `play <m> …` (append moves)
- move notation: `pass` or `<piece>:<cell>,<cell>,…`, cell = `(r+5)*11 + (q+5)` (axial q,r; centre = 60).
  Pieces: `1 2 3I 3V 4I 4A 4B 4C`.
- `analyze <sec>` / `go <sec>` – ranked moves (score = 100·logit of the win probability, ±100000 = proven),
  `go` also plays the best move
- `solve <sec>` – parallel exact solve
- `engine ab|mcts`

## Verification

- Placement counts match the independent Python enumeration (2602 placements).
- `ref/crosscheck.py`: legal-move sets and snake lengths identical to the Python reference (398 positions).
- `snek verify 3000`: bit-parallel generator gives the identical move list (same order) as the per-placement
  checker in 47 131 positions; the length bounds held for the final result in all 94 262 checks.
- Solver v2 gives the same values as v1 on all benchmark positions.
- `perft`: 1 → 91, 2 → 194 388, 3 → 315 706 848.

## Layout

| Path | What |
|---|---|
| `src/core.hpp` | geometry, pieces, bit-parallel move generation, bounds, notation |
| `src/eval.hpp`, `src/eval_weights.hpp` | features, learned + hand-made evaluation |
| `src/search.hpp` | α-β playing engine |
| `src/solver.hpp` | exact solver, parallel solve |
| `src/mcts.hpp` | v1 MCTS engine (kept for comparison) |
| `src/main.cpp` | CLI + UI protocol |
| `ref/` | independent Python reference implementation + cross-check |
| `tools/fit_eval.py` | evaluation fitting |
| `data/` | solver-labelled positions (`labels_v*.csv`), weight files |
| `ui/` | Node server + single-page UI |
| `results/` | benchmark / arena / solve logs |

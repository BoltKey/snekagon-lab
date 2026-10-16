# Snekagon Lab

Engine, exact solver and a play/analysis UI for **Snekagon** (2-player abstract snake-building game).

## Quick start

```bash
# build (g++ 13, static so the exe runs without MinGW DLLs on PATH)
g++ -O3 -march=native -std=c++20 -pthread -static -o build/snek.exe src/main.cpp -lws2_32   # Windows (MinGW)
bash dist/build.sh                                                                    # Linux

# UI: http://localhost:8080  (second arg = engine threads)
node ui/server.js 8080 10
```

The browser UI only draws; every computation runs in the C++ engine (`ui/server.js` starts `snek.exe serve`
and forwards each request to its stdin, one JSON line back per command).

UI: pick a piece (buttons or keys `1`–`8`), hover a cell, and cycle through all legal placements of that
piece covering the cell with the mouse wheel / `R` / right click; click to play.
- **Engine move** – the engine chosen under *Engine* plays: *alpha-beta* (`ab`, the default) or *network* (`nn`:
  alpha-beta with a small neural network in its evaluation and move order, which also proves the root moves
  once at most 10 pieces are left). Analyze and the live evaluation use it too. While the engine plays both
  colours, *Engine* becomes one choice per colour, so the two engines can play each other; each move and the
  evaluation of each position then come from the engine to move. The first engine, MCTS, is still there in
  the protocol: `engine mcts`.
- **Analyze** – ranked moves with win probability for the side to move (hover = preview, click = play).
- **Solve** – exact parallel solve of the current position (win/draw/loss per move) within a time limit, with a
  progress bar of the solved moves. Offered once few pieces are left (from move 7 in the browser, move 6 with the
  server): earlier positions can take minutes.
- **live evaluation** – every shown position (also while browsing the game) gets a 1 s analysis, cached per
  position, so going back and forth is instant; "Analyze" with a longer time replaces the cached one.
- game navigation: ⏮ ◀ ▶ ⏭, arrow keys, Home/End, or click a move in the history. The whole game is kept;
  playing a different move in an earlier position starts a new branch.
- checkboxes let the engine play either side.
- **Exact verification** – exact solve of the shown position as a separate background process (the UI stays
  responsive): progress, ETA, verdict with what it implies for the last two moves, winning/drawing moves.
  With "automatically after move 4" it starts by itself once the 4th move is played. Jobs are resumable and
  cached in `results/jobs/` (`<id>.moves`, progress `<id>.log`, output `<id>.out`).
- `?moves=1:60+4B:16,27,28,39+…` in the URL opens that position.

Server: `node ui/server.js <port> <engineThreads> <jobThreads>` (default job threads = cores − 4).

## Browser version (no server, GitHub Pages)

The same UI also runs as a static site: `src/web.cpp` compiles the engine to WebAssembly (`bash web/build.sh`,
needs Emscripten; output in `web/site/`), `ui/snek-worker.js` runs it in a Web Worker, and `ui/index.html`
switches to it whenever `ui/server.js` is not there. In the browser the engine thinks on a single core (the
network with its scalar code) and the exact verification jobs are hidden.
`.github/workflows/pages.yml` builds it and publishes it on GitHub Pages on every push to `main`. Both front
ends share the protocol in `src/serve.hpp`.

## Rules as implemented: [RULES.md](RULES.md)

## Engine (v2)

| Component | What it does |
|---|---|
| move generation (`core.hpp`) | for each of the 40 piece orientations the legality of **all anchor cells at once** is an AND/OR of shifted 128-bit boards (no-overlap, isolation for new snakes, own-degree ≤ 2, no sharp turns, touching own). ~2.6 µs for a full move list, perft 3 in 0.95 s (4.3× faster than v1). |
| length bounds (`snakeBounds`) | **sound** lower/upper bound of each player's final longest snake: a player can never again occupy a cell next to its own snake body, so the final snake lies in one cluster of *own cells ∪ growable cells*; new cells ≤ subset-sum fit of the remaining pieces into the growable regions; with k pieces left at most the k + 1 longest snakes of a cluster can end up in one final snake (old snakes and new pieces alternate along it). Detects enclosed snakes immediately. |
| evaluation (`eval.hpp`) | features from a Tron-style Voronoi split (who reaches which cell first from its growable snake ends), per-snake territory, bounds, number of snakes…; **logistic regression fitted on ~68 000 positions whose exact result was computed by the solver** (`tools/fit_eval.py`). Hold-out: predicts the winner in 87–90 % of decisive positions (hand-made eval: 72 %). |
| search (`search.hpp`) | iterative-deepening PVS α-β, TT on symmetry-canonical keys, killers/history, forward pruning (best 24 by 1-ply eval at depth ≥ 2), bound cutoffs, the exact solver once ≤ 6 pieces remain; parallel over root moves with multi-PV. Depth 4–5 in 3 s early in the game. |
| network engine `nn` (`search.hpp`, `nn.hpp`) | the same search, using more of its move time, with exact verdicts on the root moves once ≤ 10 pieces remain (null-window win and draw proofs), the mean of the logistic evaluation and a small efficiently updatable network (NNUE) trained on self-play as its evaluation, and the network's move ranking (policy) choosing which moves it evaluates: see [Network engine](#network-engine-nn). `ab` stays the default; not in the WebAssembly build. |
| exact solver (`solver.hpp`) | negamax over {loss, draw, win}, TT, bound cutoffs, **threat zones**, an exact closed form for the last two pieces, **tight bounds** near the end, TT/killer/static ordering + learned-eval ordering when ≥ 7 pieces remain; `solve()` takes a window, so "does the side to move win?" is a cheaper null-window question; `parallelSolve` splits root moves over threads. |
| threat zones (`Solver::scanThreats`) | an opponent placement *b* is a threat when, played as a free move, it makes the bounds decide the game. Any move that leaves *b* playable (does not cover it, nor touch it when *b* starts a new snake) lets the opponent answer with *b*, and the bounds only tighten from there, so such moves are worth at most that result: only moves through the zone of every threat are searched (the "must-play" region of Hex solvers). Threats come from the killer refutations one ply down and, once a node's first move fails, from a scan of all opponent placements, filtered cluster by cluster. |
| one-player optimum (`OnePlayer`, `core.hpp`) | could a player still make a snake of *k* cells if the opponent never moved again? A depth-first search over induced paths and rings of whole old snakes and whole new pieces, each piece attached at an end of the path built so far, cluster by cluster with a memo. It drops the opponent's future cells, the move order and the touch-nothing rule, so a "no" is a sound upper bound, and it follows how the fixed piece shapes chain, which the cluster bound does not. |
| tight bounds (`Solver::tightenBounds`) | lower bounds one and two plies ahead: the side to move's best immediate length, and the length the opponent reaches whatever single move the side to move makes. Then only the one-player questions whose answer ends the node in its window are asked. Used at every node once ≤ 5 pieces are left in total, and with 6 left once a node's first move failed, over the moves still to search; killer threats use the optimum as well. |

**Strength:** `snek arena` (random opening = random 1-hex + random reply, each opening played with both
colours, 1 s/move):

| match | result |
|---|---|
| α-β with the hand-made eval vs MCTS (v1) | 2 : 10 |
| α-β with the learned eval vs MCTS (v1) | **16 : 0** (weights v1), **7 : 0, 1 draw** (current weights) |
| `nn` vs `ab` (one thread each, balanced openings, see [Network engine](#network-engine-nn)) | **+205 Elo** [+185, +226], 1 200 games |

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
**Remaining:** in the opening (plies 1–4) the logistic evaluation's value still moves by about ±15 points
depending on who is to move. Exact labels there are too expensive, so the network evaluation below learns
plies 2–4 from search scores and game results instead.

Training data now: 68k solver-labelled positions (`data/labels_*.csv`, newer files also store the position,
so `snek refeature` can recompute features without re-solving).

### Network engine (`nn`)

`nn` is `ab`'s search with four changes, each measured against the engine without it (below). It is chosen
by name: `engine nn` in the UI and `snek serve`, `nn` in `snek arena`; `ab` stays the default. Its network
(`src/nn.hpp`; weights `data/nn/nn_g2.bin`, compiled in through `src/nn_net.hpp`) and move ranking
(`data/nn/nn_g2_policy.bin`, through `src/nn_policy.hpp`) are compiled into every build, the browser's too.
`nn:<net file>` evaluates by another network and without a policy (a policy reads the first layer
it was trained on), and `NN_POLICY=<file>` replaces the compiled-in policy.

- **Time use**: no iteration starts after 60 % of the move time (`ab`: 35 %), and an iteration that the
  deadline cuts off still ranks the root moves it finished (the previous best is always among them) before
  the others.
- **Exact root verdicts**: once at most 10 pieces are left, null-window proofs of the root moves in order:
  "does it win" for each, then "does it draw" once none wins, stopping at the first win (or draw). From 9
  pieces left a first exact phase in 1-ply order gets half the move time; then the heuristic search; then
  the exact phase again on candidates alternating between the search's and the 1-ply order. The final order
  is a proven win, the proofs still running (furthest first), a proven draw, the rest, proven losses. The
  exact solvers share one 128 MB table kept between moves. In `serve` and the arena, `nn` does not first try
  to solve all root moves as `ab` does.
- **Network**: a perspective NNUE. 937 inputs: four 91-cell planes per player (snake body, other own cells,
  empty cells next to an own end, empty cells next to the body, which the player can never use), the empty
  cells next to no tile, the pieces left, and one-hot each player's longest snake and `snakeBounds` upper bound.
  They feed 128 int16 units (AVX2, or a scalar path with the same results), then SCReLU and one output per ply:
  the logit of P(side to move wins), draws ½. The evaluation, for move ordering and leaves alike, is the mean
  of the network's value and the logistic evaluation's.
- **Incremental**: the search keeps an accumulator pair per ply and updates it from the parent by the inputs a
  move changes. Playing a move and updating the network costs about 0.55 µs, playing it and running the
  logistic evaluation about 1 µs (400 k children of random positions after 2–9 plies, one thread, with other
  work on 6 of the 16 hardware threads). `snek nnverify` checks the incremental path against a full refresh.
- **Lazy mean**: a leaf that the network alone puts more than 1 logit below alpha keeps the network's value
  (the mean could reach alpha only if the logistic evaluation disagreed by more than 2 logits) and skips the
  logistic evaluation.
- **Move ranking (policy)**: a logit per placement for the side to move, from the same accumulators: SCReLU of
  both perspectives times the placement's 256 int8 weights, plus a bias and, per piece, weights for the
  number of the placement's cells and neighbours in each of the side to move's input planes. It is computed
  for the legal moves only, about 45 ns per move against about 1.6 µs for playing a move and evaluating it by
  the mean. `ab` plays and evaluates every child of a node with depth ≥ 2 (about 280 on average) to keep the
  best 24; `nn` evaluates only the policy's best 48 and the table move, sorts them by the evaluation plus the
  policy's logit (one logit of the evaluation per nat), and the other moves follow in the policy's order.
  Nodes with depth 1 keep their cheap order (table move, killers, history).

**Strength** (`snek arena`, one thread per engine, each opening played with both colours, fresh books as in
the test protocol below; Elo of the first engine with the 95 % interval of `tools/arena_stats.py`, which takes
the two games of an opening as one sample):

| match | s/move | games | Elo | pairs split 1–1 |
|---|---|---|---|---|
| `nn` vs `ab` | 1 | 1 200 | **+205 [+185, +226]** | 39 % |
| `nn` vs `nn` without the policy¹ | 1 | 2 030 | **+36.8 [+24.9, +48.7]** | 66 % |

The parts, each against the engine without it, on books from which only exact repeats of earlier books'
lines were dropped (up to symmetry, 13–15 % of their openings are openings of `data/book2.txt`):

| match | s/move | games | Elo | pairs split 1–1 |
|---|---|---|---|---|
| `ab` + time use² vs `ab` | 1 | 600 | +105 [+79, +132] | 40 % |
| + exact root verdicts, vs `ab` + time use | 1 | 600 | +27 [+9, +45] | 73 % |
| + network (`nn` without the policy¹), vs `ab` + time use + verdicts | 1 | 2 160 | +18 [+8, +29] | 72 % |
| the same | 0.3 | 2 760 | +27 [+17, +37] | 67 % |
| + policy (`nn`), vs `nn` without the policy¹ | 1 | 2 400 | +28.1 [+17.9, +38.4] | 71 % |
| the same | 0.3 | 5 610 | +7.9 [+1.6, +14.3]³ | 73 % |
| `ab` + time use + verdicts vs `ab` | 1 | 600 | +110 [+84, +136] | 50 % |
| `nn` without the policy¹ vs `ab` | 1 | 600 | +155 [+130, +182] | 45 % |
| `nn` without the policy¹ but the network as the whole evaluation, vs `ab` + time use + verdicts | 0.3 | 600 | −56 [−80, −33] | 60 % |

¹ `nn:data/nn/nn_g2.bin`: the same network, loaded from its file, so without a policy.
² Still solving all root moves first, as `ab` does.
³ An SPRT (elo0 0, elo1 10, α 0.05, β 0.1) over three books accepted the gain of 10 Elo at its check after
5 556 games (LLR 3.45 ≥ 2.89); over all 5 610 games of the row the LLR is 2.81.

What this shows, and what it does not:
- The largest part of `nn`'s lead over `ab` (+205) is its use of the move time (+105 Elo against
  `ab`); the exact root verdicts add +27 [+9, +45], the network +18 [+8, +29] at 1 s/move (+27 [+17, +37] at
  0.3 s), the policy +28.1 [+17.9, +38.4] at 1 s/move (+36.8 [+24.9, +48.7] in the match on fresh openings above;
  +7.9 [+1.6, +14.3] at 0.3 s). Each was measured against its predecessor, not in one match against `ab`, so
  the steps do not have to add up to `nn` vs `ab`.
- The network gains little on its own (a score of 53–54 % against the engine without it), and loses when it
  is the whole evaluation; averaged with the logistic evaluation it wins. The policy gains more with more
  time: at 0.3 s the last iteration the search reaches is about as deep with it as without (2.78 against
  2.77 on average), at 1 s it is deeper (3.22 against 2.78; see the speed below).
- Offline, on the held-out games of its training data and their exact labels, `nn_g2`'s log-loss beats the
  logistic evaluation's clearly only at plies 7–11 (0.225 against 0.291): positions with at most 8 pieces left,
  which `nn` already tries to prove exactly. At plies 5–6 the two are level (0.210 against 0.216; the mean that
  `nn` uses: 0.199), and at ply 5 alone the network is worse (0.383 against 0.294, 183 positions).
- The opening decides most games: in 39–73 % of the pairs the same colour won both games, so
  differences of a few Elo need thousands of games.

**Test protocol.** Openings from `snek book`: two random plies, kept when `nn` gives the side to move 30–70 %
after 0.25 s. `data/book2.txt` holds the openings the time use and the exact verdicts were tuned on. A test
book drops every opening whose position equals, under one of the 12 rotations and reflections of the board,
that of a line of `data/book2.txt`, of an earlier book or of an earlier line of its own (two openings are equal
when each player's cells are). Matches run as one-thread processes over slices of the book, and
`tools/arena_stats.py` pools their logs:

```bash
build/snek.exe book build/fresh.txt 700 8 0.25 <seed> 2 0.3 0.7      # then drop the repeats, up to symmetry
for k in 0 1 2 3 4 5; do
  ARENA_BOOK=build/fresh.txt ARENA_BOOK_START=$((50 * k)) build/snek.exe arena 50 1 1 nn ab $((7000 + k)) > build/arena_$k.txt &
done; wait
python tools/arena_stats.py build/arena_*.txt
```

**Training.** `snek datagen` plays `nn` (or `nn:<net file>`, or `ab`) against itself from a random 1-hex and,
half the time, a random reply, with random or sampled moves at plies 1–5. Each position is labelled with the
engine's proof where it found one, else by an exact solve under a time limit at ply ≥ 6 and in 10 % of the
games at ply 5; a solve that runs out of time is kept as censored, never dropped. Sibling moves and positions
from the search tree are added for a ranking loss. `tools/train_nn.py` (numpy) trains on the exact value where
known, else ½ · sigmoid(search score) + ½ · game result, with a RankNet loss over siblings and one of the 12
hex symmetries per batch; the games whose id hashes to 0 mod 10 are held out and reported by ply. `nn_g2` was
trained on 725 k such positions, 56 % of them exactly labelled, from several runs (played by `nn`'s search
with the logistic evaluation in place of the network, some scored by searches with earlier networks) whose
exact mix is not recorded: the commands below make a comparable network, not `nn_g2` itself. The data is not
in the repository; at 0.05 s per move a game with its labels takes about 3 thread-seconds. To make a network
and test it:

```bash
build/snek.exe datagen build/nn/d1.csv 10000 8 0.05 1          # games, threads, s/move, seed; appends
python tools/train_nn.py --snek build/snek.exe --data build/nn/d1.csv --out build/nn/net.bin
build/snek.exe arena 300 0.3 1 nn:build/nn/net.bin nn:data/nn/nn_g2.bin   # better use a book, as above
python tools/train_nn.py --embed build/nn/net.bin src/nn_net.hpp   # the compiled-in network
```

**Training the move ranking.** `snek policygen` plays an engine (`nn` by default; below `nn` without a policy,
as for `nn_g2_policy`) against itself as `datagen` does and records for every position the root moves with an
exact search score, or the proven wins (once the search proved a win at ply ≥ 6, every other move also gets a
win test of 5 000 nodes) or proven draws. `tools/policy_rows.py` makes rows of the same layout from the proofs
of `snek refute` logs and `snek proveline` summaries (the defender's refuting replies, the moves that hold or
win, `proveline`'s winning answers) and from the best moves of `snek datagen` games. `tools/train_policy.py`
(numpy) trains the policy with the network's first layer frozen, so the evaluation stays exactly as it is:
softmax over the legal moves, cross-entropy against a uniform target over the proven wins, else over the proven
draws, else softmax(score / 40) of the scored moves, one of the 12 symmetries per batch. Held out: the games
that `train_nn.py` holds out, and the proof rows of three lines of the centre opening, every proof position
that contains blue's reply `4B:16,27,28,39`, `3V:36,37,48` or `4A:18,19,30,41` to `1:60` up to symmetry (game
positions in those lines follow their game); training rows that equal a test position up to symmetry are
dropped. Its data and results are in [Move ranking: data and results](#move-ranking-data-and-results).

```bash
build/snek.exe policygen build/nn/p1.csv 6000 6 0.15 101 nn:data/nn/nn_g2.bin   # games, threads, s/move, seed, engine; appends
python tools/policy_rows.py proofs build/nn/proofs.csv <refute logs, proveline directories…>
python tools/policy_rows.py datagen build/nn/best.csv build/nn/d1.csv
python tools/train_policy.py --snek build/snek.exe --data build/nn/p1.csv build/nn/proofs.csv build/nn/best.csv \
  --out build/nn/policy.bin --epochs 10 --check 300
NN_POLICY=build/nn/policy.bin build/snek.exe arena 300 0.3 1 nn nn:data/nn/nn_g2.bin   # with a book, as above
python tools/train_policy.py --embed build/nn/policy.bin src/nn_policy.hpp   # the compiled-in policy
```

#### Move ranking: data and results

`nn_g2_policy` was trained for 10 epochs on 248 k positions; `snek policygen` played `nn` without a policy
(`nn:data/nn/nn_g2.bin`), the proof rows come from the refutation sweeps of the centre opening, which are not
in this repository:

| source | positions | plies | targets |
|---|---|---|---|
| `snek policygen`, `nn` (without a policy) at 0.15 s/move, 5 926 games | 55 400 | 1–14 | 28 813 scored, 24 380 proven wins, 2 207 proven draws |
| best moves (flag `b`) of 13 113 `snek datagen` games (the 11 835 of `nn_g2`'s training data and 1 278 more), the side to move not lost | 111 636 | 1–14 | one move each |
| refutation logs of the centre opening⁴ | 81 074 | 2–4 | 80 699 refuting replies (41 671 proven wins), 296 holds, 76 `proveline` answers, 3 best moves of a value list |

⁴ Including rows from older logs of those sweeps (without a header, or with exact values from an earlier
solver), which `tools/policy_rows.py proofs` does not read: from the same logs it gives the same rows except
for 2 420 positions that only the older logs give (2 of them outside the held-out lines) and the targets that
those logs add to 1 356 others (738 outside the held-out lines).

Held out: 33 783 positions (dropped from training: 14 914 that equal a test position up to symmetry). The three
lines hold out their proof rows: 4 634 game positions in them (plies 3–14, 137 at ply 4) are training rows. A
policy trained the same way with those held out as well ranks the refuting replies no worse than the shipped
policy (top-1 / top-5 / mean rank in `train_policy.py`'s report: 0.254 / 0.507 / 18.3, against 0.254 / 0.497 /
18.7). On the test positions, the move to find (the proven move, or the search's best) ranks among the legal
moves as follows; "searched" is the share where it is among the 24 moves that a node with depth ≥ 2 searches:

| test positions | n | legal moves | 1-ply evaluation: top-1 / top-5 / searched / mean rank | policy alone | `nn`'s order (the policy's best 48, sorted with the evaluation) |
|---|---|---|---|---|---|
| refuting replies of the held-out lines | 16 912 | 688 | 0.13 / 0.33 / 0.66 / 24.6 | 0.25 / 0.50 / 0.78 / 18.8 | 0.27 / 0.50 / 0.82 / 17.7 |
| `policygen` positions of held-out games | 5 511 | 544 | 0.44 / 0.61 / 0.75 / 45.3 | 0.41 / 0.61 / 0.80 / 24.3 | 0.46 / 0.65 / 0.82 / 23.5 |
| … plies 1–3 | 1 462 | 1 359 | 0.06 / 0.19 / 0.36 / 132.9 | 0.11 / 0.30 / 0.59 / 56.1 | 0.11 / 0.32 / 0.59 / 56.1 |
| … plies 6–14 | 2 865 | 118 | 0.74 / 0.89 / 0.96 / 5.2 | 0.66 / 0.84 / 0.93 / 7.2 | 0.74 / 0.88 / 0.96 / 6.2 |
| `datagen` best moves of held-out games⁵ | 11 163 | 422 | 0.26 / 0.50 / 0.72 / 41.7 | 0.20 / 0.47 / 0.74 / 32.2 | 0.25 / 0.52 / 0.76 / 31.1 |

The columns come from a test program on `src/` (not in the repository) that orders each test position's legal
moves by the mean evaluation of each child, by the policy's integer logits and as `nn` does, ties counting
half; `train_policy.py`'s report, from its float model, gives the policy-alone top-1 and top-5 within 0.01 and
the mean ranks within 0.1.

⁵ Held out from the policy's training, not from the network's: `nn_g2`'s training run held out games by their
id plus 10¹² times the file's position in its data, so 6 002 of these positions come from games it was
trained on (4 971 of them its training rows), 4 058 from its held-out games and 1 103 from games it never saw.

The order that nodes with depth 1 use (table move, killers, history, piece size, contact) is far worse on the
same positions (top-1 0.001–0.09), but ordering them by the policy did not reduce the nodes of a fixed-depth
search (most of them are all-nodes, which try every move), and searching only the policy's best 64 moves
there scored −26 Elo [−63, +11] against `nn` (108 games at 0.3 s, stopped).

Speed (the same test program; one thread, positions after 2–7 plies of held-out `policygen` games, while
matches and another workload kept most of the other hardware threads busy): a depth-3 search of 20 positions,
each with a new engine and without the exact verdicts, takes 0.34 s and 263 k nodes on average with the policy
against 0.89 s and 558 k nodes without (nodes include the evaluated children); in a fixed time, 60 positions
with one engine kept between them, the last iteration reached is depth 2.78 on average with the policy against
2.77 without at 0.3 s, and 3.22 against 2.78 at 1 s.

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

### Centre opening: what is proven

| position | result |
|---|---|
| `1:60 4B:16,27,28,39 4A:7,8,9,20 4I:15,25,35,45` (after 4 moves, red to move) | **red loses** – all 956 moves (pilot, ~10 thread-hours) |
| `1:60 4B:16,27,28,39 4A:7,8,9,20` (after 3 moves, blue to move) | **blue wins** with `4I:15,25,35,45` |

### Solver v3 (speed-up round)

Benchmark `bench/ply5_blue_wins.txt` = 24 sub-positions of the pilot (ply 5, blue to move and winning),
`snek benchset <file> <threads>`:

| version | thread-seconds | wall, 16 threads |
|---|---|---|
| v2 | 510 | 61 s |
| + 2-way lockless TT (work-preferred slot) | 478 | |
| + incremental snakes in `Pos` (no flood fills for lengths), bounds that flood only the free regions, symmetric hashing only up to ply 6 | **274** | **35 s** |

Tried and rejected (measured, no gain): enhanced transposition cutoffs, a TT shared by all threads, deeper
learned-eval ordering (fewer nodes but the eval costs more than it saves), eval ordering only for the side
that looks winning, a cheap single-region bound before the exact one, and "inert move" equivalence (a
provably safe definition covers well under 1 % of moves). Profiling (`PROFILE=1 snek benchset …`) shows
why big gains are hard: the winning side finds its move at the first try ~90 % of the time, so the search
is already close to the minimal proof; the cost is the loser having to answer every move.

### Solver v4: threat zones, tight bounds, win tests

What the solver does differently from v3 (commit ed2c6b1), every change exact (the values never change):

- **threat zones** and the **last two pieces** in closed form (the side to move places its piece, then the
  opponent its longest extension that is still legal), see the table above;
- the **k + 1 rule** in `snakeBounds` (also in `verify`'s reference bound);
- **windows**: `solve()` takes (alpha, beta); `parallelSolve` can ask only whether the side to move loses
  or wins (`SolveQuestion`), which is what a refutation or a distributed unit needs;
- **ordering**: no history heuristic (its scores, piled up over a long solve, outweighed the static
  ordering), the learned evaluation only once the table move or a killer fails, and after the first three
  moves one sort of the rest;
- **tight bounds** near the end: the one-player optimum and guaranteed growth;
- **one table per sweep**: the solves of a `refute` sweep or a distributed worker share one table kept from
  unit to unit (`SOLVER_SWEEP_TT=<log2 entries>`, default 25: 512 MB per process); killers and the
  one-player memo are reset per position (`Solver::newPosition`, also before every root move of
  `parallelSolve`). Unlike the table shared by one solve's threads that v3 rejected, it is shared by
  sibling units, which have much of their trees in common. Past ply 6, where keys are no longer
  symmetry-canonical, every table entry carries a 32-bit check of the position, since the key bits that
  pick the bucket verify nothing (a 2^25-entry table would otherwise mistake a position about once in
  2^39 probes);
- **hot paths**: placements in one array by id, the next child's table entry fetched while a child is
  searched, `remainingCells` without a loop, `learnedLogit` out of line (inlined, its fused multiply-adds
  ordered moves differently in different callers).

Measured against v3, every position in its own process (empty tables), 4 processes at a time on a
Ryzen 7 250 (16 threads) that ran other solver work on 6 threads; node counts are exact, seconds approximate
(the first four sets: the range of two back-to-back rounds; the refutation units: one round):

| positions | v3: nodes, thread-s | v4: nodes, thread-s | v4 win test (`BENCH_WINTEST=1`) |
|---|---|---|---|
| `ply5_blue_wins.txt` | 361.2 M, 205-209 | 4.40 M, 13 | 3.85 M, 12 |
| `ply5_blue_wins_holdout.txt` | 425.4 M, 252-255 | 4.31 M, 14 | 3.69 M, 12 |
| `ply5_random_units.txt` | 285.2 M, 157 | 6.16 M, 18 | 4.59 M, 14 |
| `ply5_other_openings.txt` | 220.5 M, 117 | 7.45 M, 15 | 6.42 M, 13 |
| `ply5_ublock_refutations.txt` (30 units) | 2 560 M, 2 059 | 37.37 M, 137 | 22.04 M, 85 |

Over the 96 units of the first four sets: 1 292 M nodes and 731–738 thread-seconds with v3, 22.3 M and 59.5
with v4 (58x and 12x fewer), 18.6 M and 51 with the win test. The sets are in `bench/`. The first four hold
24 ply-5 units each, all blue wins: `ply5_blue_wins.txt` (red's moves in the pilot),
`ply5_blue_wins_holdout.txt` (24 others from the pilot in the same cost range), `ply5_random_units.txt`
(drawn like the distributed solve's units: a random red reply to `1:60 4B:16,27,28,39`, blue's answer from a
2 s search, a random red move) and `ply5_other_openings.txt` (blue wins from two verification jobs in
`results/jobs`). `ply5_ublock_refutations.txt` holds 30 proof steps: every 31st of the 938 refutations that
`snek refute` found after red's U-block reply `1:60 4B:16,27,28,39 4C:40,41,50,52` (below), each a position
where blue does not win (27 losses, 3 draws). `snek solvebench` gives the full window on random positions,
one thread:

| `snek solvebench` | v3: nodes, seconds | v4: nodes, seconds |
|---|---|---|
| `5 12 3600 1` | 751.6 M, 487 s | 15.54 M, 56 s |
| `6 12 3600 1` | 51.65 M, 34 s | 2.47 M, 8 s |

The feasibility estimate below predates v4: it prices a red reply at v2/v3 costs.

### Is a full solution feasible?

Solving the centre opening (value after `1:60`) if blue wins = one blue move + all 1589 red replies, each a
ply-3/ply-4 problem like the pilot: ~1589 × ~5.5 thread-hours × (1.5–3 overhead) ≈ **1–3·10⁴ thread-hours**,
i.e. roughly 1–3 months of this laptop (Ryzen 7 5800H, 16 threads) running 24/7, or ~1 day on 1000 cloud
cores. Units are independent jobs (resumable, see `results/jobs/`), so any number of machines can share the
work.

## Refutation sweeps (`snek refute`, `snek proveline`)

`snek refute <threads> <log> <moves…>` shows that the side to move (the attacker) cannot win, or finds a
move that wins: for every attacker move a defender reply after which the attacker does not win. Pass 1 tries
the replies that refuted other moves first, then the defender's best by the evaluation, as node-limited win
tests (`REFUTE_TRIES` × `REFUTE_NODES` M nodes); pass 2 settles the moves left open with a full search over
every reply (a `parallelSolve` yes/no question). The passes alternate on chunks of `REFUTE_CHUNK` moves and
stop at the first move that survives. Attacker moves are taken in the evaluation's order after the 8 best of a
`REFUTE_SEARCH` (2) second engine search; `REFUTE_FIRST=<file>` proves the listed moves first, in their
order, and `REFUTE_PROVE_TOP=<k>` then the k best, before any refutation tries. `REFUTE_LOSS=1` asks whether
the attacker loses instead. The log is resumable and headed by the position, `REFUTE_LOSS` and
`SOLVER_RED_BONUS` (a log of anything else is refused); every refutation in it is one position anyone can
re-solve. For example, `REFUTE_SEARCH=2 snek refute 5 u.log 1:60 4B:16,27,28,39 4C:40,41,50,52` refutes all
938 blue moves after red's U-block reply in 382 s on 5 threads (934 of them in pass 1), so blue's
`4B:16,27,28,39` does not win against the centre; with `REFUTE_LOSS=1` and `REFUTE_FIRST` listing
`4A:49,59,70,81`, the sweep proves in 35 s that this blue move does not lose there.

`snek proveline <threads> <dir> <moves…>` settles every reply of the side to move with a `refute` sweep for
the opponent: an answer that wins against all replies, or proof that the opponent cannot win after that reply
(which settles the line; `PROVELINE_ALL=1` goes on). Answers that won against earlier replies are tried first;
`PROVELINE_SEARCH=<s>` takes the replies in the order of an `<s>` second engine search (kept in
`dir/order.txt`). `dir/summary.txt` and the sweeps' logs make it resumable. `snek uniquemoves` lists the
symmetry-unique moves of a position in the same order.

## Distributed solve of the centre opening (`dist/`)

Proves (or refutes) that after `1:60 4B:16,27,28,39` blue wins: for each of red's 1589 replies the
coordinator tries blue candidates and sends every red answer as a unit (position after 5 moves) to the
workers. A unit that does not come back as a blue win discards the candidate.

- blue's candidates are ranked by a 10 s alpha-beta search (precomputed in the background); the 1-ply
  evaluation alone proposed near-identical moves that all failed against the same red answer,
- red answers that refuted earlier candidates ("killers") are tried first for the next one,
- units carry a node budget (`PROBE_NODES`, default 150 M); over-budget units are solved later without
  budget, an over-budget killer marks the candidate suspect (retried only if nothing else works).
  A candidate is accepted only when every unit is an exact blue win, so budgets never affect soundness.

Updating a running installation: `bash dist/stop.sh`, unzip the new version over the old folder
(`results/dist/` is kept), `bash dist/build.sh`, `bash dist/start.sh` - old state is migrated (killers are
rebuilt from `results.log`).

On a Linux box (runs nonstop, coordinator + local worker):

```bash
sudo apt install build-essential python3
bash dist/start.sh          # builds build/snek, starts coordinator + worker, prints the status URL
bash dist/stop.sh           # stop (state in results/dist/ is kept, start.sh resumes)
```

Other machines on the same network join with `snek worker <ip>:8090 <threads>` (Windows:
`snek.exe worker …`). Status page: `http://<ip>:8091/` (also `/events`, `/state`). Earlier verification results from
`results/jobs/` are reused automatically. Workers started with `SOLVER_WIN_TEST=1` only ask whether blue wins each
unit; a unit that is not a blue win then comes back as 0 (draw or loss) or -1 (loss). A worker's threads share
one transposition table that is kept from unit to unit (`SOLVER_SWEEP_TT`, as for `snek refute`).

## Tools

| Command | Purpose |
|---|---|
| `snek serve [threads]` | UI protocol (below) |
| `snek arena <pairs> <sec> <threads> <A> <B> [seed]` | engine match; engines `ab`, `nn`, `nn:<net file>`, `abhand`, `mcts`, `ab:<weights.txt>`; `ARENA_BOOK=<file>` / `ARENA_BOOK_START=<n>` take the openings from a book; `NN_POLICY=<file>`: `nn`'s move ranking |
| `python tools/arena_stats.py <logs…>` | pentanomial score, Elo with a 95 % interval and the SPRT log-likelihood ratio from `snek arena` logs |
| `snek book <out> <candidates> <threads> <sec> <seed> [plies] [lo] [hi]` | balanced arena openings: `plies` random moves, kept when `nn` scores the side to move within [lo, hi] after `sec` seconds |
| `snek datagen <out.csv> <games> <threads> <sec/move> <seed> [engine] [book]` | self-play training data for the network (see [Network engine](#network-engine-nn)), engine `nn` by default; solves that run out of time are kept as censored |
| `snek policygen <out.csv> <games> <threads> <sec/move> <seed> [engine] [book]` | self-play targets for the move ranking: per position the root search's scored moves and the proven wins (all the winning moves a 5 000-node win test finds once the search proved one at ply ≥ 6) or draws |
| `python tools/policy_rows.py proofs\|datagen <out.csv> <inputs…>` | move-ranking rows in `policygen`'s layout from `refute` logs and `proveline` summaries, or from the best moves of `datagen` files |
| `python tools/train_policy.py --snek <exe> --data a.csv … --out policy.bin` | trains the move ranking on `policygen` files and `policy_rows.py`'s (legal moves from `snek policyfeatures <in.csv> <out.bin> [policy]`, which with a policy file also writes the engine's logits for `--check`); `--test-out` writes the held-out rows; `--embed policy.bin src/nn_policy.hpp` makes it the compiled-in policy |
| `python tools/train_nn.py --snek <exe> --data a.csv b.csv … --out net.bin` | trains the network (inputs from `snek nnfeatures <in.csv> <out.bin>`); `--init net.bin.npz --report-only` reports a network on every row of the files, exact-label and blended-target rows apart; `--embed net.bin src/nn_net.hpp` makes it the compiled-in network |
| `snek nnverify <games> [seed] [net file]` | the network's incremental path against a full refresh for every child of every position of random games; prints the children that differ and a hash of their values, and with the policy a hash of its logits of every position's moves, to compare builds |
| `snek gendata <games> <plyMin> <plyMax> <solveSec> <threads> <seed>` | labelled positions (features + exact value) as CSV |
| `python tools/fit_eval.py data/*.csv` | fit the evaluation → `src/eval_weights.hpp`, `data/weights_latest.txt` |
| `snek solvepos <sec> <threads> <moves…>` | parallel exact solve of a position (`PROGRESS=<file>` logs and resumes; `SOLVER_LOSS_TEST=1` only proves or refutes that the side to move loses: each root move is `loss` or `notloss`, much cheaper) |
| `snek refute <threads> <log> <moves…>` | shows that the side to move cannot win (`REFUTE_LOSS=1`: that it loses), or finds a move that survives (see [Refutation sweeps](#refutation-sweeps-snek-refute-snek-proveline)) |
| `snek proveline <threads> <dir> <moves…>` | settles every reply of the side to move with `refute` for the opponent, resumable |
| `snek uniquemoves <sec> <top> <threads> <moves…> [-- <move>…]` | the side to move's symmetry-unique moves, best first (the `<top>` best of a `<sec>` second search, then the evaluation's order); moves after `--` are mapped to their representative |
| `snek solvebench <plies> <n> <sec> <threads>` | solver benchmark on random positions |
| `snek selfplay <games> <sec> <threads>` | engine vs itself, prints the game |
| `snek verify [games]` | fast move generator == reference checker, incremental snakes and fast bounds == reference, bounds never violated, the one-player optimum between the final snake and the cluster bound, the same per cluster as on the whole board, and never below the real-rules one-player optimum |
| `snek benchset <file> <threads>` | solver benchmark on a fixed position set (`PROFILE=1` prints per-ply statistics, including threat zones, the last pieces, the tight bounds and the one-player questions; `BENCH_FRESH=1` starts every position from empty tables for reproducible node counts, `BENCH_WINTEST=1` only asks whether the side to move wins) |
| `snek perft <d>`, `snek speed`, `snek info` | counting / micro-benchmarks |
| `python ref/crosscheck.py [games]` | engine vs independent Python rules implementation |

### Engine protocol (`snek serve [threads]`, one line in → one JSON line out)

- `new`, `state`, `undo`, `set <m1> <m2> …` (position from the start), `play <m> …` (append moves)
- move notation: `pass` or `<piece>:<cell>,<cell>,…`, cell = `(r+5)*11 + (q+5)` (axial q,r; centre = 60).
  Pieces: `1 2 3I 3V 4I 4A 4B 4C`.
- `analyze <sec> [engine]` / `go <sec> [engine]` – ranked moves (score = 100·logit of the win probability,
  ±100000 = proven; `engine` for this command only, the session's engine by default),
  `go` also plays the best move
- `solve <sec>` – parallel exact solve
- `engine ab|nn|mcts` – the engine of `analyze` and `go` (default `ab`)

## Verification

- Placement counts match the independent Python enumeration (2602 placements).
- `ref/crosscheck.py`: legal-move sets and snake lengths identical to the Python reference (398 positions).
- `snek verify 3000`: bit-parallel generator gives the identical move list (same order) as the per-placement
  checker in 47 131 positions; the length bounds held for the final result in all 94 262 checks.
- Solver v2 gives the same values as v1 on all benchmark positions.
- Solver v4 gives the same values as v3 on 1 548 random positions (`snek solvebench`, 96 per ply 8–12 and red
  bonus −1/0/+1 (`SOLVER_RED_BONUS`), so that draws and losses occur, and 12 per ply 5–7 and bonus), on the 96
  units of the first four `bench/` sets (exact and win test) and on the 30 of `ply5_ublock_refutations.txt`.
- `snek verify 3000` also checks the one-player optimum in 55 259 positions with at most 4 pieces left: never
  below the final snake, never above the cluster bound (strictly below it in 24 272), the same cluster by
  cluster as over the whole board, and never below the real-rules one-player optimum (brute force on 43 191
  positions with at most 3 pieces).
- `snek nnverify 1000`: the network's incremental planes, accumulators and values equal a full refresh for all
  6 308 902 children of 1 000 random games; a build without `-march=native` (the scalar paths) prints the same
  value and policy hashes (`nnverify 200`).
- `perft`: 1 → 91, 2 → 194 388, 3 → 315 706 848.

## Layout

| Path | What |
|---|---|
| `src/core.hpp` | geometry, pieces, bit-parallel move generation, bounds, notation |
| `src/eval.hpp`, `src/eval_weights.hpp` | features, learned + hand-made evaluation |
| `src/search.hpp` | α-β playing engines `ab`, `nn` |
| `src/nn.hpp`, `src/nn_net.hpp`, `src/nn_policy.hpp` | network evaluation and move ranking, compiled-in network and policy |
| `src/datagen.hpp` | self-play training data, opening books |
| `src/solver.hpp` | exact solver, parallel solve |
| `src/refute.hpp` | refutation sweep (`snek refute`, `snek proveline`) |
| `src/mcts.hpp` | v1 MCTS engine (kept for comparison) |
| `src/main.cpp` | CLI + UI protocol |
| `readable/` | the same engine refactored for reading (identical results), with a reading guide: [readable/README.md](readable/README.md) |
| `ref/` | independent Python reference implementation + cross-check |
| `tools/fit_eval.py`, `tools/train_nn.py`, `tools/policy_rows.py`, `tools/train_policy.py` | evaluation fitting, network and move ranking training |
| `tools/arena_stats.py` | match statistics |
| `data/` | solver-labelled positions (`labels_v*.csv`), weight files, `nn/` networks and policies, `book2.txt` arena openings |
| `ui/` | Node server + single-page UI |
| `bench/` | fixed position sets for `snek benchset` |
| `results/` | benchmark / arena / solve logs |

# Readable version of the engine

This folder holds the same program as `src/`, rewritten to be read by people: smaller files by topic,
descriptive names, short functions, and comments that explain *why*. Its behaviour and results are the
same as those of `src/`. Every deterministic output was compared and is identical (see
[Verification](#verification)).

`src/` stays the production version: `build.sh`, the UI (`ui/server.js`) and the distributed workers use
it. Use this folder to understand the code, or as a drop-in replacement; the command line is the same.

```bash
g++ -O3 -march=native -std=c++20 -pthread -static -o build/snek_readable.exe readable/*.cpp -lws2_32   # Windows (MinGW)
g++ -O3 -march=native -std=c++17 -pthread -o build/snek_readable readable/*.cpp                         # Linux
```

The learned evaluation weights are not duplicated: `evaluation.hpp` includes `../src/eval_weights.hpp`,
which `tools/fit_eval.py` writes.

## Reading order

| # | File | What it contains |
|---|------|------------------|
| 1 | `board.hpp` | Hex coordinates, the 128-bit `Bitboard`, neighbours by shifting, flood fill, the 12 symmetries |
| 2 | `pieces.hpp` | The 8 pieces, their orientations, all placements (numbered once at start-up), `Move` |
| 3 | `position.hpp` | `Position`, `play()`, snakes kept up to date incrementally, Zobrist hashing, symmetric keys |
| 4 | `rules.hpp` | The placement rules: a plain rule check (for either player) and the fast bit-parallel move generator |
| 5 | `bounds.hpp` | Sound lower and upper bounds on each player's final longest snake, the clusters behind them, and the exact one-player optimum (the longest snake a player could still make if the opponent never moved again) |
| 6 | `features.hpp` | Territory split ("who gets where first") and per-player features |
| 7 | `evaluation.hpp` | Learned evaluation (logistic regression over 14 features) and the old hand-made one |
| 8 | `transposition_table.hpp` | Lockless two-slot hash table of the exact solver |
| 9 | `solver.hpp` | **Exact solver**: alpha-beta over {-1, 0, +1} with a window, threat zones and the last pieces valued without a search, the one-player optimum and guaranteed growth near the end, split into steps (table, last pieces, bounds, tight bounds, threats, ordering, search, store) |
| 10 | `parallel_solve.hpp` | Exact solve of a position, or a yes/no question about it, with its root moves spread over threads |
| 11 | `engine.hpp` | **Playing engine**: iterative-deepening PVS with forward pruning, multi-PV, score smoothing |
| 12 | `analysis.hpp` | "Analyse this position" for the UI: exact when few pieces are left, otherwise the engine |
| 13 | `refute.hpp` | **Refutation sweep**: does the side to move win, or can each of its moves be refuted? Node-limited tries of likely refutations, then full yes/no solves, on chunks of moves; resumable log |
| 14 | `mcts.hpp` | The first engine (Monte-Carlo tree search), kept for comparison |
| 15 | `notation.hpp`, `training_data.hpp` | Moves and positions as text; CSV rows for training the evaluation |
| 16 | `main.cpp`, `commands.hpp` | Table of all commands with their arguments |
| 17 | `cmd_*.cpp` | One file per group of commands: `serve` (UI protocol), `solve` (solvepos, solvebench, benchset), `refute` (refute, proveline, uniquemoves), `check` (perft, verify, info, speed), `data` (gendata, refeature, evalexplain, evaltrace), `play` (arena, selfplay), `worker` (distributed solving) |

To see how a position gets solved, read `rules.hpp`, then `bounds.hpp`, then `solver.hpp`. To see where
the engine's move comes from, read `evaluation.hpp`, then `engine.hpp`, then `analysis.hpp`. To see how a
line of play gets proved one reply at a time, read `parallel_solve.hpp`, then `refute.hpp`, then
`proveline` in `cmd_refute.cpp`.

## Names: original to readable

| `src/` | `readable/` |
|--------|-------------|
| `BB` | `Bitboard` |
| `Pos`, `s` | `Position`, `pos` |
| `own[c]`, `avail[c]`, `stm`, `passes` | `cells[player]`, `piecesLeft[player]`, `sideToMove`, `passesInRow` |
| `comp`, `ncomp`, `L` | `snakes`, `snakeCount`, `longestSnake` |
| `h[t]`, `key(t)` | `hash[t]`, `canonicalKey(symmetry)` |
| `T` (one table struct) | `geometry`, `pieceTables`, `zobrist` |
| `T.PL[p]`, `gid` | `placementsOf(p)`, `placementId` (all placements in one vector) |
| `T.gidSym`, `T.perm`, `T.inv` | `symmetricPlacement`, `geometry.symmetric`, `geometry.inverseSymmetry` |
| `T.fit` | `pieceTables.bestFill` |
| `expand`, `shiftFrom`, `flood`, `popc`, `lsb` | `neighboursOf`, `cellsWithOffsetIn`, `floodFill`, `countCells`, `firstCell` |
| `Shape` (`ge1`, `ge2`, `ge3`, `tri2`, `full`) | `SnakeShape` (`touches1..3`, `touchesAdjacentPair`, `body`) |
| `GenCtx`, `orientLegal` | `MoveGenContext`, `legalAnchors` |
| `Orient` (`ideg`, `tri`, `two`, `gidAt`) | `Orientation` (`innerNeighbours`, `wedgeCells`, `bridgeCells`, `placementAt`) |
| `s.genMoves`, `s.hasMove`, `s.legal`, `s.legalMove` | `generateMoves`, `hasLegalMove`, `isLegalPlacement`, `isLegalMove` |
| `s.legalFor(c, pl)`, `s.forEachLegalTouching(cells, f)` | `isLegalPlacementFor(pos, player, p)`, `forEachLegalMoveCovering(pos, cells, f)` |
| `snakeBounds(s, c, lb, ub)` | `SnakeBounds snakeBounds(pos, player)` with `.lower`, `.upper` |
| `Clusters` (`n`, `ub`, `region`), `snakeClusters`, `clusterBound` | `SnakeClusters` (`count`, `upper`, `regions`), `snakeBounds(pos, player, &clusters)`, `clusterUpperBound` |
| `growable`, `boundAfterBlock`, `lengthAfter` | `growableCells`, `upperBoundAfterBlock`, `longestSnakeAfter` |
| `voronoi`, `PlayerFeat`, `features` | `splitTerritory`, `PlayerFeatures`, `computeFeatures` |
| `EW`, `EHEAD`, `g_evalSet`, `g_handEval`, `evaluateHand` | `featureWeights`, `featureHeadStart`, `activeWeightSet`, `useHandMadeEval`, `evaluateHandMade` |
| `LTT`, `SWEEP_TT_LOG2` | `TranspositionTable`, `SWEEP_TABLE_LOG2` |
| `Solver::terminalValue`, `finalScore` | `gameResult`, `redLead` |
| `Threat`, `addThreat`, `scanThreats`, `threatBound` | `Threat`, `ThreatList::tryPlacement`, `ThreatList::scan`, `ThreatList::boundFor` |
| `OnePlayer` (`mayReach`, `reachIn`, `dfs`, `queries`, `unreachableAnswers`, `dfsNodes`), `DFS_BUDGET` | `OnePlayerOptimum` (`mayReach`, `reachableInCluster`, `extend`, `stats.questions`, `.unreachable`, `.searchNodes`), `SEARCH_BUDGET` |
| `Orient::nextTo`, `orientGrowLegal`, `forEachLegalGrow` | `Orientation::anchorsNextTo`, `growingAnchors`, `forEachGrowingMove` |
| `Solver::growthBounds`, `tightenBounds`, `restBounds`, `pTight` | `guaranteedGrowth`, `Solver::narrowByTightBounds` (also for the moves left at an all-node, step 6 of `search`), `profile.tightCut` |
| `lastPieces` (`-2`: not the last pieces) | `lastPiecesValue` (`false`) |
| `pPruned`, `pMoves`, `pLast`, `pScans`, `pScanHits` | `profile.threatSkipped`, `.movesListed`, `.lastPieces`, `.threatScans`, `.threatScansFound` |
| `ParallelSolveResult`, `SolveQuestion` | `RootSolveResult`, `SolveQuestion` (an `enum class`) |
| `AbSearcher::ab`, `exactKnown`, `AP` | `AlphaBetaSearcher::search`, `exactScore`, `engineParams` |
| `abAnalyze`, `RootResult`, `AbAnalysis` | `analyzeWithEngine`, `RootMoveResult`, `EngineAnalysis` |
| `terminalScore` | `finishedGameScore` |
| `uniqueRootMoves` | `symmetryUniqueMoves` |
| `refuteMoves`, `RefuteResult` (`refute.hpp`) | `refuteMoves`, `RefuteResult`; the sweep is a `RefutationSweep` |
| `quickPass`, `fullPass`, `killers`, `SEARCH_TOP`, `searchSecs` | `quickPass` (`tryRefutations` per move), `fullPass`, `refutations`, `SEARCH_FAVOURITES`, `searchSeconds` |
| proveline's `done`, `answers` | `settled`, `winningAnswers` |
| `moveStr`, `posStr`, `parsePosStr` | `moveToString`, `positionToString`, `parsePosition` |
| `-9` | `Solver::UNKNOWN` |

The feature names of the learned evaluation (`dL`, `dSingle`, ...) and the CSV column names stay as they
are, because weight files and `tools/fit_eval.py` use them. So do the environment variables
(`SOLVER_*`, `BENCH_*`, `REFUTE_*`, `PROVELINE_*`).

## What must not change (or results change)

Some details look arbitrary but decide the exact node counts, the move order and so the results:

- **Placement numbering**: piece by piece, orientations in the order the symmetries produce them,
  anchor cells ascending. The move generator returns moves in this order.
- **Zobrist keys**: seed `0x5eca9011`, generated in the order red cells, red pieces, blue cells, blue
  pieces, side to move. Symmetric hashes are kept only up to ply 6 (`SYMMETRY_PLY`).
- **Move ordering**: all scores (killers, `+256` / `+128` / `+32` bonuses, evaluation × 1000; the
  engine's history), the selection sort of the solver's first three moves that keeps the earlier move
  on ties, the one sort of the rest by score and then placement id, and `std::sort` (not stable) on
  the same input arrays (also the threats by zone size, the last pieces' replies and the replies the
  refutation sweep tries).
- **Threat lists**: at most 6 threats per node, 8 placements tried one by one, 64 found per scan; the
  killers of the next ply are tried before the moves are generated.
- **Transposition-table layout** (with the 32-bit key check) and its replacement rule.
- **Floating point**: the logit is summed in feature order, and `learnedLogit` and the sum that
  `evalexplain` prints stay out of line, so that every caller rounds the same, and as in `src/`
  (inlined, the compiler fuses the multiply-adds in some callers and not in others);
  `(int)(1000 * logit)` truncates and `lround(100 * z)` rounds, as before.
- **Output formats**: JSON for the UI, CSV for training data, the worker protocol, the `solvepos`
  progress lines, the `refute` log, the `proveline` summary and its log names.

## Differences from `src/`

- An unknown command prints the list of commands (`src/` silently exits).
- Missing required arguments of `solvepos` and `evaltrace` fall back to defaults instead of crashing.
- `src/` fetches the table entry of the next move while a move is searched (a prefetch: speed only).

## Verification

Both builds (`build/snek_orig.exe` from `src/`, `build/snek_readable.exe` from here) were run on the
same inputs and the outputs compared with timings removed. All were identical:

| Check | What it covers |
|-------|----------------|
| `perft 3` | 91 / 194 388 / 315 706 848: move generation |
| `verify 3000` | 47 131 positions: fast generator vs rule check, fast vs reference bounds, bounds vs final results; the one-player optimum in 55 259 positions |
| `info` | placement counts |
| `solvebench 5 12`, `solvebench 6 12`, `solvebench 9 96` with `SOLVER_RED_BONUS` -1, 0 and 1 (1 thread) | values and node counts of the exact solver |
| `BENCH_FRESH=1 benchset` on the four `bench/ply5_*` sets, exact and with `BENCH_WINTEST=1`, and on 30 refutation units with `BENCH_WINTEST=1 PROFILE=1` (1 thread) | node counts per position (4 403 893 exact on `ply5_blue_wins.txt`), per-ply statistics |
| `solvepos` at ply 6 with a `PROGRESS` log and its resume (1 thread) and with 8 threads, at ply 7 with `SOLVER_LOSS_TEST=1` (1 thread) | root-move results, and node counts with 1 thread |
| `refute 1` with `REFUTE_SEARCH=0` after `1:60 4B:16,27,28,39 3V:40,41,50 4A:62,72,82,93 4A:58,59,68,78 3V:31,42,52 4I:7,8,9,10` (blue cannot win; also with `REFUTE_LOSS=1` and small `REFUTE_CHUNK`, `REFUTE_TRIES`, `REFUTE_NODES`) and after `1:60 4B:16,27,28,39 3V:29,40,50 4A:6,7,8,19 2:89,99 4C:49,59,70,71 3I:94,104,114` (blue wins) | output and log: every refutation, open move and survivor |
| `PROVELINE_ALL=1 proveline 1` after the second of those positions and `2:20,21` | output, summary and the sweeps' logs |
| `refute` with its log in a missing directory, `proveline` with its directory below a file | the error and exit code |
| `uniquemoves 0 0 1`, with moves mapped after `--` | order and mapping |
| `serve 1`: state, set, play, undo, children, engine, solve, errors | UI protocol |
| `gendata` (normal and `good` mode, 1 thread), `refeature` of their rows, `evalexplain`, `evaltrace 3` | training rows; features, learned evaluation, fixed-depth engine search |
| `worker` against a local test coordinator, also with `SOLVER_WIN_TEST=1` | protocol, values and nodes, node budgets, invalid units |
| `speed` | the checksums of the hot functions |

Timing-dependent commands (`arena`, `selfplay`, MCTS, `serve`'s analyze and go, `refute`'s engine search)
cannot be compared output to output.

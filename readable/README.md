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
| 4 | `rules.hpp` | The placement rules: a plain rule check and the fast bit-parallel move generator |
| 5 | `bounds.hpp` | Sound lower and upper bounds on each player's final longest snake |
| 6 | `features.hpp` | Territory split ("who gets where first") and per-player features |
| 7 | `evaluation.hpp` | Learned evaluation (logistic regression over 14 features) and the old hand-made one |
| 8 | `transposition_table.hpp` | Lockless two-slot hash table of the exact solver |
| 9 | `solver.hpp` | **Exact solver**: alpha-beta over {-1, 0, +1}, split into steps (table, bounds, ordering, search, store) |
| 10 | `parallel_solve.hpp` | Exact solve of a position with its root moves spread over threads |
| 11 | `engine.hpp` | **Playing engine**: iterative-deepening PVS with forward pruning, multi-PV, score smoothing |
| 12 | `analysis.hpp` | "Analyse this position" for the UI: exact when few pieces are left, otherwise the engine |
| 13 | `mcts.hpp` | The first engine (Monte-Carlo tree search), kept for comparison |
| 14 | `notation.hpp`, `training_data.hpp` | Moves and positions as text; CSV rows for training the evaluation |
| 15 | `main.cpp`, `commands.hpp` | Table of all commands with their arguments |
| 16 | `cmd_*.cpp` | One file per group of commands: `serve` (UI protocol), `solve` (solvepos, solvebench, benchset), `check` (perft, verify, info, speed), `data` (gendata, refeature, evalexplain, evaltrace), `play` (arena, selfplay), `worker` (distributed solving) |

To see how a position gets solved, read `rules.hpp`, then `bounds.hpp`, then `solver.hpp`. To see where
the engine's move comes from, read `evaluation.hpp`, then `engine.hpp`, then `analysis.hpp`.

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
| `snakeBounds(s, c, lb, ub)` | `SnakeBounds snakeBounds(pos, player)` with `.lower`, `.upper` |
| `voronoi`, `PlayerFeat`, `features` | `splitTerritory`, `PlayerFeatures`, `computeFeatures` |
| `EW`, `EHEAD`, `g_evalSet`, `g_handEval`, `evaluateHand` | `featureWeights`, `featureHeadStart`, `activeWeightSet`, `useHandMadeEval`, `evaluateHandMade` |
| `LTT` | `TranspositionTable` |
| `Solver::terminalValue`, `finalScore` | `gameResult`, `redLead` |
| `AbSearcher::ab`, `exactKnown`, `AP` | `AlphaBetaSearcher::search`, `exactScore`, `engineParams` |
| `abAnalyze`, `RootResult`, `AbAnalysis` | `analyzeWithEngine`, `RootMoveResult`, `EngineAnalysis` |
| `terminalScore` | `finishedGameScore` |
| `uniqueRootMoves` | `symmetryUniqueMoves` |
| `moveStr`, `posStr`, `parsePosStr` | `moveToString`, `positionToString`, `parsePosition` |
| `-9` | `Solver::UNKNOWN` |

The feature names of the learned evaluation (`dL`, `dSingle`, ...) and the CSV column names stay as they
are, because weight files and `tools/fit_eval.py` use them.

## What must not change (or results change)

Some details look arbitrary but decide the exact node counts, the move order and so the results:

- **Placement numbering**: piece by piece, orientations in the order the symmetries produce them,
  anchor cells ascending. The move generator returns moves in this order.
- **Zobrist keys**: seed `0x5eca9011`, generated in the order red cells, red pieces, blue cells, blue
  pieces, side to move. Symmetric hashes are kept only up to ply 6 (`SYMMETRY_PLY`).
- **Move ordering**: all scores (history, killers, `+256` / `+128` / `+32` bonuses, evaluation × 1000),
  the selection sort that keeps the earlier move on ties, and `std::sort` (not stable) on the same
  input arrays.
- **Transposition-table layout** and its replacement rule.
- **Floating point**: the logit is summed in feature order; `(int)(1000 * logit)` truncates and
  `lround(100 * z)` rounds, as before.
- **Output formats**: JSON for the UI, CSV for training data, the worker protocol, the `solvepos`
  progress lines.

## Differences from `src/`

- An unknown command prints the list of commands (`src/` silently exits).
- Missing required arguments of `solvepos` and `evaltrace` fall back to defaults instead of crashing.

## Verification

Both builds (`build/snek_orig.exe` from `src/`, `build/snek_readable.exe` from here) were run on the
same inputs and the outputs compared with timings removed. All were identical:

| Check | What it covers |
|-------|----------------|
| `perft 3` | 91 / 194 388 / 315 706 848: move generation |
| `verify 2000` | 31 391 positions: fast generator vs rule check, fast vs reference bounds, bounds vs final results |
| `info` | placement counts |
| `solvebench 8 6`, `solvebench 10 10` (1 thread) | values and node counts of the exact solver |
| `benchset` (6 ply-5 positions, `PROFILE=1`) | 25 137 794 nodes, identical per-ply statistics |
| `solvepos` at ply 6 (1 and 8 threads), with `PROGRESS` log and resume | all 408 root-move results (red loses) |
| `serve`: state, set, play, undo, children, engine, analyze, go, solve, errors | UI protocol |
| `gendata` (normal and `good` mode, 1 thread) | training rows: features, exact values, hand evaluation |
| `refeature`, `evalexplain`, `evaltrace 3` | features, learned evaluation, fixed-depth engine search |
| `worker` against a local test coordinator | protocol, node budgets, invalid units |
| `speed` | same checksums of the hot functions; speed equal within noise |

Timing-dependent commands (`arena`, `selfplay`, MCTS) cannot be compared output to output. They run, and
their results are the same within normal variation.

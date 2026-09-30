# Snekagon – rules as implemented

Source: *Snekagon Rulebook 0.9 (en)* + print & play sheet, plus clarifications from the designer.

- **Board:** hexagon with side 6 → 91 cells.
- **Pieces** (per player, double-sided → may be rotated and flipped), shapes read off the PnP sheet, axial (q, r):
  | name | cells | shape |
  |---|---|---|
  | `1` | 1 | single hex – **only player 1 (red)** |
  | `2` | 2 | domino |
  | `3I` | 3 | straight |
  | `3V` | 3 | 120° bend `(0,0) (1,0) (1,1)` |
  | `4I` | 4 | straight |
  | `4A` | 4 | `(0,0) (1,0) (1,1) (1,2)` – chiral, 12 orientations |
  | `4B` | 4 | `(0,0) (0,1) (1,1) (1,2)` – zig-zag |
  | `4C` | 4 | `(0,0) (-1,1) (-1,2) (0,2)` |

  Player 1: 25 cells, player 2: 24 cells. The grey hexes on the PnP sheet are not used.
- **First move:** player 1 places the 1-hex anywhere (not counted as a full turn → "7 turns" each afterwards).
  Player 2 has no 1-hex.
- **Turn:** place exactly one remaining piece, either
  - **new snake:** the piece touches no tile at all (neither colour), or
  - **grow:** the piece touches at least one own tile (may also touch opponent tiles; may join own snakes;
    closing a ring is allowed).
- **Snake shape:** after the move, in the player's own cells every cell has ≤ 2 own neighbours and no three own
  cells are mutually adjacent ("too sharp a turn"). Equivalently every own component is an induced path or
  ring. This also forbids branching and two own snakes touching sideways.
- **Pass:** a player without a legal placement passes. The game ends when neither player can move.
- **Result:** longest snake (component size in hexes) wins; equal lengths = draw.

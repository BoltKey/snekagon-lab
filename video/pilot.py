"""Pilot: section 1 (cold open), the title card and section 5 (the rules).

Render:  .venv/Scripts/manim -r 1920,1080 --fps 30 pilot.py Pilot
"""
from snekviz import *  # noqa: F401,F403


class Pilot(Narrated):
    def construct(self):
        self.cold_open()
        self.title_card()
        self.rules()

    # ---- helpers ------------------------------------------------------------------------------
    def counters(self):
        red = VGroup(Text('red', font=FONT, font_size=30, color=P_RED), Text('0', font=FONT, font_size=40, color=P_RED, weight=BOLD)).arrange(RIGHT, buff=0.2)
        blue = VGroup(Text('blue', font=FONT, font_size=30, color=P_BLUE), Text('0', font=FONT, font_size=40, color=P_BLUE, weight=BOLD)).arrange(RIGHT, buff=0.2)
        red.to_corner(UL, buff=0.5)
        blue.to_corner(UR, buff=0.5)
        return red, blue

    def set_count(self, counter, value, color):
        """Animation that changes a counter's number."""
        old = counter[1]
        new = Text(str(value), font=FONT, font_size=40, color=color, weight=BOLD)
        new.next_to(counter[0], RIGHT, buff=0.2)
        return Transform(old, new)

    def play_moves(self, board, moves, start_ply, per_move, counters=None, placed=None):
        """Animates moves (engine notation) onto the board; returns the piece mobjects."""
        placed = placed if placed is not None else {0: set(), 1: set()}
        pieces = []
        for k, m in enumerate(moves):
            ply = start_ply + k
            player = ply % 2
            cells = move_cells(m)
            if not cells:
                self.wait(per_move * 0.5)
                pieces.append(None)
                continue
            p = piece_on(board, cells, P_RED if player == 0 else P_BLUE)
            placed[player] |= set(cells)
            anims = [FadeIn(p, scale=1.35)]
            if counters:
                anims.append(self.set_count(counters[player], longest_snake(placed[player]), P_RED if player == 0 else P_BLUE))
            self.play(*anims, run_time=per_move)
            pieces.append(p)
        return pieces, placed

    # ---- 1. cold open -------------------------------------------------------------------------
    def cold_open(self):
        board = Board(s=0.34)
        self.board = board

        def draw_board(d):
            rings = [VGroup(*board.ring(k)) for k in range(6)]
            self.play(LaggedStart(*[FadeIn(r, scale=0.8) for r in rings], lag_ratio=0.35), run_time=d * 0.45)
            hexr = piece_on(board, [60], P_RED)
            hexr.shift(UP * 4)
            self.add(hexr)
            self.play(hexr.animate.shift(DOWN * 4), rate_func=rush_into, run_time=0.9)
            self.play(Flash(board.at(60), color=P_RED, line_length=0.25), run_time=0.5)
            self.first_hex = hexr

        self.beat("Here is a game you can explain in under a minute. Two players, one board, a handful of small "
                  "pieces each. You take turns placing them, and whoever ends up with the longest snake wins.", draw_board)

        red, blue = self.counters()
        red[1].become(Text('1', font=FONT, font_size=40, color=P_RED, weight=BOLD).next_to(red[0], RIGHT, buff=0.2))
        self.counters_rb = (red, blue)

        def whole_game(d):
            self.play(FadeIn(red), FadeIn(blue), run_time=0.5)
            pieces, placed = self.play_moves(board, DRAW_GAME[1:], 1, per_move=(d * 0.8) / 15, counters=(red, blue),
                                             placed={0: {60}, 1: set()})
            self.game_pieces = [self.first_hex] + [p for p in pieces if p is not None]
            self.play(red[1].animate.set_color(GOLD), blue[1].animate.set_color(GOLD), run_time=0.4)

        self.beat("Each player gets just seven real turns. The whole game fits on a coffee table and takes about "
                  "five minutes.", whole_game)

        def tree(d):
            everything = VGroup(board, *self.game_pieces)
            self.play(everything.animate.scale(0.32).to_corner(UL, buff=0.35),
                      FadeOut(red), FadeOut(blue), run_time=1.2)
            root = everything.get_corner(DR) + np.array([0.15, -0.1, 0])
            levels = [6, 5, 4, 3]
            lines = VGroup()
            parents = [root]
            width = 12.5
            for depth, fan in enumerate(levels):
                kids = []
                n = len(parents) * fan
                y = root[1] - 1.15 * (depth + 1)
                for j, par in enumerate(parents):
                    for k in range(fan):
                        idx = j * fan + k
                        x = -width / 2 + width * (idx + 0.5) / n + 0.6
                        child = np.array([x, y, 0])
                        lines.add(Line(par, child, stroke_width=max(0.4, 2.2 - 0.5 * depth), color=INK,
                                       stroke_opacity=max(0.12, 0.8 - 0.2 * depth)))
                        kids.append(child)
                parents = kids
            self.play(LaggedStart(*[Create(l) for l in lines], lag_ratio=0.004), run_time=d * 0.45)
            self.tree_lines = lines
            row = VGroup(*[Square(0.11, stroke_width=0.8, color=MUTED).set_fill(MUTED, 0.25) for _ in range(76)])
            row.arrange(RIGHT, buff=0.045).to_edge(DOWN, buff=0.45)
            self.play(lines.animate.set_stroke(opacity=0.12), LaggedStart(*[FadeIn(sq) for sq in row], lag_ratio=0.03),
                      run_time=d * 0.18)
            last = row[-1]
            self.play(last.animate.set_fill(GOLD, 1).set_stroke(GOLD).scale(1.8), Flash(last, color=GOLD), run_time=1.0)
            self.row76 = row

        self.beat("So here is the question that has been bugging me for years. If both players play perfectly, who "
                  "wins? It feels like a question a computer should answer over lunch. This video is about why it did "
                  "not, what we found when we tried anyway, and one single move, buried 76th in line, that changed the "
                  "whole picture.", tree)

    def title_card(self):
        self.play(FadeOut(Group(*self.mobjects)), run_time=1.0)
        title = Text('Snekagon', font=FONT, font_size=110, weight=BOLD, color=INK)
        sub = Text('the 7 turn game that could not be solved', font=FONT, font_size=40, color=MUTED)
        VGroup(title, sub).arrange(DOWN, buff=0.45)
        snake = VGroup(*[hex_shape(np.array([-3 + 0.52 * k, -2.4 + 0.18 * math.sin(k), 0]), 0.3,
                                   fill=P_RED if k < 6 else P_BLUE, stroke=BG, sw=2) for k in range(12)])
        self.play(Write(title), run_time=1.6)
        self.play(FadeIn(sub, shift=UP * 0.2), LaggedStart(*[GrowFromCenter(h) for h in snake], lag_ratio=0.08), run_time=1.6)
        self.wait(1.8)
        self.play(FadeOut(VGroup(title, sub, snake)), run_time=0.8)

    # ---- 5. the rules -------------------------------------------------------------------------
    def rules(self):
        board = Board(s=0.34)
        names = ['2', '3I', '3V', '4I', '4A', '4B', '4C']

        def board_and_pieces(d):
            self.play(FadeIn(board, scale=0.95), run_time=d * 0.12)
            n91 = self.caption('91 cells', INK, 34).next_to(board, DOWN, buff=0.25)
            side = [board.cells[i] for i in (5, 6, 7, 8, 9, 10)]
            self.play(FadeIn(n91), *[s.animate.set_stroke(GOLD, 3) for s in side], run_time=d * 0.12)
            six = self.caption('6 per side', GOLD, 26).next_to(VGroup(*side), UP, buff=0.12)
            self.play(FadeIn(six), run_time=0.5)
            self.play(*[s.animate.set_stroke(FAINT, 1.2) for s in side], FadeOut(six), run_time=0.4)
            self.play(VGroup(board, n91).animate.shift(LEFT * 3.4), run_time=d * 0.1)
            shapes = VGroup(*[loose_piece(n, P_BLUE, s=0.26) for n in names])
            top = VGroup(*shapes[:3]).arrange(RIGHT, buff=0.7)
            bottom = VGroup(*shapes[3:]).arrange(RIGHT, buff=0.55)
            grid = VGroup(top, bottom).arrange(DOWN, buff=0.75).move_to(RIGHT * 3.3 + UP * 0.4)
            self.play(LaggedStart(*[FadeIn(sh, shift=UP * 0.2) for sh in shapes], lag_ratio=0.15), run_time=d * 0.25)
            self.play(*[Rotate(sh, angle=PI / 3) for sh in shapes], run_time=0.8)
            self.play(*[sh.animate.stretch(-1, 0) for sh in shapes], run_time=0.8)
            self.play(*[Rotate(sh, angle=-PI / 3) for sh in shapes], run_time=0.6)
            labels = VGroup(*[self.caption(n, MUTED, 22).next_to(shapes[k], DOWN, buff=0.18) for k, n in enumerate(names)])
            self.play(FadeIn(labels), run_time=0.5)
            self.rules_board, self.n91, self.shapes, self.shape_labels = board, n91, shapes, labels

        self.beat("The board is a hexagon of 91 cells. Each player has a set of pieces made of hexes: a domino, two "
                  "shapes of three, and four shapes of four.", board_and_pieces)

        def single_hex(d):
            one = loose_piece('1', P_RED, s=0.34).move_to(RIGHT * 3.3 + UP * 3.0)
            lab = self.caption('only red', P_RED, 24).next_to(one, RIGHT, buff=0.25)
            self.play(GrowFromCenter(one), FadeIn(lab), run_time=d * 0.15)
            self.play(one.animate.move_to(self.rules_board.at(60)), run_time=d * 0.2)
            turns = self.caption('1 hex, then 7 turns each', INK, 30).move_to(RIGHT * 3.3 + UP * 3.0)
            self.play(FadeOut(lab), run_time=0.4)
            self.play(FadeIn(turns), run_time=d * 0.12)
            self.one, self.turns = one, turns

        self.beat("The first player, red, also gets this: a single hex. Red places it anywhere to start the game. It "
                  "is such a tiny move that it does not really count as a turn, which is why we say each player then "
                  "gets seven turns.", single_hex)

        def tallies(d):
            red_sum = self.caption('red   1 + 2 + 3 + 3 + 4 + 4 + 4 + 4 = 25', P_RED, 28)
            blue_sum = self.caption('blue      2 + 3 + 3 + 4 + 4 + 4 + 4 = 24', P_BLUE, 28)
            sums = VGroup(red_sum, blue_sum).arrange(DOWN, aligned_edge=RIGHT, buff=0.25).move_to(RIGHT * 3.3 + DOWN * 2.6)
            self.play(Write(red_sum), run_time=d * 0.3)
            self.play(Write(blue_sum), run_time=d * 0.3)
            self.play(Indicate(red_sum[-2:], color=GOLD), Indicate(blue_sum[-2:], color=GOLD), run_time=1.0)
            self.sums = sums

        self.beat("So red owns 25 cells of material and blue owns 24. Remember that difference; the players certainly "
                  "did.", tallies)

        red, blue = self.counters()
        placed = {0: set(), 1: set()}

        def opening(d):
            self.play(FadeOut(VGroup(self.shapes, self.shape_labels, self.sums, self.turns, self.n91)),
                      VGroup(self.rules_board, self.one).animate.shift(RIGHT * 3.4), run_time=1.0)
            placed[0].add(60)
            red[1].become(Text('1', font=FONT, font_size=40, color=P_RED, weight=BOLD).next_to(red[0], RIGHT, buff=0.2))
            self.play(FadeIn(red), FadeIn(blue), run_time=0.4)
            pcs, _ = self.play_moves(self.rules_board, DRAW_GAME[1:5], 1, per_move=(d - 2.0) / 4, counters=(red, blue), placed=placed)
            self.rule_pieces = [self.one] + pcs

        self.beat("On your turn you place one of your pieces, and there are exactly two ways to do it.", opening)

        def new_snake(d):
            cap = self.caption('1.  new snake: touches nothing', INK, 32).to_edge(DOWN, buff=0.35)
            cells = move_cells(DRAW_GAME[5])
            p = piece_on(self.rules_board, cells, P_BLUE)
            p.shift(UP * 3)
            self.add(p)
            self.play(FadeIn(cap), p.animate.shift(DOWN * 3), rate_func=smooth, run_time=d * 0.35)
            placed[1] |= set(cells)
            ring = {n for c in cells for n in neighbours(c)} - set(cells)
            halo = VGroup(*[self.rules_board.cells[c].copy().set_fill(opacity=0).set_stroke(GOOD, 3.5) for c in ring])
            self.play(Create(halo), run_time=d * 0.25)
            self.play(halo.animate.set_stroke(opacity=0.15), run_time=d * 0.15)
            self.play(FadeOut(halo), run_time=0.4)
            self.rule_pieces.append(p)
            self.cap = cap

        self.beat("One: start a new snake. Then the piece must not touch any other piece at all, of either colour.", new_snake)

        def grow(d):
            cap2 = self.caption('2.  grow: touch your own snake', INK, 32).to_edge(DOWN, buff=0.35)
            self.play(FadeOut(self.cap), FadeIn(cap2), run_time=0.5)
            pcs, _ = self.play_moves(self.rules_board, DRAW_GAME[6:9], 6, per_move=d * 0.15, counters=(red, blue), placed=placed)
            self.rule_pieces += pcs
            join_cells = move_cells(DRAW_GAME[9])
            pcs, _ = self.play_moves(self.rules_board, DRAW_GAME[9:10], 9, per_move=d * 0.12, counters=(red, blue), placed=placed)
            self.rule_pieces += pcs
            snake = snake_of(placed[1], join_cells[0])
            glow = VGroup(*[self.rules_board.cells[c].copy().set_fill(opacity=0).set_stroke(GOLD, 4) for c in snake])
            self.play(Create(glow), Flash(blue[1], color=GOLD), run_time=d * 0.15)
            self.play(FadeOut(glow), run_time=0.5)
            self.cap = cap2

        self.beat("Two: grow. The piece touches at least one of your own pieces. It is allowed to touch the opponent as "
                  "well, and it may join two of your snakes together.", grow)

        def snake_shape(d):
            game = VGroup(self.rules_board, *self.rule_pieces, red, blue, self.cap)
            self.play(FadeOut(game), run_time=0.8)
            s = 0.27
            xs = [-5.1, -1.7, 1.7, 5.1]
            patches = VGroup(*[patch(2, s, origin=np.array([x, 0.3, 0])) for x in xs])
            tri = shape_on_patch([(0, 0), (1, 0), (0, 1)], s, P_RED, np.array([xs[0], 0.3, 0]))
            yb = shape_on_patch([(0, 0), (1, 0), (2, 0), (0, -1), (0, -2), (-1, 1), (-2, 2)], s, P_RED, np.array([xs[1], 0.3, 0]))
            path = shape_on_patch([(-2, 1), (-1, 1), (0, 0), (1, -1), (1, -2), (0, -2)], s, P_RED, np.array([xs[2], 0.3, 0]))
            ring = shape_on_patch([(1, 0), (1, -1), (0, -1), (-1, 0), (-1, 1), (0, 1)], s, P_RED, np.array([xs[3], 0.3, 0]))
            names_ = ['triangle', 'branch', 'path', 'ring']
            caps = VGroup(*[self.caption(n, MUTED, 26).move_to(np.array([x, -1.55, 0])) for n, x in zip(names_, xs)])
            self.play(FadeIn(patches), run_time=0.6)
            self.play(FadeIn(tri), FadeIn(caps[0]), run_time=d * 0.08)
            x1 = Cross(tri, stroke_color=BAD_RED, stroke_width=6).scale(1.2)
            self.play(Create(x1), run_time=d * 0.08)
            self.play(FadeIn(yb), FadeIn(caps[1]), run_time=d * 0.08)
            x2 = Cross(yb, stroke_color=BAD_RED, stroke_width=6).scale(0.9)
            self.play(Create(x2), run_time=d * 0.08)
            self.play(FadeIn(path), FadeIn(caps[2]), run_time=d * 0.1)
            ok1 = Text('✓', font=FONT, font_size=60, color=GOOD).next_to(path, UP, buff=0.2)
            self.play(FadeIn(ok1, scale=1.4), run_time=d * 0.06)
            self.play(FadeIn(ring), FadeIn(caps[3]), run_time=d * 0.1)
            ok2 = Text('✓', font=FONT, font_size=60, color=GOOD).next_to(ring, UP, buff=0.2)
            self.play(FadeIn(ok2, scale=1.4), run_time=d * 0.06)
            self.shape_demo = VGroup(patches, tri, yb, path, ring, caps, x1, x2, ok1, ok2)

        self.beat("Whatever you do, your cells must stay snake-shaped: no cell may have more than two of your own "
                  "neighbours, and three of your cells may never all touch each other. No branches, no blobs, no sharp "
                  "turns. Every group of your cells is a path, or a closed ring.", snake_shape)

        def ending(d):
            self.play(FadeOut(self.shape_demo), run_time=0.6)
            self.play(FadeIn(VGroup(self.rules_board, *self.rule_pieces, red, blue)), run_time=0.6)
            pcs, _ = self.play_moves(self.rules_board, DRAW_GAME[10:14], 10, per_move=d * 0.08, counters=(red, blue), placed=placed)
            self.rule_pieces += pcs
            left = loose_piece('4C', MUTED, s=0.3).move_to(LEFT * 5.6 + DOWN * 2.2).set_opacity(0.6)
            pass_txt = self.caption('pass', MUTED, 30).next_to(left, UP, buff=0.2)
            self.play(FadeIn(left), FadeIn(pass_txt), run_time=d * 0.12)
            snakes = []
            for player in (0, 1):
                best = max((snake_of(placed[player], c) for c in placed[player]), key=len)
                snakes.append(VGroup(*[self.rules_board.cells[c].copy().set_fill(opacity=0).set_stroke(GOLD, 4) for c in best]))
            self.play(Create(snakes[0]), Create(snakes[1]), red[1].animate.set_color(GOLD), blue[1].animate.set_color(GOLD),
                      run_time=d * 0.2)
            tie = self.caption('17 : 17  tie', GOLD, 40).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(tie, shift=UP * 0.2), run_time=0.7)
            self.final = VGroup(left, pass_txt, *snakes, tie)

        self.beat("If you cannot place anything, you pass. When neither player can move, the game ends. You measure your "
                  "longest snake in cells, and the longer snake wins. Equal lengths are a tie.", ending)

        def close(d):
            self.play(FadeOut(VGroup(self.final, red, blue)), run_time=d * 0.5)

        self.beat("That's the whole rulebook.", close, pad=1.2)
        self.play(FadeOut(Group(*self.mobjects)), run_time=1.0)


BAD_RED = '#FF6B5E'

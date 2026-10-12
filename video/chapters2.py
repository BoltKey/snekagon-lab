"""Chapters 8-14 of the Snekagon video (see chapters.py for 1-7).

Draft render of one chapter:  .venv/Scripts/manim -ql chapters2.py Ch09Solving
"""
import bisect
import json
import math
import random

from PIL import Image as PILImage

from snekviz import *  # noqa: F401,F403
from script import para, split_at

DATA = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'video_data.json'), encoding='utf-8'))
# real positions behind the big grids (zoom_data.py): coordinator list of red replies, the proven ones,
# the refuted blue answers to the stubborn reply, follow-up moves
ZOOM = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'zoom_data.json'), encoding='utf-8'))
# what a depth-first search over real positions visits (search_trace.py), for the rapidly changing moves
TRACES = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'search_traces.json'), encoding='utf-8'))
# AlphaGo vs Lee Sedol, game 4 (2016), the stones on the board after Lee Sedol's move 78 in playing order (go_pattern.py)
GO = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'go_pattern.json'), encoding='utf-8'))
BASE = ['1:60', '4B:16,27,28,39']  # the opening line we tried to prove
# blue's move 8 in the story game: the 21 moves (of 159) that keep the draw, and the cells they cover
# the snake red threatens to build along the bottom before blue's move 8 (red_shadow.py)
SHADOW = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'red_shadow.json'), encoding='utf-8'))
MOVE8 = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'move8_draws.json'), encoding='utf-8'))
# WIN_GAME (snekviz): one of the sixteen proven replies, played out close; blue wins 22 : 18.
# HARD_GAME (snekviz): the toughest of the sixteen proofs (241 hours), played out close; blue wins 21 : 15.

# engine analysis after red's centre opening (blue to move), win probability for blue
OPENING_ANALYSIS = [('4B:16,27,28,39', 65), ('3I:17,28,39', 54), ('4A:51,52,63,74', 50), ('3V:36,37,48', 50), ('4B:20,31,32,43', 48)]
# exact snake-length bounds [lower, upper] for red and blue after k moves of the won game (engine 'set')
WIN_BOUNDS = {4: ((5, 25), (8, 24)), 5: ((5, 25), (8, 24)), 6: ((5, 25), (12, 24)), 7: ((8, 25), (12, 24)), 8: ((8, 25), (15, 24)),
              9: ((12, 20), (15, 24)), 10: ((12, 20), (18, 24)), 11: ((12, 20), (18, 24)), 12: ((12, 20), (22, 24)),
              13: ((18, 20), (22, 24)), 14: ((18, 18), (22, 22)), 15: ((18, 18), (22, 22))}
# per position of the won game: the remaining pieces of each player placed to reach its upper bound (bound_shadows.py)
BOUND_SHADOWS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'bound_shadows.json'), encoding='utf-8'))
RED_FIRST_MOVES = [5, 6, 7, 16, 17, 18, 27, 28, 38, 39, 49, 60]  # the 12 different cells for the 1-hex


def tick_mark(color=GOOD, size=40):
    return Text('✓', font=FONT, font_size=size, color=color)


def card(text, color=INK, width=3.2, height=1.1, size=24, fill=None):
    box = RoundedRectangle(width=width, height=height, corner_radius=0.12, stroke_color=color, stroke_width=2.5)
    if fill:
        box.set_fill(fill, 0.18)
    t = Paragraph(*wrap(text, max(8, int((width - 0.4) / (size * 0.0058)))), font=FONT, font_size=size, color=color, alignment='center', line_spacing=0.8)
    if t.width > width - 0.3:
        t.scale_to_fit_width(width - 0.3)
    t.move_to(box)
    return VGroup(box, t)


def worker(label, color):
    """The home laptop that ran the solve, drawn as a flower of seven cells that fill up with solved jobs."""
    s = 0.42
    core = VGroup(*[hex_shape(hex_xy(q, r, s), s, fill=EMPTY, stroke=color, sw=2.5) for q, r in [(0, 0)] + DIRS])
    if not label:
        return VGroup(core)
    return VGroup(core, caption(label, MUTED, 22).next_to(core, DOWN, buff=0.3))


def tiny_grid(n, cols, size=0.12, gap=0.04, color=MUTED):
    g = VGroup(*[Square(size, stroke_width=0).set_fill(color, 0.6) for _ in range(n)])
    g.arrange_in_grid(cols=cols, buff=gap)
    return g


# ---- chapter-card illustrations ------------------------------------------------------------------
def art_engine():
    b, pcs = board_with(DRAW_GAME, 5, 0.17)
    bars = VGroup(*[Rectangle(width=w, height=0.22, stroke_width=0).set_fill(GOLD if k == 0 else MUTED, 0.9) for k, w in enumerate((2.0, 1.6, 1.4))])
    bars.arrange(DOWN, buff=0.18, aligned_edge=LEFT).next_to(b, RIGHT, buff=0.5)
    return VGroup(b, pcs, bars)


def art_laptop():
    core = worker('', P_BLUE)[0]
    for c in (0, 2, 3, 5):
        core[c].set_fill(GOOD, 0.55)
    return core


def art_queue():
    cells = tiny_grid(76, 19, size=0.22, gap=0.06)
    for c in cells[:-1]:
        c.set_fill(P_RED, 0.5)
    cells[-1].set_fill(GOLD, 1)
    return VGroup(cells, caption('76', GOLD, 48, BOLD).next_to(cells, DOWN, buff=0.3))


def art_andor():
    g = VGroup()
    top = UP * 1.3
    reds = [top + np.array([x, -1.0, 0]) for x in (-2.1, -0.7, 0.7, 2.1)]
    for r in reds:
        g.add(Line(top, r, color=P_RED, stroke_width=4))
    blues = [reds[1] + np.array([x, -1.0, 0]) for x in (-0.6, 0, 0.6)]
    for k, bl in enumerate(blues):
        g.add(Line(reds[1], bl, color=P_BLUE if k == 1 else FAINT, stroke_width=4 if k == 1 else 2))
    for x in (-0.6, -0.2, 0.2, 0.6):
        g.add(Line(blues[1], blues[1] + np.array([x, -0.9, 0]), color=P_RED, stroke_width=3))
    return g


def art_mirror():
    """Two snakes, mirror images of each other: is the game fair?"""
    cells = [(0, 0), (1, -1), (1, -2), (2, -3), (2, -4)]
    red = shape_on_patch(cells, 0.3, P_RED)
    blue = shape_on_patch(cells, 0.3, P_BLUE).flip(UP)
    axis = DashedLine(UP * 1.5, DOWN * 1.5, color=MUTED, dash_length=0.12)
    return VGroup(red, axis, blue).arrange(RIGHT, buff=0.6)


def art_cards():
    return VGroup(*[hex_shape(ORIGIN, 0.95, fill=c, fill_opacity=0.15, stroke=c, sw=3, scale=1.0)
                    for c in (P_RED, GOLD, P_BLUE)]).arrange(RIGHT, buff=0.3)


# ================================================================================================
# 8. teaching a machine to play
# ================================================================================================
class Ch08Engine(Narrated):
    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath

        def first_session(d):
            term = RoundedRectangle(width=5.4, height=2.2, corner_radius=0.12, stroke_color=FAINT, stroke_width=2).set_fill('#0D1014', 1)
            term.to_corner(UL, buff=0.5)
            cmds = VGroup(*[Text(t, font='Consolas', font_size=22, color=GOOD if t.startswith('$') else INK)
                            for t in ('$ snek perft 2', 'perft 2 = 194388', '$ node ui/server.js 8080')])
            cmds.arrange(DOWN, aligned_edge=LEFT, buff=0.15).move_to(term).align_to(term, LEFT).shift(RIGHT * 0.3)
            self.play(FadeIn(term), run_time=0.5)
            for c in cmds:
                self.type_text(c, 0.7)
            self.wait(0.6)
            self.play(FadeOut(VGroup(term, cmds)), run_time=0.5)
            b, pcs = board_with(DRAW_GAME, 1, 0.3)
            ui = VGroup(b, pcs).to_edge(LEFT, buff=0.6)
            rows = VGroup()
            for k, (m, pct) in enumerate(OPENING_ANALYSIS):
                bar = Rectangle(width=4.2 * pct / 100, height=0.32, stroke_width=0).set_fill(GOLD if k == 0 else P_BLUE, 0.85)
                rows.add(VGroup(caption(m, INK, 22), bar, caption('%d %%' % pct, INK, 22)).arrange(RIGHT, buff=0.25))
            rows.arrange(DOWN, aligned_edge=LEFT, buff=0.3).next_to(ui, RIGHT, buff=0.8)
            head = caption('blue\u2019s chance to win', MUTED, 24).next_to(rows, UP, buff=0.35, aligned_edge=LEFT)
            self.play(FadeIn(ui), run_time=0.8)
            self.play(FadeIn(head), LaggedStart(*[FadeIn(r, shift=RIGHT * 0.2) for r in rows], lag_ratio=0.25), run_time=d * 0.3)
            best = piece_on(b, move_cells(OPENING_ANALYSIS[0][0]), P_BLUE)
            self.click(0.5)
            self.play(FadeIn(best), Create(outline_on(b, move_cells(OPENING_ANALYSIS[0][0]))), run_time=1.0)

        self.beat(para(8, 0), first_session)
        self.clear_all(0.6)

        def mcts(d):
            b, pcs = board_with(DRAW_GAME, 4, 0.22)
            g = VGroup(b, pcs).shift(LEFT * 4.2)
            self.play(FadeIn(g), run_time=0.6)
            rnd = random.Random(4)
            lines, dots = VGroup(), VGroup()
            src = g.get_right() + RIGHT * 0.1
            for k in range(60):
                y = -3.2 + 6.4 * (k + 0.5) / 60
                end = np.array([5.8, y, 0])
                path = VMobject(stroke_color=INK, stroke_width=1, stroke_opacity=0.35)
                pts = [src]
                for j in range(1, 6):
                    t = j / 6
                    pts.append(src + (end - src) * t + np.array([0, rnd.uniform(-0.35, 0.35), 0]))
                pts.append(end)
                path.set_points_smoothly(pts)
                lines.add(path)
                dots.add(Dot(end, radius=0.06, color=P_RED if rnd.random() < 0.45 else P_BLUE))
            self.play(LaggedStart(*[Create(l) for l in lines], lag_ratio=0.02), run_time=d * 0.35)
            self.play(LaggedStart(*[FadeIn(dt, scale=2) for dt in dots], lag_ratio=0.01), run_time=d * 0.15)
            note = caption('blind to walls', GOLD, 30).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(note), run_time=0.8)

        self.beat(para(8, 1), mcts)
        self.clear_all(0.6)

        def handmade(d):
            b, pcs = board_with(RULES_GAME, 8, 0.32)
            g = VGroup(b, pcs).shift(LEFT * 2.6)
            self.play(FadeIn(g), run_time=0.6)
            claim = territory(RULES_GAME, 8)
            tints = VGroup(*[b.cells[c].copy().set_fill(P_RED if pl == 0 else P_BLUE, 0.35).set_stroke(width=0)
                             for pl in (0, 1) for c in claim[pl]])
            self.play(LaggedStart(*[FadeIn(t) for t in tints], lag_ratio=0.01), run_time=d * 0.3)
            lab = caption('who gets where first', GOLD, 30).next_to(g, RIGHT, buff=0.6).shift(UP * 1.2)
            score = VGroup(caption('hand-made judge', INK, 28), caption('2 : 10', INK, 54, BOLD), caption('random playouts', INK, 28)).arrange(DOWN, buff=0.15)
            score.next_to(lab, DOWN, buff=0.8)
            self.play(FadeIn(lab), run_time=0.6)
            self.until('it did even worse')
            self.play(FadeIn(score, shift=UP * 0.2), run_time=0.8)
            self.hm = (g, VGroup(*tints, lab, score))

        self.beat(para(8, 2), handmade)
        g, extra = self.hm
        g.save_state()
        g.saved_state.shift(LEFT * 0.8)  # where the features are shown
        self.play(FadeOut(extra), g.animate.scale(0.38).to_corner(UL, buff=0.3), run_time=0.8)

        def learned(d):
            ax = Axes(x_range=[-6, 6, 2], y_range=[-0.15, 1.15, 0.5], x_length=9.5, y_length=4.6, tips=False,
                      axis_config={'include_numbers': False, 'color': FAINT, 'stroke_width': 2}).shift(DOWN * 0.3)
            rnd = random.Random(1)
            dots = VGroup()
            for z, v in DATA['scatter']:
                z = max(-5.9, min(5.9, z))
                y = (1 if v > 0 else 0 if v < 0 else 0.5) + rnd.uniform(-0.06, 0.06)
                dots.add(Dot(ax.c2p(z, y), radius=0.035, color=P_BLUE if v > 0 else P_RED if v < 0 else INK, fill_opacity=0.7))
            curve = ax.plot(lambda x: 1 / (1 + math.exp(-x)), x_range=[-6, 6], color=GOLD, stroke_width=5)
            labels = VGroup(caption('won', P_BLUE, 24).next_to(ax.c2p(-6, 1), LEFT, buff=0.2),
                            caption('lost', P_RED, 24).next_to(ax.c2p(-6, 0), LEFT, buff=0.2),
                            caption('learned score', MUTED, 24).next_to(ax, DOWN, buff=0.2))
            title = caption('%s solved positions' % '{:,}'.format(DATA['n_labels']), INK, 30).to_edge(UP, buff=0.4)
            self.play(Create(ax), FadeIn(labels), FadeIn(title), run_time=0.8)
            self.play(LaggedStart(*[FadeIn(dt) for dt in dots], lag_ratio=0.002), run_time=d * 0.18)
            pw = caption('P(win)', GOLD, 28).next_to(ax.c2p(3.5, 0.97), UP, buff=0.1)
            self.until('a logistic regression')
            self.play(Create(curve), FadeIn(pw), run_time=d * 0.1)
            chart = VGroup(ax, dots, curve, labels, title, pw)

            # a few of the features, on the position from the hand-made judge above
            self.until('The features are simple things')
            self.play(FadeOut(chart), run_time=0.6)
            g = self.hm[0]
            b, pcs = g
            head = caption('13 features', MUTED, 26).move_to(np.array([3.2, 2.9, 0]))
            self.play(Restore(g), FadeIn(head), run_time=0.8)
            placed = position_after(RULES_GAME, 8)
            claim = territory(RULES_GAME, 8)

            def snake_info(player):
                mine, other = placed[player], placed[1 - player]
                body = {c for c in mine if sum(1 for n in neighbours(c) if n in mine) >= 2}
                ends = {c for c in mine if sum(1 for n in neighbours(c) if n in mine) <= 1}
                empty = set(CELLS) - mine - other
                growable = {c for c in empty if not any(n in body for n in neighbours(c))}
                live = {e for e in ends if any(n in growable for n in neighbours(e))}
                return ends, live

            def ring(cells, color, width=4):
                return VGroup(*[b.cells[c].copy().set_fill(opacity=0).set_stroke(color, width) for c in cells])

            longest = VGroup(*[ring(max(components(placed[pl]), key=len), GOLD) for pl in (0, 1)])
            end_marks = VGroup()
            live_counts = []
            for pl in (0, 1):
                ends, live = snake_info(pl)
                live_counts.append(len(live))
                for e in ends:
                    if e in live:
                        end_marks.add(Dot(b.at(e), radius=0.09, color=GOOD))
                    else:
                        end_marks.add(Cross(b.cells[e], stroke_color=BAD, stroke_width=4).scale(0.45))
            tints = VGroup(*[b.cells[c].copy().set_fill(P_RED if pl == 0 else P_BLUE, 0.4).set_stroke(width=0) for pl in (0, 1) for c in claim[pl]])
            outlines = VGroup(*[ring(comp, WHITE, 3) for pl in (0, 1) for comp in components(placed[pl])])
            features = [
                ('the length of each', 'longest snake', longest_snake(placed[0]), longest_snake(placed[1]), longest),
                ('how many snake ends', 'snake ends that can still grow', live_counts[0], live_counts[1], end_marks),
                ('who reaches which', 'empty cells reached first', len(claim[0]), len(claim[1]), tints),
                ('how many separate snakes', 'separate snakes', len(components(placed[0])), len(components(placed[1])), outlines),
            ]
            rows, shown = VGroup(), None
            for k, (phrase, name, r, bl, marks) in enumerate(features):
                row = VGroup(caption(name, INK, 26),
                             VGroup(caption('%d' % r, P_RED, 26, BOLD), caption('%d' % bl, P_BLUE, 26, BOLD)).arrange(RIGHT, buff=0.5))
                row.arrange(DOWN, aligned_edge=LEFT, buff=0.1).move_to(np.array([0.9, 1.9 - 1.25 * k, 0]), aligned_edge=LEFT)
                self.until(phrase)
                anims = [FadeIn(row, shift=RIGHT * 0.2), FadeIn(marks)]
                if shown is not None:
                    anims += [FadeOut(shown), rows[-1][0].animate.set_color(MUTED)]
                self.play(*anims, run_time=0.8)
                rows.add(row)
                shown = marks
            self.until('With that learned judgement')
            self.play(FadeOut(VGroup(g, head, rows, shown)), run_time=0.6)
            # all of them (red's value minus blue's; tools/fit_eval.py), the four shown above in gold
            names = [('longest snake', True), ('best snake + the room in front of it', False),
                     ('best group of own cells + its room', False), ('cells in hand without room for them', False),
                     ('most cells a snake can still reach', False), ('cells still in hand', False),
                     ('empty cells reached first (up to 20)', False), ('empty cells reached first', True),
                     ('room in front of the best snake', False), ('separate snakes', True),
                     ('snake ends that can still grow', True), ('largest untouched region', False), ('whose turn it is', False)]
            items = VGroup(*[caption('%d.  %s' % (k + 1, n), GOLD if shown_ else INK, 24) for k, (n, shown_) in enumerate(names)])
            cols = VGroup(items[:7].copy().arrange(DOWN, aligned_edge=LEFT, buff=0.22), items[7:].copy().arrange(DOWN, aligned_edge=LEFT, buff=0.22))
            cols.arrange(RIGHT, buff=0.8, aligned_edge=UP)
            title2 = caption('all 13 features', INK, 32, BOLD)
            VGroup(title2, cols).arrange(DOWN, buff=0.4).move_to(UP * 0.5)
            self.play(FadeIn(title2), LaggedStart(*[FadeIn(i, shift=RIGHT * 0.1) for col in cols for i in col], lag_ratio=0.08), run_time=2.0)
            score = VGroup(caption('learned judge', INK, 26), caption('16 : 0', GOLD, 50, BOLD), caption('random playouts', INK, 26)).arrange(DOWN, buff=0.12)
            score.to_corner(DR, buff=0.4)
            self.play(FadeIn(score, shift=UP * 0.2), run_time=0.8)

        self.beat(para(8, 3), learned, pad=2.5)
        self.clear_all()


# ================================================================================================
# 9. trying to solve it
# ================================================================================================
TRICKS = ['speed', 'memory', 'bounds', 'order']


class Ch09Solving(Narrated):
    def trick_bar(self, active):
        items = VGroup(*[caption('%d. %s' % (k + 1, t), GOLD if k == active else FAINT, 26, BOLD if k == active else NORMAL)
                         for k, t in enumerate(TRICKS)]).arrange(RIGHT, buff=0.6).to_edge(UP, buff=0.3)
        return items

    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath

        def intro(d):
            a = card('Playing well: probably good', MUTED, 5.2, 1.6, 28)
            b = card('Solving: proven', GOLD, 5.2, 1.6, 28)
            VGroup(a, b).arrange(RIGHT, buff=0.8)
            self.play(FadeIn(a, shift=UP * 0.2), run_time=d * 0.3)
            self.play(FadeIn(b, shift=UP * 0.2), run_time=d * 0.3)

        self.beat(para(9, 0), intro)
        self.clear_all(0.6)

        def tree_size(d):
            # the cheap trick first: twelve rotated and mirrored copies of a position are really one
            base = {60: 0, 16: 1, 27: 1, 28: 1, 39: 1}
            boards = VGroup()
            for t in range(12):
                b = Board(s=0.09)
                cells = {}
                for c, pl in base.items():
                    q, r = apply_symmetry(t, *qr(c))
                    cells.setdefault(pl, []).append(idx(q, r))
                boards.add(VGroup(b, piece_on(b, cells[0], P_RED, 0.09), piece_on(b, cells[1], P_BLUE, 0.09)))
            boards.arrange_in_grid(rows=2, cols=6, buff=0.35).shift(UP * 0.6)
            # the position first, then its eleven rotated and mirrored copies, slowly, one by one
            self.play(FadeIn(boards[0], scale=0.8), run_time=0.8)
            text, start, dur = self._beat
            left = start + dur * text.index('so up to twelve') / len(text) - self.renderer.time
            self.play(LaggedStart(*[FadeIn(bd, scale=0.8) for bd in boards[1:]], lag_ratio=0.35), run_time=max(2.0, left))
            self.until('are really one')
            self.play(*[bd.animate.move_to(UP * 0.6).set_opacity(0.0) for bd in boards[1:]],
                      boards[0].animate.move_to(UP * 0.6).scale(2.2), run_time=1.2)
            first = caption('first moves: 91 \u2192 12', GOLD, 30)
            second = caption('replies: 2,132 \u2192 172', GOLD, 30)
            VGroup(first, second).arrange(DOWN, buff=0.2).to_edge(DOWN, buff=0.5)
            self.until('That leaves red just 12')
            self.play(FadeIn(first), run_time=0.6)
            self.until('blue has 172')
            self.play(FadeIn(second), run_time=0.6)
            # but only at the very start: the tree keeps branching
            self.until('But symmetry only helps')
            self.play(FadeOut(VGroup(boards[0], first, second)), *[FadeOut(bd) for bd in boards[1:]], run_time=0.6)
            counts = ['12', '172', '\u2248 1,600', '\u2248 930', '\u2248 680', '\u2248 360', '\u2026']
            levels = VGroup()
            for k, c in enumerate(counts):
                n = min(2 ** (k + 1), 64)
                dots = VGroup(*[Dot(radius=0.05, color=INK, fill_opacity=max(0.15, 1 - 0.13 * k)) for _ in range(n)])
                dots.arrange(RIGHT, buff=max(0.03, 0.6 - 0.09 * k))
                if dots.width > 10:
                    dots.scale_to_fit_width(10)
                levels.add(VGroup(dots, caption(c, GOLD if k < 2 else MUTED, 24)))
            for k, lv in enumerate(levels):
                lv[0].move_to(np.array([-1.0, 3.0 - 0.85 * k, 0]))
                lv[1].move_to(np.array([5.6, 3.0 - 0.85 * k, 0]))
            self.play(LaggedStart(*[FadeIn(lv) for lv in levels], lag_ratio=0.5), run_time=max(2.5, d * 0.25))
            self.until('Even counting rotated')
            big = caption('\u2248 10\u00b2\u2079 games', GOLD, 48, BOLD).to_edge(DOWN, buff=0.4)
            self.play(FadeIn(big, scale=1.2), run_time=1.0)

        self.beat(para(9, 1), tree_size)
        self.clear_all(0.6)

        self.beat(para(9, 2), lambda d: self.play(FadeIn(self.trick_bar(-1)), run_time=1.0))

        def bits_of(moves):
            placed = position_after(moves, len(moves))
            return placed[0], placed[1]

        def hex_number(cells, color):
            value = sum(1 << i for i in cells)
            return Text('0x%032x' % value, font='Consolas', font_size=17, color=color)

        def speed(d):
            bar = self.trick_bar(0)
            self.play(Transform(self.mobjects[0], bar), run_time=0.5)
            b = Board(s=0.22).move_to(LEFT * 4.4 + UP * 0.55)
            grid = VGroup()
            for i in range(121):
                sq = Square(0.25, stroke_width=1, stroke_color=FAINT).set_fill(EMPTY if i in CELLS else BG, 1)
                sq.move_to(np.array([(i % 11) * 0.28, -(i // 11) * 0.28, 0]))
                grid.add(sq)
            grid.move_to(RIGHT * 0.9 + UP * 0.55)
            glab = caption('11 × 11 bitmap', MUTED, 22).next_to(grid, UP, buff=0.12)
            # bit 120 on the left, bit 0 on the right: the strips read like the numbers below them
            strips, numbers, names = VGroup(), VGroup(), VGroup()
            for player, y in ((0, -2.45), (1, -3.2)):
                col = P_RED if player == 0 else P_BLUE
                strip = VGroup(*[Square(0.045, stroke_width=0).set_fill(FAINT, 1) for _ in range(121)]).arrange(RIGHT, buff=0.007)
                strips.add(strip.move_to(np.array([-1.9, y, 0])))
                names.add(hex_shape(ORIGIN, 0.14, fill=col, stroke=BG, sw=1, scale=1.0).next_to(strip, LEFT, buff=0.25))  # whose bits
            state = {}

            def show(moves, run_time):
                red_cells, blue_cells = bits_of(moves)
                anims = []
                pcs = VGroup(*[piece_on(b, move_cells(m), P_RED if j % 2 == 0 else P_BLUE) for j, m in enumerate(moves) if m != 'pass'])
                if 'pieces' in state:
                    anims += [FadeOut(state['pieces']), FadeIn(pcs)]
                else:
                    anims.append(FadeIn(pcs))
                state['pieces'] = pcs
                for i in range(121):
                    colour = P_RED if i in red_cells else P_BLUE if i in blue_cells else (EMPTY if i in CELLS else BG)
                    anims.append(grid[i].animate.set_fill(colour, 1))
                for player, cells in ((0, red_cells), (1, blue_cells)):
                    col = P_RED if player == 0 else P_BLUE
                    num = hex_number(cells, col).next_to(strips[player], RIGHT, buff=0.3)
                    if len(numbers) > player:  # the numbers are on screen: animate them
                        anims += [strips[player][120 - i].animate.set_fill(col if i in cells else FAINT, 1) for i in range(121)]
                        anims.append(Transform(numbers[player], num))
                    else:
                        for i in range(121):
                            strips[player][120 - i].set_fill(col if i in cells else FAINT, 1)
                        numbers.add(num)
                self.play(*anims, run_time=run_time)
                return red_cells

            self.play(FadeIn(b), FadeIn(grid), FadeIn(glab), run_time=0.8)
            show(DRAW_GAME[:5], d * 0.1)
            self.until('one bit per cell')
            self.play(FadeIn(strips), FadeIn(names), FadeIn(numbers), run_time=0.8)
            self.until('Every position the solver meets')
            for moves in (RULES_GAME[:8], SKETCH_GAME[:8], WIN_GAME[:14], WIN_GAME[:9]):
                red_cells = show(moves, 0.8)
                self.wait(0.5)
            snake = max(components(red_cells), key=len)
            around = {n for c in snake for n in neighbours(c)} - set(snake)
            glow_b = VGroup(*[b.cells[c].copy().set_fill(GOLD, 0.55).set_stroke(width=0) for c in around])
            self.until('Finding all the neighbours')
            outline = VGroup(*[b.cells[c].copy().set_fill(opacity=0).set_stroke(GOLD, 3) for c in snake])
            self.play(Create(outline), run_time=0.6)
            self.play(FadeIn(glow_b), *[grid[i].animate.set_fill(GOLD, 0.8) for i in around],
                      *[strips[0][120 - i].animate.set_fill(GOLD, 1) for i in around], run_time=d * 0.12)
            note = caption('6 shifts', GOLD, 26).next_to(grid, RIGHT, buff=0.35)
            self.play(FadeIn(note), run_time=0.6)

        self.beat(para(9, 3), speed)
        keep = self.mobjects[0]
        self.play(*[FadeOut(m) for m in self.mobjects[1:]], run_time=0.6)


        def memory(d):
            self.play(Transform(keep, self.trick_bar(1)), run_time=0.5)
            # moves 11 and 13 of the game from the rules can be swapped: both orders reach the same position
            base = RULES_GAME[:10]
            order_a = [RULES_GAME[10], RULES_GAME[11], RULES_GAME[12]]
            order_b = [RULES_GAME[12], RULES_GAME[11], RULES_GAME[10]]
            rows = VGroup()
            for seq, y in ((order_a, 1.4), (order_b, -1.6)):
                row = VGroup()
                for j in range(3):
                    moves = base + seq[:j + 1]
                    b, pcs = board_with(moves, len(moves), 0.12)
                    row.add(VGroup(b, pcs))
                row.arrange(RIGHT, buff=0.6).move_to(np.array([-2.6, y, 0]))
                rows.add(row)
            final_b, final_p = board_with(RULES_GAME, 13, 0.2)
            final = VGroup(final_b, final_p).move_to(RIGHT * 4.4 + DOWN * 0.1)
            self.play(LaggedStart(*[FadeIn(x) for x in rows[0]], lag_ratio=0.5), run_time=d * 0.2)
            self.play(LaggedStart(*[FadeIn(x) for x in rows[1]], lag_ratio=0.5), run_time=d * 0.2)
            arrows = VGroup(Arrow(rows[0][-1].get_right(), final.get_left() + UP * 0.6, color=MUTED, buff=0.1),
                            Arrow(rows[1][-1].get_right(), final.get_left() + DOWN * 0.6, color=MUTED, buff=0.1))
            self.play(FadeIn(final), Create(arrows), run_time=0.8)
            tag = card('already solved', GOOD, 3.8, 0.8, 26).next_to(final, DOWN, buff=0.2)
            self.play(FadeIn(tag, shift=UP * 0.2), run_time=0.6)

        self.beat(para(9, 4), memory)
        self.play(*[FadeOut(m) for m in self.mobjects if m is not keep], run_time=0.6)

        def bounds(d):
            self.play(Transform(keep, self.trick_bar(2)), run_time=0.5)
            board = Board(s=0.27).shift(LEFT * 3.9 + DOWN * 0.2)
            pcs = VGroup(*[piece_on(board, move_cells(m), P_RED if j % 2 == 0 else P_BLUE) for j, m in enumerate(WIN_GAME[:4])])
            self.play(FadeIn(board), FadeIn(pcs), run_time=0.6)
            ax = NumberLine(x_range=[0, 25, 5], length=6.5, color=FAINT, include_numbers=False).move_to(RIGHT * 3.3 + DOWN * 1.3)
            ticks = VGroup(*[caption(str(v), MUTED, 20).next_to(ax.n2p(v), DOWN, buff=0.15) for v in range(0, 26, 5)])

            def bars(k):
                (rl, ru), (bl, bu) = WIN_BOUNDS[k]
                g = VGroup()
                for (lo, hi), col, y in (((rl, ru), P_RED, 0.6), ((bl, bu), P_BLUE, -0.4)):
                    sure = Rectangle(width=max(0.02, ax.n2p(lo)[0] - ax.n2p(0)[0]), height=0.42, stroke_width=0).set_fill(col, 1)
                    sure.align_to(ax.n2p(0), LEFT).set_y(ax.get_y() + 1.0 + y)
                    maybe = Rectangle(width=max(0.02, ax.n2p(hi)[0] - ax.n2p(lo)[0]), height=0.42, stroke_color=col, stroke_width=2).set_fill(col, 0.18)
                    maybe.next_to(sure, RIGHT, buff=0)
                    g.add(VGroup(sure, maybe))
                return g

            def shadows(k):
                # each player's remaining pieces placed to reach its upper bound, drawn like the "still possible" bars
                g = VGroup()
                for row, col in zip(BOUND_SHADOWS[str(k)], (P_RED, P_BLUE)):
                    for m in row['moves']:
                        cells = move_cells(m)
                        g.add(VGroup(*[RegularPolygon(6, start_angle=PI / 6, radius=board.scale_s * 1.004).move_to(board.at(c))
                                       .set_fill(col, 0.22).set_stroke(width=0) for c in cells],
                                     outline_on(board, cells, col, 2)))
                return g

            legend = VGroup(caption('sure length', INK, 22), caption('still possible', MUTED, 22)).arrange(RIGHT, buff=0.6).next_to(ax, UP, buff=2.2)
            cur, ghost = bars(4), shadows(4)
            self.play(Create(ax), FadeIn(ticks), FadeIn(cur), FadeIn(legend), run_time=0.8)
            self.play(FadeIn(ghost), run_time=0.6)
            placed_pcs = []
            for k in range(4, 12):  # after move 12 blue is sure of 22, red can reach at most 20
                m = WIN_GAME[k]
                p = piece_on(board, move_cells(m), P_RED if k % 2 == 0 else P_BLUE)
                placed_pcs.append(p)
                step = max(0.9, d * 0.06)
                self.click(step * 0.5)
                nxt = shadows(k + 1)
                self.play(FadeIn(p, scale=1.2), FadeOut(ghost), FadeIn(nxt), Transform(cur, bars(k + 1)), run_time=step)
                ghost = nxt
            stamp = card('decided: blue wins', GOLD, 4.0, 0.9, 30).next_to(ax, DOWN, buff=0.6)
            note = caption('3 moves to go', MUTED, 24).next_to(stamp, DOWN, buff=0.15)
            self.play(FadeIn(stamp, scale=1.2), FadeIn(note), run_time=0.8)

        self.beat(para(9, 5), bounds)
        self.play(*[FadeOut(m) for m in self.mobjects if m is not keep], run_time=0.6)

        def order(d):
            self.play(Transform(keep, self.trick_bar(3)), run_time=0.5)
            cards = VGroup(*[card('%s %d %%' % (m, p), INK, 2.05, 1.3, 22) for m, p in OPENING_ANALYSIS])
            cards.add(card('… and the rest', MUTED, 2.05, 1.3, 22))
            cards.arrange(RIGHT, buff=0.15).shift(DOWN * 0.2)
            self.play(LaggedStart(*[FadeIn(c, shift=UP * 0.2) for c in cards], lag_ratio=0.15), run_time=d * 0.3)
            first = SurroundingRectangle(cards[0], color=GOLD, buff=0.08, stroke_width=4)
            self.play(Create(first), run_time=0.6)
            self.play(*[c.animate.set_opacity(0.25) for c in cards[1:]], run_time=d * 0.15)
            note = caption('one winning move is enough', GOLD, 28).to_edge(DOWN, buff=0.5)
            self.play(FadeIn(note), run_time=0.8)

        self.beat(para(9, 6), order)
        self.play(*[FadeOut(m) for m in self.mobjects], run_time=0.6)

        def pilot(d):
            b, pcs = board_with(['1:60', '4B:16,27,28,39', '4A:7,8,9,20', '4I:15,25,35,45'], 4, 0.3)
            g = VGroup(b, pcs).shift(LEFT * 3.6)
            self.play(FadeIn(g), run_time=0.6)
            tracker = ValueTracker(0)
            num = always_redraw(lambda: caption('{:,}'.format(int(tracker.get_value())), INK, 52, BOLD).move_to(RIGHT * 3.0 + UP * 1.0))
            lab = caption('positions checked', MUTED, 26).move_to(RIGHT * 3.0 + UP * 0.2)
            self.add(num)
            self.play(FadeIn(lab), tracker.animate(rate_func=rush_into).set_value(11046596211),
                      self.search_flicker(b, TRACES['pilot'], d * 0.45), run_time=d * 0.45)
            stamp = card('all 956 red moves lose', GOLD, 5.6, 1.0, 26).move_to(RIGHT * 3.0 + DOWN * 1.2)
            cost = caption('≈ 10 hours, one core', MUTED, 26).next_to(stamp, DOWN, buff=0.3)
            self.play(FadeIn(stamp, scale=1.15), run_time=0.8)
            self.play(FadeIn(cost), run_time=0.6)

        self.beat(para(9, 7), pilot, pad=1.2)
        self.clear_all()


# ================================================================================================
# 10. one opening, 1,589 replies
# ================================================================================================
class Ch10Opening(Narrated):
    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath

        def opening(d):
            board = Board(s=0.32).shift(LEFT * 3.2)
            self.play(FadeIn(board), run_time=0.6)
            marks = VGroup(*[board.cells[c].copy().set_fill(GOLD, 0.25 if c != 60 else 0.6).set_stroke(GOLD, 3) for c in RED_FIRST_MOVES])
            lab = caption('12 different first moves', GOLD, 26).next_to(board, DOWN, buff=0.2)
            self.play(LaggedStart(*[FadeIn(m) for m in marks], lag_ratio=0.1), FadeIn(lab), run_time=d * 0.15)
            red1 = piece_on(board, [60], P_RED)
            blue = piece_on(board, move_cells(OPENING_ANALYSIS[0][0]), P_BLUE)
            self.play(FadeOut(marks), FadeOut(lab), FadeIn(red1), run_time=0.8)
            self.click(0.5)
            self.play(FadeIn(blue, scale=1.2), Create(outline_on(board, move_cells(OPENING_ANALYSIS[0][0]))), run_time=1.0)
            est = caption('engine: 65 % for blue', P_BLUE, 34, BOLD).move_to(RIGHT * 3.4 + UP * 1.2)
            nxt = caption('next best: 54 %', MUTED, 26).next_to(est, DOWN, buff=0.25)
            self.play(FadeIn(est), FadeIn(nxt), run_time=d * 0.12)
            players = caption('players: “the first player wins”', P_RED, 28).next_to(nxt, DOWN, buff=0.9)
            q = caption('?', GOLD, 60, BOLD).next_to(players, DOWN, buff=0.2)
            self.until('goes straight against')
            self.play(FadeIn(players), FadeIn(q, scale=1.3), run_time=1.0)
            claim = card('goal: prove blue wins', GOLD, 5.6, 0.9, 28).to_edge(DOWN, buff=0.35).shift(RIGHT * 3.4)
            self.until('so we tried to actually prove')
            self.play(FadeIn(claim, shift=UP * 0.2), run_time=0.8)

        self.beat(para(10, 0), opening)
        self.clear_all(0.6)

        def fan(d):
            replies = ZOOM['replies']
            grid = tiny_grid(len(replies), 53, size=0.12, gap=0.04).move_to(LEFT * 2.3 + DOWN * 0.15)
            title = caption('1,589 red replies', INK, 30).next_to(grid, UP, buff=0.3)
            self.play(FadeIn(title), LaggedStart(*[FadeIn(c) for c in grid], lag_ratio=0.0006), run_time=d * 0.18)
            zooms = VGroup()
            for k, y in ((0, 2.3), (600, 0), (len(replies) - 1, -2.3)):
                view = position_board(BASE + [replies[k]], 0.08, [(replies[k], GOLD)])
                zooms.add(self.zoom_in(grid[k], zoom_card(view, 'reply %s' % '{:,}'.format(k + 1)), np.array([4.9, y, 0]), run_time=0.9))
            self.until('For every single one')
            r2, by = ZOOM['followups']['r2'], ZOOM['followups']['by']
            b, pcs = board_with(BASE + [r2], 3, 0.2)
            main = VGroup(b, pcs).move_to(LEFT * 4.3 + UP * 0.3)
            self.play(FadeOut(zooms), FadeOut(title), FadeOut(grid), FadeIn(main), Create(outline_on(b, move_cells(r2), GOLD, 4)), run_time=0.9)
            answer = piece_on(b, move_cells(by), P_BLUE)
            ans_lab = caption('one blue answer', P_BLUE, 28).next_to(main, DOWN, buff=0.25)
            self.click(0.4)
            self.play(FadeIn(answer, scale=1.2), Create(outline_on(b, move_cells(by))), FadeIn(ans_lab), run_time=0.9)
            self.until('and then a proof')
            follow = ZOOM['followups']['moves']
            fg = tiny_grid(len(follow), 39, size=0.1, gap=0.035, color=P_RED).move_to(RIGHT * 3.4 + UP * 1.0)
            head = caption('%d red follow-ups' % len(follow), P_RED, 26).next_to(fg, UP, buff=0.2)
            self.play(FadeIn(head), LaggedStart(*[FadeIn(c) for c in fg], lag_ratio=0.001), run_time=1.2)
            fz = VGroup()
            for k, x in ((0, 1.9), (500, 4.9)):
                moves = BASE + [r2, by, follow[k]]
                view = position_board(moves, 0.075, [(follow[k], GOLD)])
                fz.add(self.zoom_in(fg[k], zoom_card(view, ['follow-up %d' % (k + 1), 'after five moves']), np.array([x, -2.35, 0]), run_time=0.9))
            self.until('Each of those follow-ups')
            job = follow[0]
            job_piece = piece_on(b, move_cells(job), P_RED)
            job_line = outline_on(b, move_cells(job))
            self.click(0.3)
            self.play(FadeIn(job_piece, scale=1.2), Create(job_line), fz[0][1][0].animate.set_stroke(WHITE, 3), run_time=0.8)
            self.play(self.search_flicker(b, TRACES['job'], 3.6), run_time=3.6)
            jobs = VGroup(caption('one job: minutes to hours', MUTED, 24))
            jobs.next_to(ans_lab, DOWN, buff=0.35)
            self.play(FadeIn(jobs), LaggedStart(*[c.animate.set_fill(GOOD, 0.9) for c in fg], lag_ratio=0.002), run_time=d * 0.15)

        self.beat(para(10, 1), fan)
        self.clear_all(0.6)

        def laptop(d):
            # my home laptop, working day and night: jobs stream in while a sun and a moon take turns above it
            pc = worker('my home laptop, working day and night', P_BLUE).shift(DOWN * 0.4)
            queue = card('jobs', GOLD, 1.6, 0.8, 26).move_to(UP * 2.6 + LEFT * 3.0)
            sky = np.array([3.0, 2.5, 0])

            def sun():
                rays = [Line(RIGHT * 0.44, RIGHT * 0.62, color=GOLD, stroke_width=4).rotate(k * TAU / 8, about_point=ORIGIN) for k in range(8)]
                return VGroup(Circle(0.3, stroke_width=0).set_fill(GOLD, 1), *rays).move_to(sky)

            moon = VGroup(Circle(0.34, stroke_width=0).set_fill(INK, 1),  # a crescent: a light disc, partly covered
                          Circle(0.3, stroke_width=0).set_fill(BG, 1).shift(RIGHT * 0.18 + UP * 0.1)).move_to(sky)
            day, day2 = sun(), sun()
            self.play(FadeIn(pc), FadeIn(queue), FadeIn(day), run_time=0.8)
            jobs = VGroup(*[hex_shape(queue.get_center(), 0.13, fill=GOLD, stroke=GOLD, sw=0) for _ in range(7)])
            self.add(jobs)
            fly = [j.animate.move_to(pc[0][k].get_center()).scale(2.9).set_fill(GOOD, 0.55) for k, j in enumerate(jobs)]
            self.play(LaggedStart(*fly[:4], lag_ratio=0.3), FadeOut(day, scale=0.6), FadeIn(moon, scale=0.6), run_time=1.6)
            self.play(LaggedStart(*fly[4:], lag_ratio=0.3), FadeOut(moon, scale=0.6), FadeIn(day2, scale=0.6), run_time=1.6)

        self.beat(para(10, 2), laptop)
        self.clear_all(0.6)

        def progress(d):
            replies = ZOOM['replies']
            grid2 = tiny_grid(len(replies), 53, size=0.12, gap=0.04).move_to(LEFT * 2.3 + UP * 0.75)
            self.play(FadeIn(grid2), run_time=0.6)
            proven = ZOOM['proven']  # [index, red reply, blue answer that wins, hours]
            self.play(LaggedStart(*[grid2[k].animate.set_fill(P_BLUE, 1) for k, *_ in proven], lag_ratio=0.15), run_time=d * 0.2)
            self.until('each costing')
            for p, y in ((min(proven, key=lambda p: p[3]), 1.85), (max(proven, key=lambda p: p[3]), -0.75)):
                k, r2, by, hours = p
                view = position_board(BASE + [r2, by], 0.085, [(r2, GOLD), (by, WHITE)])
                card_ = zoom_card(view, ['proven: blue wins', '%s hours of computing' % ('%g' % hours)], P_BLUE)
                self.zoom_in(grid2[k], card_, np.array([4.9, y, 0]), run_time=0.9)
                self.play(grid2[k].animate.set_fill(P_BLUE, 1), run_time=0.3)
            self.until('until you do the division')
            frac = caption('16 / 1,589 ≈ 1 %', INK, 34, BOLD).to_edge(DOWN, buff=0.45)
            self.play(FadeIn(frac), run_time=0.8)
            eta = caption('a year or two', GOLD, 32).next_to(frac, UP, buff=0.25)
            self.until('At that pace')
            self.play(FadeIn(eta), run_time=0.8)
            self.prog = grid2

        self.beat(para(10, 3), progress)
        # reply 1 of the proven ones is the game from the cold open: its square opens up into the board
        sq = self.prog[0].copy()
        self.add(sq)
        self.play(*[FadeOut(m) for m in self.mobjects if m is not sq], sq.animate.set_fill(GOLD, 1).scale(1.6), run_time=0.8)
        self.board10 = Board(s=0.34)
        self.play(ReplacementTransform(sq, self.board10), run_time=1.0)

        def won_game(d):
            # the toughest of the sixteen proofs (HARD_GAME), played out close: both build a second snake, blue joins
            board = self.board10
            red, blue = self.counters(board)
            placed = position_after(HARD_GAME, 2)
            pcs = VGroup(*[piece_on(board, move_cells(m), P_RED if j % 2 == 0 else P_BLUE) for j, m in enumerate(HARD_GAME[:2])])
            self.set_count(red, 1, P_RED)
            self.set_count(blue, 4, P_BLUE)
            tag = caption('proven: blue wins \u00b7 241 hours', P_BLUE, 28).to_corner(DL, buff=0.4)
            self.play(FadeIn(pcs), FadeIn(red), FadeIn(blue), FadeIn(tag), run_time=0.8)

            def fast(a, b, run_time):
                ms = HARD_GAME[a:b]
                shown = [piece_on(board, move_cells(m), P_RED if (a + j) % 2 == 0 else P_BLUE) for j, m in enumerate(ms)]
                for j, m in enumerate(ms):
                    placed[(a + j) % 2] |= set(move_cells(m))
                self.play(LaggedStart(*[FadeIn(pc, scale=1.15) for pc in shown], lag_ratio=0.7),
                          self.count_to(red, longest_snake(placed[0]), P_RED), self.count_to(blue, longest_snake(placed[1]), P_BLUE),
                          run_time=run_time)

            self.until('Red answers with')
            self.play_moves(board, HARD_GAME[2:3], 2, per_move=1.0, counters=(red, blue), placed=placed)
            self.until('blue answers in the top corner')
            self.play_moves(board, HARD_GAME[3:4], 3, per_move=1.0, counters=(red, blue), placed=placed)
            self.until('Red grows its snake')
            self.play_moves(board, HARD_GAME[4:5], 4, per_move=1.0, counters=(red, blue), placed=placed)
            # both players start a second snake
            self.until('Each of them builds a second snake')
            fast(5, 7, 1.4)
            seconds = VGroup(outline_on(board, move_cells(HARD_GAME[5])), outline_on(board, move_cells(HARD_GAME[6])))
            self.play(Create(seconds), run_time=0.5)
            self.play(FadeOut(seconds), run_time=0.3)
            fast(7, 11, 2.2)
            # blue's straight four down the middle, between red's two snakes, joins blue's own two
            self.until('Then blue plays a straight four')
            self.play_moves(board, HARD_GAME[11:12], 11, per_move=1.0, counters=(red, blue), placed=placed)
            join = outline_on(board, move_cells(HARD_GAME[11]), GOLD)
            self.play(Create(join), Indicate(blue[1], color=GOLD), run_time=0.8)
            self.until('Blue wins')
            self.play(FadeOut(join), run_time=0.3)
            fast(12, 15, 1.8)
            glows = VGroup(*[VGroup(*[board.cells[c].copy().set_fill(opacity=0).set_stroke(GOLD, 4) for c in max(components(placed[pl]), key=len)])
                             for pl in (0, 1)])
            result = caption('blue wins 21 : 15', P_BLUE, 40, BOLD).to_edge(DOWN, buff=0.35)
            self.play(Create(glows), FadeOut(tag), FadeIn(result, shift=UP * 0.2), blue[1].animate.set_color(GOLD), run_time=1.0)
            self.wait(1.5)  # the result sits for a moment even when the moves ran past the narration
            self.wg = (board, pcs)

        self.beat(para(10, 4), won_game, pad=1.0)
        # the game rewinds to its first two moves, which the stubborn reply shares
        later = [m for m in self.mobjects if all(m is not k for k in self.wg)]
        self.play(LaggedStart(*[FadeOut(m) for m in reversed(later)], lag_ratio=0.04), run_time=1.2)

        def stubborn(d):
            # every reply proven so far went blue's way, until this one
            b, two = self.wg
            looks = caption('16 / 16: blue wins', P_BLUE, 34, BOLD).move_to(RIGHT * 2.6)
            self.play(VGroup(b, two).animate.scale(0.26 / 0.34).move_to(LEFT * 4.3), FadeIn(looks, shift=UP * 0.2), run_time=0.8)
            self.until('But then we hit')
            self.play(FadeOut(looks), run_time=0.4)
            third = piece_on(b, move_cells(DRAW_GAME[2]), P_RED)
            line = outline_on(b, move_cells(DRAW_GAME[2]))
            self.click(0.3)
            self.play(FadeIn(third, scale=1.2), Create(line), run_time=0.8)
            g = VGroup(b, two, third, line)
            tries, chips, head, marks, total = stubborn_grid()
            self.play(FadeIn(head), LaggedStart(*[FadeIn(c) for c in chips], lag_ratio=0.03), run_time=d * 0.12)
            self.until('All 40 failed')
            self.play(LaggedStart(*[AnimationGroup(refuted_fill(c.animate, t), FadeIn(m))
                                    for c, m, t in zip(chips, marks, tries)], lag_ratio=0.05), run_time=d * 0.2)
            self.until('Not by guesswork')
            self.play(FadeOut(g), run_time=0.5)
            self.sb = (chips, tries, total)

        parts = split_at(para(10, 5), ['In 31 of the 40', 'In the other 9', 'That one red reply'])
        self.beat(parts[0], stubborn)
        # two of the refuted answers, played on to the end with perfect play (pv_line.py -> stubborn_lines.json)
        lines = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'stubborn_lines.json')))
        self.ex = None

        def example(key, verdict, colour):
            def run(d):
                chips, tries, _ = self.sb
                line = lines[key]['moves']
                k = next(j for j, t in enumerate(tries) if t[:2] == line[3:5])
                if self.ex is not None:
                    self.play(FadeOut(self.ex), run_time=0.4)
                b, pcs = board_with(line, 5, 0.22)
                card = VGroup(b, pcs, outline_on(b, move_cells(line[3]), WHITE, 3.5), outline_on(b, move_cells(line[4]), GOLD, 3.5))
                zoom = self.zoom_in(chips[k], card, np.array([-4.2, -0.1, 0]), run_time=0.9)
                placed = position_after(line, 5)
                red, blue = self.counters(b)
                self.set_count(red, longest_snake(placed[0]), P_RED)
                self.set_count(blue, longest_snake(placed[1]), P_BLUE)
                self.play(FadeIn(red), FadeIn(blue), run_time=0.4)
                rest = [(j, m) for j, m in enumerate(line) if j >= 5 and m != 'pass']
                fast = [piece_on(b, move_cells(m), P_RED if j % 2 == 0 else P_BLUE) for j, m in rest]
                for j, m in rest:
                    placed[j % 2] |= set(move_cells(m))
                self.play(LaggedStart(*[FadeIn(pc, scale=1.15) for pc in fast], lag_ratio=0.6),
                          self.count_to(red, longest_snake(placed[0]), P_RED), self.count_to(blue, longest_snake(placed[1]), P_BLUE),
                          run_time=4.0)
                result = caption('%d : %d   %s' % (lines[key]['red'], lines[key]['blue'], verdict), colour, 30, BOLD)
                result.next_to(card, DOWN, buff=0.25)
                self.play(FadeIn(result, shift=UP * 0.15), run_time=0.6)
                self.ex = VGroup(zoom, *fast, red, blue, result)
            return run

        self.beat(parts[1], example('loss', 'blue loses', BAD), pad=0.8)
        self.beat(parts[2], example('draw', 'a draw', GOLD), pad=0.8)

        def summary(d):
            # the example game goes, the grid stays: chapter 11 starts from exactly this frame (stubborn_grid)
            chips, _, total = self.sb
            self.play(FadeOut(self.ex), FadeIn(total), run_time=0.8)

        self.beat(parts[3], summary, pad=1.5)


def stubborn_grid():
    """Blue's 40 best answers to the stubborn reply as a grid of chips (chapter 10 builds it up, chapter 11 starts
    from it): returns tries, chips, head, marks and the total line. tries[0] is the U (DRAW_GAME[3])."""
    tries = ZOOM['stubborn_tries']  # [blue move, red refutation, value for blue: 0 draw, -1 loss]
    chips = VGroup(*[RoundedRectangle(width=0.9, height=0.5, corner_radius=0.08, stroke_width=0).set_fill(P_BLUE, 0.8) for _ in tries])
    chips.arrange_in_grid(rows=5, cols=8, buff=0.12).shift(RIGHT * 2.4 + UP * 0.4)
    head = caption('blue\u2019s 40 best answers', INK, 28).next_to(chips, UP, buff=0.3)
    marks = VGroup(*[caption('\u2717 ' + ('loss' if t[2] < 0 else 'draw'), P_RED if t[2] < 0 else INK, 18).move_to(c)
                     for c, t in zip(chips, tries)])
    total = caption('31 losses \u00b7 9 draws \u00b7 282 hours', GOLD, 30).next_to(chips, DOWN, buff=0.4)
    return tries, chips, head, marks, total


def refuted_fill(chip, t):
    return chip.set_fill(P_RED if t[2] < 0 else MUTED, 0.5)


# ================================================================================================
# 11. the game hidden behind move 76
# ================================================================================================
class Ch11Move76(Narrated):
    def construct(self):
        board = Board(s=0.34)
        red, blue = self.counters(board)
        placed = position_after(DRAW_GAME, 3)
        shown = [piece_on(board, move_cells(m), P_RED if j % 2 == 0 else P_BLUE) for j, m in enumerate(DRAW_GAME[:3])]
        self.set_count(red, longest_snake(placed[0]), P_RED)
        self.set_count(blue, longest_snake(placed[1]), P_BLUE)
        # the last frame of chapter 10: the grid of blue's 40 refuted answers
        tries, chips, head, marks, total = stubborn_grid()
        for c, t in zip(chips, tries):
            refuted_fill(c, t)
        self.add(head, chips, marks, total)

        def play(k, per_move=1.0):
            pcs, _ = self.play_moves(board, DRAW_GAME[k:k + 1], k, per_move=per_move, counters=(red, blue), placed=placed)
            shown.extend(p for p in pcs if p is not None)

        def ring(cells, color, width=4):
            return VGroup(*[board.cells[c].copy().set_fill(opacity=0).set_stroke(color, width) for c in cells])

        def u_block(d):
            # zoom into the first chip, the first answer tried: a tiny copy of the board inside it fills the frame,
            # then the real board takes its place. Cairo lines get thicker as the camera zooms in, so the copy's
            # lines are thinned by the same factor and the swap does not show.
            first = chips[0]
            game = VGroup(board, *shown)
            k = 0.07
            tiny = game.copy().scale(k, about_point=ORIGIN).shift(first.get_center())
            for m in tiny.family_members_with_points():
                m.set_stroke(width=m.get_stroke_width() * k, family=False)
            self.play(Indicate(first, color=GOLD, scale_factor=1.15), run_time=0.6)
            self.camera.frame.save_state()
            self.play(self.camera.frame.animate.scale(k).move_to(first.get_center()),
                      FadeOut(VGroup(head, chips, marks, total)), FadeIn(tiny), run_time=1.8)
            self.remove(tiny)
            self.camera.frame.restore()
            self.add(game)
            self.play(FadeIn(red), FadeIn(blue), run_time=0.5)
            play(3)
            self.u = outline_on(board, move_cells(DRAW_GAME[3]))
            self.play(Create(self.u), run_time=0.8)

        self.beat(para(11, 0), u_block)

        def draw_stamp(d):
            play(4)
            self.stamp = card('exact value: draw', GOLD, 4.2, 1.0, 34, fill=GOLD).rotate(0.12).move_to(RIGHT * 4.5 + DOWN * 2.2)
            self.play(FadeIn(self.stamp, scale=1.15), run_time=0.8)  # the stamp lands without touching the frame edge

        self.beat(para(11, 1), draw_stamp)

        hand = {}
        for player, names in ((0, ('2', '3I', '4A', '4B', '4C')), (1, ('2', '3I', '3V', '4I', '4A'))):
            pieces = {n: loose_piece(n, P_RED if player == 0 else P_BLUE, s=0.2) for n in names}
            g = VGroup(*pieces.values()).arrange_in_grid(rows=3, cols=2, buff=(0.4, 0.3))
            within_frame(g.move_to(np.array([-5.6 if player == 0 else 5.6, -2.1, 0])))
            hand[player] = (pieces, g)

        def strength(d):
            # red's straight four ends at two cells beside blue's U: poisoned for blue, free for red
            four = outline_on(board, move_cells(DRAW_GAME[4]), GOLD, 5)
            self.play(FadeOut(self.stamp), FadeOut(self.u), Create(four), FadeIn(hand[0][1]), FadeIn(hand[1][1]), run_time=0.8)
            self.until('Red now has these spaces reserved')
            road = VGroup(*[board.cells[c].copy().set_fill(P_RED, 0.45).set_stroke(P_RED, 3) for c in (80, 81)])
            lab = caption('reserved for red', P_RED, 28).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(road), FadeIn(lab), run_time=0.8)
            # what red can build through those two cells, blue not answering (red_shadow.json, 'road')
            self.until('Red has a free road')
            way = SHADOW['road']
            ghosts = VGroup(*[piece_on(board, move_cells(m), P_RED).set_opacity(0.4) for m in way['moves']])
            road_lab = caption('free road: %d \u2192 %d' % (way['from'], way['to']), P_RED, 28).to_edge(DOWN, buff=0.35)
            self.play(FadeTransform(lab, road_lab), LaggedStart(*[FadeIn(gh, scale=0.9) for gh in ghosts], lag_ratio=0.5), run_time=1.6)
            # the natural answer, a wall right under red's two cells (the engine's 8th choice), and how red refutes it
            self.until('The natural answer is to wall it off')
            fav = piece_on(board, move_cells(BLOCK_LOSS[5]), P_BLUE).set_opacity(0.65)
            fav_lab = caption('a wall', P_BLUE, 28).to_edge(DOWN, buff=0.35)
            self.click(0.3)
            self.play(FadeIn(fav, scale=1.2), FadeOut(ghosts), FadeOut(road_lab), FadeIn(fav_lab), run_time=0.8)
            self.until('But red simply turns the other way')
            answer = piece_on(board, move_cells(BLOCK_LOSS[6]), P_RED).set_opacity(0.65)
            cut = outline_on(board, move_cells(BLOCK_LOSS[6]), GOLD, 5)  # red takes its two cells and turns left
            self.click(0.3)
            self.play(FadeIn(answer, scale=1.2), FadeOut(road), run_time=0.8)
            self.play(Create(cut), run_time=0.5)
            self.until('and blue loses')
            loses = caption('blue loses 15 : 16', BAD, 28).to_edge(DOWN, buff=0.35)
            self.play(FadeTransform(fav_lab, loses), run_time=0.6)
            self.wait(0.8)
            self.play(*[FadeOut(m) for m in (fav, answer, cut, four, loses)], run_time=0.6)

        self.beat(para(11, 2), strength)

        # the challenge: pause, then a countdown, one spoken number at a time
        pause = VGroup(*[RoundedRectangle(width=0.32, height=1.1, corner_radius=0.08, stroke_width=0).set_fill(INK, 1) for _ in range(2)])
        pause.arrange(RIGHT, buff=0.28).move_to(RIGHT * 5.3 + UP * 2.3)
        task = VGroup(caption('your move, blue:', INK, 28), caption('find one that', MUTED, 26), caption('does not lose', MUTED, 26))
        task.arrange(DOWN, aligned_edge=LEFT, buff=0.1).next_to(pause, DOWN, buff=0.5)
        self.beat(split_at(para(11, 3), ['Three, two, one.'])[0],
                  lambda d: self.play(FadeIn(pause, scale=1.2), FadeIn(task), run_time=1.0), pad=1.0)
        self.play(FadeOut(pause), FadeOut(task), run_time=0.4)
        number = None
        for word, digit in (('Three.', '3'), ('Two.', '2'), ('One.', '1')):
            big = caption(digit, GOLD, 160, BOLD).move_to(RIGHT * 5.3 + UP * 1.0)

            def show(d, big=big, prev=number):
                self.play(*([FadeOut(prev, scale=0.6)] if prev else []), FadeIn(big, scale=1.6), run_time=0.4)

            self.beat(word, show, pad=0.6)
            number = big

        def reveal(d):
            self.play(FadeOut(number, scale=0.6), run_time=0.3)
            self.play(FadeOut(hand[1][0]['4I']), run_time=0.3)
            play(5)
            gold = outline_on(board, move_cells(DRAW_GAME[5]), GOLD, 6)
            lab = caption('the 76th move', GOLD, 30, BOLD).to_edge(DOWN, buff=0.35)
            self.play(Create(gold), FadeIn(lab), Flash(board.at(57), color=GOLD, line_length=0.3), run_time=1.0)
            # the queue: blue's moves in the order the engine likes them, solved one by one
            self.until('ranked this move only 76th')
            game = VGroup(board, *shown, gold)
            game.save_state()
            self.play(game.animate.scale(0.55).to_edge(LEFT, buff=0.3), FadeOut(lab), FadeOut(red), FadeOut(blue),
                      FadeOut(hand[0][1]), *[FadeOut(m) for n, m in hand[1][0].items() if n != '4I'], run_time=0.8)
            tries = DATA['u_block_tries']
            cards = VGroup(*[RoundedRectangle(width=0.36, height=0.5, corner_radius=0.05, stroke_width=0).set_fill(MUTED, 0.6) for _ in tries])
            cards.arrange_in_grid(rows=4, cols=19, buff=0.08).shift(RIGHT * 2.6 + UP * 0.3)
            head = caption('blue’s moves, in engine order', INK, 26).next_to(cards, UP, buff=0.3)
            counter = ValueTracker(0)
            num = always_redraw(lambda: caption('%d lose' % int(counter.get_value()), P_RED, 30).next_to(cards, DOWN, buff=0.35))
            self.play(FadeIn(head), FadeIn(cards), run_time=0.6)
            self.add(num)
            self.play(LaggedStart(*[c.animate.set_fill(P_RED, 0.85) for c in cards[:-1]], lag_ratio=0.12, rate_func=rush_into),
                      counter.animate.set_value(75), run_time=3.0)
            self.play(cards[-1].animate.set_fill(GOLD, 1).scale(1.6), Flash(cards[-1], color=GOLD), run_time=0.8)
            last = caption('76th: draw', GOLD, 32, BOLD).next_to(num, DOWN, buff=0.2)
            self.play(FadeIn(last), run_time=0.5)
            self.rv = (game, cards, head, last, num, gold)

        reveal_text, why_text = split_at(para(11, 4), ['And here is why the 76th works'])
        self.beat(reveal_text, reveal, pad=0.8)

        def explain(d):
            # why it works, on the board
            game, cards, head, last, num, gold = self.rv
            self.remove(num)
            self.play(FadeOut(VGroup(cards, head, last)), Restore(game), FadeIn(red), FadeIn(blue), run_time=1.0)
            self.until('but on the left')
            path = outline_on(board, move_cells(DRAW_GAME[9]), P_BLUE, 3).set_stroke(opacity=0.7)  # where the join will go
            end = ring([16], P_BLUE, 5)
            self.until('close enough to the top end')
            self.play(Create(end), run_time=0.5)
            self.play(Create(path), run_time=0.8)
            self.until('still threatens to grow')
            # every bottom cell red's snake could still grow into
            threat = VGroup(*[board.cells[c].copy().set_fill(P_RED, 0.3).set_stroke(P_RED, 2) for c in SHADOW['wants']])
            tlab = caption('where red can still grow', P_RED, 28).to_edge(DOWN, buff=0.35)
            self.play(LaggedStart(*[FadeIn(c) for c in threat], lag_ratio=0.04), FadeIn(tlab), run_time=1.2)
            self.until('Red does flow into')
            play(6)
            cut = Cross(board.cells[79], stroke_color=BAD, stroke_width=5).scale(0.6)  # one end of the new snake
            self.play(Create(cut), run_time=0.5)
            # what red would build along the bottom (three more pieces, all legal), and blue's L right in its way
            shadow = VGroup(*[piece_on(board, move_cells(m), P_RED).set_opacity(0.4) for m in SHADOW['moves']])
            slab = caption('%d \u2192 %d' % (SHADOW['from'], SHADOW['to']), P_RED, 28).to_edge(DOWN, buff=0.35)
            self.play(FadeOut(threat), FadeTransform(tlab, slab),
                      LaggedStart(*[FadeIn(g, scale=0.9) for g in shadow], lag_ratio=0.5), run_time=1.4)
            self.until('next piece, an L')
            play(7)
            shown[-1].set_z_index(1)  # the L stays above the dimming shadow
            shadow_cells = {c for m in SHADOW['moves'] for c in move_cells(m)}
            hits = VGroup(*[Cross(board.cells[c], stroke_color=BAD, stroke_width=5).scale(0.5)
                            for c in move_cells(DRAW_GAME[7]) if c in shadow_cells]).set_z_index(2)
            self.play(Create(hits), shadow.animate.set_opacity(0.15), run_time=0.6)
            self.until('only 21 keep the draw')
            # every move that keeps the draw here goes into the bottom of the board
            self.play(FadeOut(shadow), FadeOut(hits), FadeOut(slab), run_time=0.4)
            shown[-1].set_z_index(0)
            zone = VGroup(*[board.cells[c].copy().set_fill(GOLD, 0.3).set_stroke(GOLD, 3) for c in MOVE8['cells']])
            zlab = caption('21 moves hold the draw', GOLD, 28).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(zone), FadeIn(zlab), run_time=0.8)
            # red grows into its corridor on the left, then blue's three-piece joins its two snakes
            self.until('Then blue')
            self.play(FadeOut(zone), FadeOut(zlab), FadeOut(cut), FadeOut(end), run_time=0.4)
            play(8, 0.9)
            play(9)
            self.play(FadeOut(path), run_time=0.3)
            glow = ring(max(components(placed[1]), key=len), GOLD)
            self.play(Create(glow), Flash(blue[1], color=GOLD), run_time=1.0)
            self.wait(0.5)
            self.play(FadeOut(glow), FadeOut(gold), run_time=0.5)

        self.beat(why_text, explain, pad=1.0)

        def perfect(d):
            # the rest of the game, quickly and without comment
            for k in range(10, 14):
                play(k, 0.9)
            self.play(red[1].animate.set_color(GOLD), blue[1].animate.set_color(GOLD), run_time=0.8)

        self.beat(para(11, 5), perfect, pad=1.0)

        def human(d):
            focus = board.at(57)
            gl = outline_on(board, move_cells(DRAW_GAME[5]), GOLD, 6)
            self.play(Create(gl), self.camera.frame.animate.scale(0.55).move_to(focus), run_time=d * 0.3)
            lab = caption('ranked 76th', GOLD, 22, BOLD).move_to(focus + DOWN * 1.3)
            lab.add_background_rectangle(color=BG, opacity=0.85, buff=0.08)
            self.play(FadeIn(lab), run_time=0.6)
            self.wait(max(0.1, d * 0.3))
            self.play(self.camera.frame.animate.scale(1 / 0.55).move_to(ORIGIN), FadeOut(lab), FadeOut(gl), run_time=1.0)

        self.beat(para(11, 6), human, pad=1.2)

        self.clear_all()


# ================================================================================================
# 12. why it is so hard
# ================================================================================================
class Ch12WhyHard(Narrated):
    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath

        def lopsided(d):
            self.until('A proof of a win')
            self.play(FadeIn(caption('a lopsided proof', INK, 44, BOLD)), run_time=1.0)

        self.beat(para(12, 0), lopsided)
        self.clear_all(0.5)

        def tree(d):
            root = Dot(UP * 3.0, radius=0.12, color=GOLD)
            levels = VGroup()
            parents = [root.get_center()]
            labels = VGroup()
            highlights = []
            for depth in range(4):
                red_turn = depth % 2 == 0
                kids, lines = [], VGroup()
                y = 3.0 - 1.45 * (depth + 1)
                for pi, par in enumerate(parents):
                    n = 4 if red_turn else 3
                    span = 6.0 / (1 + depth * 0.9)
                    for j in range(n):
                        x = par[0] + (j - (n - 1) / 2) * span / n
                        child = np.array([x, y, 0])
                        chosen = red_turn or j == 1
                        lines.add(Line(par, child, color=(P_RED if red_turn else P_BLUE) if chosen else FAINT,
                                       stroke_width=3 if chosen else 1.5))
                        if chosen:
                            kids.append(child)
                levels.add(lines)
                labels.add(caption('every move must fail' if red_turn else 'one move is enough',
                                   P_RED if red_turn else P_BLUE, 24).to_edge(RIGHT, buff=0.3).set_y(y + 0.7))
                parents = kids[:6] if not red_turn else kids
            self.play(FadeIn(root), run_time=0.4)
            for lines, lab in zip(levels, labels):
                self.play(LaggedStart(*[Create(l) for l in lines], lag_ratio=0.03), FadeIn(lab), run_time=d * 0.18)

        self.beat(para(12, 1), tree)
        self.clear_all(0.6)

        def queue(d):
            grid = tiny_grid(ZOOM['stubborn_total'], 43, size=0.15, gap=0.05).move_to(LEFT * 1.9 + UP * 0.3)
            head = caption('blue’s 1,032 moves', INK, 28).next_to(grid, UP, buff=0.3)
            self.play(FadeIn(head), LaggedStart(*[FadeIn(c) for c in grid], lag_ratio=0.0008), run_time=d * 0.2)
            self.until('The 40 most promising')
            self.play(LaggedStart(*[grid[k].animate.set_fill(P_RED, 1) for k in range(40)], lag_ratio=0.05), run_time=d * 0.12)
            blue_try, red_answer, _ = ZOOM['stubborn_tries'][0]
            view = position_board(DRAW_GAME[:3] + [blue_try, red_answer], 0.095, [(blue_try, WHITE), (red_answer, GOLD)])
            self.zoom_in(grid[0], zoom_card(view, ['refuted', 'red holds a draw'], P_RED), np.array([5.0, 1.45, 0]), run_time=0.9)
            self.play(grid[0].animate.set_fill(P_RED, 1), run_time=0.2)
            lab = caption('40 refuted · 992 to go', GOLD, 30).next_to(grid, DOWN, buff=0.3)
            self.until('are left')
            self.play(FadeIn(lab), run_time=0.8)
            k = 600
            open_move = ZOOM['stubborn_open'][k - 40]
            view = position_board(DRAW_GAME[:3] + [open_move], 0.095, [(open_move, WHITE)])
            self.zoom_in(grid[k], zoom_card(view, ['still open', 'move %d of 1,032' % (k + 1)], MUTED), np.array([5.0, -1.45, 0]), run_time=0.9)

        self.beat(para(12, 2), queue)
        self.clear_all(0.6)

        def scale_up(d):
            # each level of the proof with the position it starts from and its search flashing over it
            names = ['hours', 'days', 'weeks', 'months', 'years', 'decades']
            axis = NumberLine(x_range=[0, 5, 1], length=7.4, color=FAINT, include_numbers=False).move_to(RIGHT * 2.6 + DOWN * 2.9)
            ticks = VGroup(*[caption(n, MUTED, 22).next_to(axis.n2p(k), DOWN, buff=0.2) for k, n in enumerate(names)])
            rows = [('stubborn', 'one red reply', 'refute all 1,032 blue moves', 1.0, 2.0, P_RED, 1.85, None),
                    ('opening', 'this opening', 'answer all 1,589 red replies', 4.0, 4.35, P_BLUE, 0.05, 'If the centre opening'),
                    ('whole', 'the whole game', 'the same for all 12 first moves', 4.6, 5.0, GOLD, -1.75, 'Proving the result of the whole game')]
            self.play(Create(axis), FadeIn(ticks), run_time=0.8)
            bar = None
            for key, name, what, lo, hi, col, y, phrase in rows:
                if phrase:
                    self.until(phrase)
                start = TRACES[key]['start']
                b, pcs = board_with(start, len(start), 0.095)
                g = VGroup(b, pcs).move_to(np.array([-6.0, y, 0]))
                marks = VGroup()
                if key == 'whole':
                    marks = VGroup(*[b.cells[c].copy().set_fill(GOLD, 0.35).set_stroke(GOLD, 1.5) for c in RED_FIRST_MOVES])
                lab = VGroup(caption(name, col, 26, BOLD), caption(what, MUTED, 22)).arrange(DOWN, aligned_edge=LEFT, buff=0.08)
                lab.next_to(g, RIGHT, buff=0.3)
                bar = Rectangle(width=axis.n2p(hi)[0] - axis.n2p(lo)[0], height=0.5, stroke_width=0).set_fill(col, 0.8)
                bar.move_to(np.array([(axis.n2p(lo)[0] + axis.n2p(hi)[0]) / 2, y, 0]))
                self.play(FadeIn(g), FadeIn(marks), FadeIn(lab), run_time=0.5)
                self.play(self.search_flicker(b, TRACES[key], 2.4), GrowFromEdge(bar, LEFT), run_time=2.4)
            self.until('And if the true answer is a draw')
            more = caption('→', GOLD, 40).next_to(bar, RIGHT, buff=0.1)
            draw = caption('a draw: both sides', GOLD, 22).next_to(bar, UP, buff=0.15).align_to(bar, RIGHT)
            self.play(FadeIn(more, shift=RIGHT * 0.2), FadeIn(draw), run_time=0.8)

        self.beat(para(12, 3), scale_up)
        self.clear_all(0.6)

        def deepest(d):
            b, pcs = board_with(DRAW_GAME, 6, 0.32)
            g = VGroup(b, pcs)
            self.play(FadeIn(g), run_time=0.8)
            gl = outline_on(b, move_cells(DRAW_GAME[5]), GOLD, 6)
            lab = caption('the move nobody would guess', GOLD, 30).to_edge(DOWN, buff=0.35)
            self.play(Create(gl), FadeIn(lab), run_time=d * 0.3)
            # every red reply has to be proven: any of them may hide a move like this, far down the line
            self.until('And that is exactly why')
            replies = VGroup(*[Square(0.13, stroke_width=0).set_fill(P_RED, 0.55) for _ in range(400)])
            replies.arrange_in_grid(rows=20, cols=20, buff=0.05).move_to(RIGHT * 3.4)
            self.play(VGroup(g, gl).animate.scale(0.62).move_to(LEFT * 3.6), FadeOut(lab), run_time=0.8)
            self.play(LaggedStart(*[FadeIn(r) for r in replies], lag_ratio=0.01), run_time=1.6)
            self.until('is actually the strongest')
            hidden = replies[263]
            self.play(hidden.animate.set_fill(GOLD, 1).scale(1.6), Flash(hidden, color=GOLD, line_length=0.2), run_time=0.8)

        self.beat(para(12, 4), deepest, pad=1.5)
        self.clear_all()


# ================================================================================================
# 13. human intuition versus the cold machine
# ================================================================================================
class Ch13Interlude(Narrated):
    def construct(self):
        chapter_card(self, 4, 'Where does this leave us?', art_question())

        peace = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'peaceful_game.json')))['moves']

        table_text, build_text = split_at(para(13, 0), ['If both players simply build'])
        # spoken to the camera at the table, showing red's extra piece (camera.py; a placeholder until the footage exists)
        table = on_camera_placeholder(table_text, 'Adam at the table, on camera: the answer is right there, red has one more piece', 8, 4.5)
        self.beat(table_text, lambda d: self.play(FadeIn(table), run_time=0.8))

        def why(d):
            # a real game where both players only grow their own snake and place every piece (peaceful_game.py)
            board = Board(s=0.3)
            red_c, blue_c = self.counters(board)
            self.play(FadeOut(table), FadeIn(board), FadeIn(red_c), FadeIn(blue_c), run_time=0.8)
            placed = {0: set(), 1: set()}
            pieces = []
            for j, m in enumerate(peace):
                pieces.append(piece_on(board, move_cells(m), P_RED if j % 2 == 0 else P_BLUE))
                placed[j % 2] |= set(move_cells(m))
            self.play(LaggedStart(*[FadeIn(pc, scale=1.15) for pc in pieces], lag_ratio=0.55),
                      self.count_to(red_c, longest_snake(placed[0]), P_RED), self.count_to(blue_c, longest_snake(placed[1]), P_BLUE),
                      run_time=4.5)
            self.until('red wins by exactly one cell')
            result = caption('both just build: red wins 25 : 24', INK, 30).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(result, shift=UP * 0.15), red_c[1].animate.set_color(GOLD), run_time=0.8)
            # red has both the first and the last move
            self.until('Red also moves first and last')
            ends = VGroup(outline_on(board, move_cells(peace[0]), GOLD, 5), outline_on(board, move_cells(peace[-1]), GOLD, 5))
            tags = VGroup(caption('first', GOLD, 26).next_to(ends[0], UP, buff=0.12),
                          caption('last', GOLD, 26).next_to(ends[1], LEFT, buff=0.15))
            for t in tags:
                t.add_background_rectangle(color=BG, opacity=0.85, buff=0.06)  # readable over the pieces
            self.play(Create(ends[0]), FadeIn(tags[0]), run_time=0.6)
            self.play(Create(ends[1]), FadeIn(tags[1]), run_time=0.6)
            self.w13 = (VGroup(board, *pieces), VGroup(red_c, blue_c, result, ends, tags))

        self.beat(build_text, why)

        def two_games(d):
            peaceful, extra = self.w13
            lt = caption('both just build: red wins 25 : 24', MUTED, 24)
            b, pcs = board_with(DRAW_GAME, 4, 0.22)
            rg = VGroup(b, pcs, outline_on(b, move_cells(DRAW_GAME[3]))).shift(RIGHT * 3.4 + UP * 0.6)
            rt = caption('blue blocks', P_BLUE, 24).next_to(rg, DOWN, buff=0.2)
            # the peaceful game moves aside, the same size as the game where blue blocks
            target = peaceful.copy().scale(0.22 / 0.3).move_to(LEFT * 3.4 + UP * 0.6)
            lt.next_to(target, DOWN, buff=0.2)
            # labels, outlines and counters go first: fading while the board moved away, they hung in the middle
            self.play(FadeOut(extra), run_time=0.4)
            self.play(peaceful.animate.scale(0.22 / 0.3).move_to(LEFT * 3.4 + UP * 0.6), FadeIn(lt), run_time=max(0.8, d * 0.12))
            self.play(FadeIn(rg), FadeIn(rt), run_time=d * 0.12)
            squares = VGroup(*[Square(0.4, stroke_width=0).set_fill(P_BLUE if k < 16 else MUTED, 1) for k in range(17)]).arrange(RIGHT, buff=0.1)
            squares.to_edge(DOWN, buff=1.0)
            lab = caption('16 of 17: blue wins', INK, 26).next_to(squares, DOWN, buff=0.2)
            self.play(LaggedStart(*[FadeIn(s) for s in squares], lag_ratio=0.1), FadeIn(lab), run_time=d * 0.25)

        self.beat(para(13, 1), two_games)
        self.clear_all(0.6)

        tree_text, fun_text = split_at(para(13, 2), ['And just because'])

        def not_tree(d):
            title = caption('A game is not its game tree.', INK, 52, BOLD)
            self.play(FadeIn(title), run_time=1.2)
            self.until('Theoretical balance')
            quote = VGroup(caption('Theoretical balance means nothing', GOLD, 44, BOLD),
                           caption('when the balancing moves are invisible.', GOLD, 44, BOLD)).arrange(DOWN, buff=0.25).shift(DOWN * 0.4)
            self.play(title.animate.shift(UP * 1.6).set_color(MUTED), FadeIn(quote, shift=UP * 0.2), run_time=1.0)
            # let it sink in: a slow push on the line and a few seconds of quiet after the narration
            _, t0, dur = self._beat
            self.play(quote.animate.scale(1.06), run_time=max(1.0, t0 + dur - self.renderer.time + 3.5), rate_func=smooth)
            self.w13b = (title, quote)

        self.beat(tree_text, not_tree, pad=0.2)

        def balance(d):
            title, quote = self.w13b
            # balanced is not the same as fun, and what makes a game fun is still an open question
            self.play(FadeOut(quote), run_time=0.6)
            fair = card('perfectly balanced', GOOD, 4.2, 1.0, 30)
            fun = card('fun', GOLD, 2.4, 1.0, 30)
            VGroup(fair, fun).arrange(RIGHT, buff=2.2).shift(DOWN * 0.3)
            arrow = Arrow(fair.get_right(), fun.get_left(), buff=0.2, color=MUTED, stroke_width=5)
            no = caption('✗', BAD, 56, BOLD).move_to(arrow)
            self.play(FadeIn(fair, shift=UP * 0.2), run_time=0.6)
            self.play(GrowArrow(arrow), FadeIn(fun, shift=UP * 0.2), run_time=0.7)
            self.until('does not guarantee')
            self.play(FadeIn(no, scale=1.4), run_time=0.5)
            self.until('What makes a game fun')
            q = caption('?', GOLD, 72, BOLD).next_to(fun, UP, buff=0.2)
            self.play(FadeIn(q, scale=1.5), Indicate(fun, color=GOLD), run_time=1.0)
            self.until('game designers have been trying')
            open_q = caption('an open question for decades', MUTED, 30).next_to(VGroup(fair, fun), DOWN, buff=0.8)
            self.play(FadeIn(open_q, shift=UP * 0.2), run_time=0.8)

        self.beat(fun_text, balance, pad=1.2)
        self.clear_all(0.6)

        def question(d):
            b, pcs = board_with(DRAW_GAME, 6, 0.32)
            g = VGroup(b, pcs)
            self.play(FadeIn(g), run_time=0.6)
            focus = b.at(57)
            gl = outline_on(b, move_cells(DRAW_GAME[5]), GOLD, 6)
            self.play(Create(gl), self.camera.frame.animate.scale(0.6).move_to(focus), run_time=d * 0.25)
            self.play(self.camera.frame.animate.scale(1 / 0.6).move_to(ORIGIN), run_time=1.2)  # the board stays until Go takes over
            # a Go board: a close-up of one corner that zooms out while AlphaGo vs Lee Sedol, game 4 (2016), is played
            # on it, up to Lee Sedol's move 78 (go_pattern.py)
            self.until('It almost feels like the game of Go')
            c = 0.36

            def at(x, y):
                return np.array([(x - 9) * c, (9 - y) * c, 0])

            grid = VGroup(*[Line(at(0, k), at(18, k)) for k in range(19)], *[Line(at(k, 0), at(k, 18)) for k in range(19)])
            grid.set_stroke(FAINT, 2)
            wood = Square(18 * c + 0.6, stroke_color=FAINT, stroke_width=2).set_fill(EMPTY, 1)
            dots = VGroup(*[Dot(at(x, y), radius=0.05, color=FAINT) for x in (3, 9, 15) for y in (3, 9, 15)])
            stones = [Circle(0.46 * c, stroke_width=1.5, stroke_color=MUTED if col == 0 else INK).set_fill(SEAM if col == 0 else INK, 1).move_to(at(x, y))
                      for x, y, col in GO['stones']]
            frame = self.camera.frame
            # the board's lines keep their width on screen while the camera zooms (Cairo widths grow with the zoom)
            grid.add_updater(lambda m: m.set_stroke(width=2 * frame.width / config.frame_width))
            wood.add_updater(lambda m: m.set_stroke(width=2 * frame.width / config.frame_width))
            frame.save_state()
            # the camera dives into a corner: the Snekagon board dissolves into a close-up of the Go board
            self.play(frame.animate.scale(0.32).move_to(at(14, 3.5)), FadeOut(g), FadeOut(gl),
                      FadeIn(wood, suspend_mobject_updating=False), FadeIn(grid, suspend_mobject_updating=False), FadeIn(dots),
                      FadeIn(stones[0], scale=1.3), run_time=1.4)
            _, t0, dur = self._beat
            zoom = max(6.0, t0 + dur - self.renderer.time)
            self.play(Restore(frame, rate_func=smooth),
                      LaggedStart(*[FadeIn(st, scale=1.3) for st in stones[1:]], lag_ratio=0.4, rate_func=linear), run_time=zoom)
            grid.clear_updaters()
            wood.clear_updaters()
            last = GO['last']
            ring = Circle(0.62 * c, stroke_color=GOLD, stroke_width=5).move_to(at(last[0], last[1]))
            label = VGroup(caption('AlphaGo vs Lee Sedol', MUTED, 24), caption('2016 \u00b7 move 78', GOLD, 24)).arrange(DOWN, buff=0.12)
            space = config.frame_width / 2 - wood.get_right()[0]  # the free strip right of the board
            label.scale_to_fit_width(min(label.width, space - 0.8)).move_to(RIGHT * (wood.get_right()[0] + space / 2))
            self.play(Create(ring), FadeIn(label), Flash(ring, color=GOLD, line_length=0.2), run_time=1.0)

        self.beat(para(13, 3), question, pad=2.0)
        self.clear_all()


# ================================================================================================
# 14. what we know
# ================================================================================================
DEMO_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'demo')  # record_demo.js


def browser_window(width, height, url):
    """A plain browser window around a page of width x height: a dark bar with three dots and the address.
    Returns the window and the centre of its page area."""
    bar = 0.46
    body = RoundedRectangle(width=width + 0.12, height=height + bar + 0.1, corner_radius=0.14, stroke_width=0).set_fill('#2A2F38', 1)
    top = body.get_top()[1]
    bar_y = top - bar / 2 - 0.02
    dots = VGroup(*[Circle(0.075, stroke_width=0).set_fill(c, 1) for c in (P_RED, GOLD, GOOD)]).arrange(RIGHT, buff=0.12)
    dots.move_to(np.array([body.get_left()[0] + 0.45, bar_y, 0]))
    address = RoundedRectangle(width=width * 0.5, height=0.3, corner_radius=0.15, stroke_width=0).set_fill('#1B1F26', 1)
    address.move_to(np.array([0, bar_y, 0]))
    text = caption(url, INK, 20).move_to(address)
    page_centre = np.array([0, top - bar - 0.04 - height / 2, 0])
    return VGroup(body, dots, address, text), page_centre


def pointer():
    """A mouse pointer; its first vertex is the tip."""
    pts = [(0, 0), (0, -0.4), (0.1, -0.31), (0.18, -0.47), (0.24, -0.44), (0.16, -0.29), (0.29, -0.29)]
    return Polygon(*[np.array([x, y, 0]) for x, y in pts], stroke_color=BLACK, stroke_width=2).set_fill(WHITE, 1)


class Ch14WhatWeKnow(Narrated):
    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath
        cards = VGroup(card('Extremely tight for human players', P_RED, 3.9, 2.2, 28),
                       card('An exact proof is extremely expensive', GOLD, 3.9, 2.2, 28),
                       card('Nobody knows how perfect play ends', P_BLUE, 3.9, 2.2, 28)).arrange(RIGHT, buff=0.35)

        self.beat(para(14, 0), lambda d: self.play(FadeIn(caption('Three things I am now sure of', INK, 40, BOLD).to_edge(UP, buff=0.6)), run_time=1.0))
        for k in range(3):
            self.beat(para(14, k + 1), lambda d, k=k: self.play(FadeIn(cards[k], shift=UP * 0.3), run_time=1.0))
        self.clear_all(0.8)

        def ending(d):
            b = Board(s=0.32)
            # a question mark made of pieces: red's single hex is the dot, three blue pieces build the curve above it
            path = [(0, -3), (1, -4), (2, -4), (3, -4), (3, -3), (3, -2), (2, -1), (1, 0), (0, 1), (-1, 2)]
            c = [idx(qq, rr) for qq, rr in path]
            dot = piece_on(b, [idx(-2, 4)], P_RED)
            self.play(FadeIn(b), run_time=0.8)
            self.click(0.2)
            self.play(FadeIn(dot, scale=1.3), run_time=0.6)
            self.wait(0.4)
            for cells in (c[7:10], c[4:7], c[0:4]):  # 3I, 3V, 4A, from the dot upwards
                self.click(0.25)
                self.play(FadeIn(piece_on(b, cells, P_BLUE), scale=1.2), run_time=max(0.6, d * 0.1))

        self.beat(para(14, 4), ending, pad=2.0)
        self.clear_all(1.0)

        def call_to_action(d):
            # the game in a browser, recorded from the live page; the pointer follows the recorded mouse
            demo = json.load(open(os.path.join(DEMO_DIR, 'raw.json')))
            t0, W, H = demo['t0'], demo['width'], demo['height']
            times = [f['t'] - t0 for f in demo['frames']]
            files = [os.path.join(DEMO_DIR, 'raw', f['file']) for f in demo['frames']]
            moves = [(c['t'] - t0, c['x'], c['y']) for c in demo['cursor']]
            move_times = [m[0] for m in moves]
            clicks = [c['t'] - t0 for c in demo['cursor'] if c.get('click')]
            width = 9.4
            height = width * H / W
            win, page_centre = browser_window(width, height, 'boltkey.github.io/snekagon-lab')
            offset = np.array([-1.5, 0.15, 0])
            win.shift(offset)
            shot = ImageMobject(files[0]).set_width(width).move_to(page_centre + offset)
            left, top = shot.get_left()[0], shot.get_top()[1]

            def at(x, y):
                return np.array([left + x / W * width, top - y / H * height, 0])

            arrow = pointer().scale(0.85)
            arrow.shift(at(W * 0.55, H * 0.75) - arrow.get_vertices()[0])
            ripple = Circle(0.1, color=GOLD, stroke_width=4).set_stroke(opacity=0)
            self.play(FadeIn(win), FadeIn(shot), FadeIn(arrow), run_time=0.8)
            self.add(ripple)
            state = {'k': 0}

            def show(t):
                k = max(0, bisect.bisect_right(times, t) - 1)
                if k != state['k']:
                    shot.pixel_array = np.asarray(PILImage.open(files[k]).convert('RGBA'))
                    state['k'] = k
                j = bisect.bisect_right(move_times, t) - 1
                if j >= 0:
                    p = at(moves[j][1], moves[j][2])
                    arrow.shift(p - arrow.get_vertices()[0])
                    since = [t - c for c in clicks if 0 <= t - c < 0.45]
                    if since:
                        u = since[-1] / 0.45
                        ripple.set_width(0.15 + 0.6 * u).move_to(p).set_stroke(opacity=1 - u)
                    else:
                        ripple.set_stroke(opacity=0)

            def segment(a, b, *others):
                # the recording from a to b seconds, played 1.3 times as fast
                self.play(UpdateFromAlphaFunc(shot, lambda m, alpha: show(a + (b - a) * alpha), run_time=(b - a) / 1.3, rate_func=linear),
                          *others)

            length = demo['t1'] - t0
            text, b0, dur = self._beat
            split = min(length, max(0.5, (b0 + dur * text.index('To buy the game') / len(text) - self.renderer.time) * 1.3))
            segment(0, split)
            game = Group(photo('published.jpg', 2.0, tilt=2), maty_logo(0.7)).arrange(DOWN, buff=0.3).move_to(np.array([5.3, -0.6, 0]))
            segment(split, length, FadeIn(game, run_time=0.8))

        self.beat(para(14, 5), call_to_action, pad=0.8)
        self.clear_all(0.8)
        # end card
        title = VGroup(Text('Snekagon', font=FONT, font_size=80, weight=BOLD, color=INK),
                       caption('made by Adam Španěl', INK, 34)).arrange(DOWN, buff=0.3)
        lines = Group(caption('play against the engine: boltkey.github.io/snekagon-lab', MUTED, 28),
                      Group(caption('the game:', MUTED, 28), maty_logo(0.55)).arrange(RIGHT, buff=0.25),
                      caption('engine, solver and the Manim code of this video written by Claude Opus 5.5', MUTED, 28),
                      caption('animated with Manim · thank you, Grant Sanderson (3Blue1Brown)', MUTED, 28)).arrange(DOWN, buff=0.25)
        Group(title, lines).arrange(DOWN, buff=0.7)
        self.play(FadeIn(title), FadeIn(lines, shift=UP * 0.2), run_time=1.0)
        self.wait(3.0)
        self.clear_all(1.0)


CHAPTERS2 = ['Ch08Engine', 'Ch09Solving', 'Ch10Opening', 'Ch11Move76', 'Ch12WhyHard', 'Ch13Interlude', 'Ch14WhatWeKnow']

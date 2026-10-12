"""Chapters 1-7 of the Snekagon video. Each chapter is its own scene; build.py renders and joins them.

Draft render of one chapter:  .venv/Scripts/manim -ql chapters.py Ch05Rules
"""
import json
import os

from snekviz import *  # noqa: F401,F403
from script import para, split_at


# ================================================================================================
# chapter-card illustrations
# ================================================================================================
def art_pieces():
    """The two piece sets, laid out like the overview in the rulebook (red has the extra 1-hex)."""
    red = VGroup(*[loose_piece(n, P_RED, s=0.2) for n in ('1', '2', '3I', '3V', '4I', '4A', '4B', '4C')]).arrange(RIGHT, buff=0.3)
    blue = VGroup(*[loose_piece(n, P_BLUE, s=0.2) for n in ('2', '3I', '3V', '4I', '4A', '4B', '4C')]).arrange(RIGHT, buff=0.3)
    return VGroup(red, blue).arrange(DOWN, buff=0.45)


def art_sketch():
    """A pencil sketch on a patch of hexes: outlines only, two snakes shaded in."""
    s = 0.3
    grid = VGroup(*[hex_shape(hex_xy(q, r, s), s, fill=BG, fill_opacity=0, stroke=MUTED, sw=1.2)
                    for q in range(-3, 4) for r in range(-2, 3) if abs(q + r) <= 3])

    def shade(cells, color):
        return VGroup(*[hex_shape(hex_xy(q, r, s), s, fill=color, fill_opacity=0.3, stroke=color, sw=2.5) for q, r in cells])

    return VGroup(grid, shade([(-1, 0), (0, 0), (1, -1), (1, -2)], P_RED), shade([(-2, 2), (-1, 2), (0, 1)], P_BLUE))


def art_ripples():
    """One red hex and rings of cells fading outwards: the game going out into the world."""
    s, rings = 0.26, VGroup()
    for k in range(5):
        ring = VGroup(*[hex_shape(hex_xy(q, r, s), s, fill=P_RED if k == 0 else BG, fill_opacity=1 if k == 0 else 0,
                                  stroke=P_RED if k == 0 else INK, sw=1.5)
                        for q in range(-k, k + 1) for r in range(-k, k + 1) if max(abs(q), abs(r), abs(q + r)) == k])
        if k:
            ring.set_stroke(opacity=0.75 - 0.16 * k)
        rings.add(ring)
    return rings


def art_rules():
    b = Board(s=0.17)
    pieces = VGroup(piece_on(b, [59, 60, 61, 69], P_BLUE, 0.17), piece_on(b, [83, 91, 92, 93], P_RED, 0.17), piece_on(b, [38], P_RED, 0.17))
    return VGroup(b, pieces)


def art_snakes():
    red = shape_on_patch([(-3, 0), (-2, 0), (-1, -1), (0, -1), (1, -1), (2, -2), (3, -2)], 0.32, P_RED)
    blue = shape_on_patch([(-3, 2), (-2, 2), (-1, 1), (0, 1), (1, 0), (2, 0), (3, -1)], 0.32, P_BLUE)
    return VGroup(red, blue)


# ================================================================================================
# 1. cold open + title
# ================================================================================================
class Ch01ColdOpen(Narrated):
    def construct(self):
        board = Board(s=0.34)

        def draw_board(d):
            rings = [VGroup(*board.ring(k)) for k in range(6)]
            self.play(LaggedStart(*[FadeIn(r, scale=0.8) for r in rings], lag_ratio=0.35), run_time=d * 0.4)
            self.supply, self.supply_group = piece_supply(board, WIN_GAME)
            self.until('a handful of small pieces')
            self.play(LaggedStart(*[FadeIn(pc, shift=UP * 0.15) for g in self.supply_group for pc in g], lag_ratio=0.06), run_time=1.2)
            first, target, k = self.supply.pop(WIN_GAME[0])
            self.click(0.85)
            self.play(first.animate.scale(1 / k).move_to(target), run_time=0.9)
            self.play(Flash(board.at(60), color=P_RED, line_length=0.25), run_time=0.5)
            self.first = first

        self.beat(split_at(para(1, 0), ['You take turns'])[0], draw_board)

        red, blue = self.counters(board)
        self.set_count(red, 1, P_RED)
        placed = {0: {60}, 1: set()}

        def first_moves(d):
            # the words of the rule follow the board: blue's first piece starts a new snake, red's second grows
            self.play(FadeIn(red), FadeIn(blue), run_time=0.5)
            self.pieces = [self.first]

            def play(k, per_move=1.0):
                pcs, _ = self.play_moves(board, WIN_GAME[k:k + 1], k, per_move=per_move, counters=(red, blue), placed=placed,
                                         supply=self.supply)
                self.pieces += pcs

            self.until('either start a new snake')
            play(1)
            cells = move_cells(WIN_GAME[1])
            around = {n for c in cells for n in neighbours(c)} - set(cells)
            halo = VGroup(*[board.cells[c].copy().set_fill(INK, 0.18).set_stroke(width=0) for c in around])
            new = VGroup(caption('new snake', P_BLUE, 32).to_edge(DOWN, buff=0.35), outline_on(board, cells))
            self.play(FadeIn(new[0]), Create(new[1]), FadeIn(halo), run_time=0.8)
            self.until('or grow one of your snakes')
            self.play(FadeOut(new), FadeOut(halo), run_time=0.25)
            play(2, 0.8)
            grow = VGroup(caption('grow', P_RED, 32).to_edge(DOWN, buff=0.35), outline_on(board, move_cells(WIN_GAME[2])))
            self.play(FadeIn(grow[0]), Create(grow[1]), run_time=0.5)
            self.until('with the longest snake')
            self.play(FadeOut(grow), run_time=0.4)
            play(3)
            play(4)
            self.play(Indicate(red[1], color=GOLD), Indicate(blue[1], color=GOLD), run_time=0.8)

        self.beat(split_at(para(1, 0), ['You take turns'])[1], first_moves)

        def rest_of_game(d):
            pcs, _ = self.play_moves(board, WIN_GAME[5:], 5, per_move=0.9, counters=(red, blue), placed=placed, supply=self.supply)
            self.pieces += [p for p in pcs if p is not None]
            self.play(red[1].animate.set_color(GOLD), blue[1].animate.set_color(GOLD), run_time=0.5)
            self.wait(0.6)

        self.beat(para(1, 1), rest_of_game)

        def tree(d):
            everything = VGroup(board, *self.pieces)
            on_board = {id(p) for p in self.pieces}
            left = VGroup(*[pc for g in self.supply_group for pc in g if id(pc) not in on_board])
            self.play(everything.animate.scale(0.32).to_corner(UL, buff=0.35), FadeOut(red), FadeOut(blue), FadeOut(left), run_time=1.2)
            root = everything.get_corner(DR) + np.array([0.15, -0.1, 0])
            lines, parents, width = VGroup(), [root], 12.5
            for depth, fan in enumerate([6, 5, 4, 3]):
                kids, n = [], len(parents) * fan
                y = root[1] - 1.15 * (depth + 1)
                for j, par in enumerate(parents):
                    for k in range(fan):
                        x = -width / 2 + width * (j * fan + k + 0.5) / n + 0.6
                        child = np.array([x, y, 0])
                        lines.add(Line(par, child, stroke_width=max(0.4, 2.2 - 0.5 * depth), color=INK,
                                       stroke_opacity=max(0.12, 0.8 - 0.2 * depth)))
                        kids.append(child)
                parents = kids
            self.play(LaggedStart(*[Create(l) for l in lines], lag_ratio=0.004), run_time=d * 0.45)
            row = VGroup(*[Square(0.11, stroke_width=0.8, color=MUTED).set_fill(MUTED, 0.25) for _ in range(76)])
            row.arrange(RIGHT, buff=0.045).to_edge(DOWN, buff=0.8)
            rank = caption('engine\u2019s ranking', MUTED, 26).next_to(row, UP, buff=0.25)
            first = caption('1st', MUTED, 24).next_to(row[0], DOWN, buff=0.2)
            last = caption('76th', GOLD, 30, BOLD).next_to(row[-1], DOWN, buff=0.25)
            self.play(lines.animate.set_stroke(opacity=0.12), FadeIn(rank), FadeIn(first),
                      LaggedStart(*[FadeIn(sq) for sq in row], lag_ratio=0.03), run_time=d * 0.18)
            self.until('76th')
            self.play(row[-1].animate.set_fill(GOLD, 1).set_stroke(GOLD).scale(1.8), Flash(row[-1], color=GOLD), FadeIn(last, shift=UP * 0.15), run_time=1.0)
            # the punch line: the weak-looking move that changed everything
            self.until('changed the whole picture')
            self.play(row[-1].animate.scale(1.5), Flash(row[-1], color=GOLD, line_length=0.35, flash_radius=0.4),
                      lines.animate.set_stroke(opacity=0.05), run_time=0.9)

        self.beat(para(1, 2), tree, pad=1.2)

        self.clear_all(1.0)
        title = Text('Snekagon', font=FONT, font_size=110, weight=BOLD, color=INK)
        sub = caption('the 7 turn game that could not be solved', MUTED, 40)
        VGroup(title, sub).arrange(DOWN, buff=0.45)
        snake = shape_on_patch([(k - 6, 0 if k < 6 else 0) for k in range(12)], 0.28, P_RED).next_to(sub, DOWN, buff=0.8)
        # no chime: it clashed with the background music
        self.play(Write(title), run_time=1.6)
        self.play(FadeIn(sub, shift=UP * 0.2), FadeIn(snake, shift=RIGHT * 0.5), run_time=1.6)
        self.wait(1.8)
        self.clear_all(0.8)


# ================================================================================================
# 2. who I am
# ================================================================================================
class Ch02WhoIAm(Narrated):
    def construct(self):
        chapter_card(self, 1, 'How it started', art_sketch())  # right after the title screen
        ph = on_camera_placeholder(para(2, 0), 'Adam on camera, or a slow pan over the Project L and Drillers boxes on a shelf', 8, 4.5)

        self.beat(para(2, 0),
                  lambda d: self.play(FadeIn(ph), run_time=1.0))

        def box_and_prototype(d):
            if len(ph):  # the placeholder, unless Adam's footage replaced it
                self.play(FadeOut(ph), run_time=0.5)
            published = photo('published.jpg', 4.0, 'Snekagon, Maty Moves 2020', tilt=-3).shift(LEFT * 3.3 + UP * 0.2)
            proto = photo('prototype.jpg', 5.6, 'the prototype', tilt=3).shift(RIGHT * 3.6)
            # photos fade in without moving: a moving bitmap is resampled every frame and shimmers
            self.play(FadeIn(published), run_time=0.8)
            self.play(FadeIn(proto), run_time=0.9)

        self.beat(para(2, 1), box_and_prototype)
        self.clear_all()


# ================================================================================================
# 3. how it started
# ================================================================================================
class Ch03HowItStarted(Narrated):
    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath

        def sketch(d):
            note = art_sketch().scale(1.3).shift(LEFT * 2.6)
            proto = photo('prototype_board.jpg', 4.6, tilt=-2).shift(RIGHT * 3.9)
            self.play(LaggedStart(*[Create(h) for h in note[0]], lag_ratio=0.03), run_time=d * 0.35)
            self.play(LaggedStart(*[DrawBorderThenFill(h) for h in note[1]], lag_ratio=0.25), run_time=d * 0.12)
            self.play(LaggedStart(*[DrawBorderThenFill(h) for h in note[2]], lag_ratio=0.25), run_time=d * 0.1)
            self.play(FadeIn(proto), run_time=0.8)  # no motion: a moving photo shimmers
            self.sk = (note, proto)

        self.beat(para(3, 0), sketch)

        board = Board(s=0.32)
        # the pencil sketch grows into the real board (its hexes were added one by one: regroup them first)
        note, proto = self.sk
        self.remove(*note[0], *note[1], *note[2])
        self.add(note[0], note[1], note[2])
        self.play(FadeOut(proto), FadeOut(note[1]), FadeOut(note[2]), ReplacementTransform(note[0], board), run_time=1.2)

        def fills_up(d):
            pcs, placed = self.play_moves(board, SKETCH_GAME[:3], 0, per_move=1.3)
            # blue's U lands exactly on "a threat and a wall"
            self.until('a threat and a wall')
            u, _ = self.play_moves(board, SKETCH_GAME[3:4], 3, per_move=1.3, placed=placed)
            wall = outline_on(board, move_cells(SKETCH_GAME[3]), GOLD)
            note = caption('a threat and a wall', GOLD, 30).to_edge(DOWN, buff=0.3)
            self.play(Create(wall), FadeIn(note), run_time=1.0)
            self.wait(0.8)
            more, _ = self.play_moves(board, SKETCH_GAME[4:8], 4, per_move=1.1, placed=placed)
            self.extra = VGroup(*[p for p in pcs + u + more if p], wall, note)

        self.beat(para(3, 1), fills_up)
        self.clear_all(0.6)

        lines = ['“This game keeps fascinating me.', 'An area-control abstract that lasts eight moves?',
                 'That can’t work. But it does.', 'You can never calculate more than seven moves ahead,',
                 'simply because there will never be that many moves left.',
                 'It is about planning and intuition, and every game is different.”']
        card = paper_card(lines, 'Diary, February 2020, after a playtest at CGE', width=10, size=26)

        def diary(d):
            self.play(FadeIn(card[0]), FadeIn(card[1][0]), run_time=0.6)
            body = card[1][1]
            run = d * 0.85
            # typed up letter by letter, a quiet key tap for each: an uneven human rhythm, a short rest between lines,
            # a caret after the last letter
            rnd = np.random.default_rng(3)
            per = run / len(body)
            typed = []  # (letter, line, time)
            for k, line in enumerate(body):
                gaps = rnd.uniform(0.5, 1.5, len(line))
                gaps = gaps / gaps.sum() * per * 0.8
                for ch, t in zip(line, k * per + np.concatenate([[0], np.cumsum(gaps)[:-1]])):
                    typed.append((ch, k, t))
                    self.sfx('key', 5, t, gain=-22)
            body.set_opacity(0)
            caret = Rectangle(width=0.035, height=0.34, stroke_width=0).set_fill(PAPER_INK, 1)
            caret.move_to(np.array([body[0].get_left()[0] + 0.02, body[0].get_y(), 0]))

            def typing(m, a):
                now, last = a * run, None
                for ch, k, t in typed:
                    ch.set_opacity(1 if now >= t else 0)
                    if now >= t:
                        last = (ch, k)
                if last:
                    caret.move_to(np.array([last[0].get_right()[0] + 0.06, body[last[1]].get_y(), 0]))
                caret.set_opacity(1 if (now * 2.2) % 1 < 0.6 or not last or now < typed[-1][2] + 0.1 else 0)  # blinks once done

            self.add(body, caret)
            self.play(UpdateFromAlphaFunc(body, typing), run_time=run, rate_func=linear)
            self.play(FadeOut(caret), run_time=0.3)

        self.beat(para(3, 2), diary)
        self.clear_all(0.6)

        def small(d):
            b = Board(s=0.22).shift(LEFT * 4.6)
            facts = VGroup(caption('91 cells', INK, 34), caption('7 turns each', INK, 34),
                           caption('nothing hidden', INK, 34)).arrange(DOWN, aligned_edge=LEFT, buff=0.35)
            facts.shift(RIGHT * 2.1 + UP * 1.6)
            self.play(FadeIn(b), run_time=0.6)
            self.play(LaggedStart(*[FadeIn(f, shift=RIGHT * 0.2) for f in facts], lag_ratio=0.6), run_time=d * 0.35)
            outcomes = VGroup(*[VGroup(RoundedRectangle(width=2.7, height=1.0, corner_radius=0.12, stroke_color=c, stroke_width=2.5),
                                       caption(t, c, 21)) for t, c in (('first player wins', P_RED), ('second player wins', P_BLUE), ('tie', GOLD))])
            for o in outcomes:
                o[1].move_to(o[0])
            outcomes.arrange(RIGHT, buff=0.2).shift(RIGHT * 2.1 + DOWN * 1.4)
            self.play(LaggedStart(*[FadeIn(o, scale=0.9) for o in outcomes], lag_ratio=0.4), run_time=d * 0.3)
            q = Text('?', font=FONT, font_size=72, color=GOLD, weight=BOLD).next_to(outcomes, UP, buff=0.15)
            self.play(FadeIn(q, scale=1.5), run_time=0.6)
            # the games that have been worked out, and Snekagon at the end of the row
            self.until('It really feels like a game')
            self.play(FadeOut(VGroup(b, facts, outcomes, q)), run_time=0.5)
            cards = solved_row()
            for k, c in enumerate(cards):
                self.play(FadeIn(c, shift=UP * 0.2), run_time=0.4 if k < 4 else 0.6)
            self.play(Indicate(cards[-1], color=GOLD), run_time=0.8)

        self.beat(para(3, 3), small, pad=1.0)
        self.clear_all()


# ================================================================================================
# 4. out into the world
# ================================================================================================
class Ch04OutIntoTheWorld(Narrated):
    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath

        email = paper_card(['Thank you for your game proposition. […]',
                            'Strategy part seems a little bit too light for us […]'],
                           'From: Gigamic · 10 December 2019 · RE: Snekagon game pitch', width=9, size=26)
        email.shift(UP * 0.6)

        def rejection(d):
            when = caption('December 2019', MUTED, 28).to_edge(UP, buff=0.4)
            self.play(FadeIn(when), FadeIn(email, shift=UP * 0.2), run_time=d * 0.25)
            body = email[1][1]
            hl1 = SurroundingRectangle(body[1], color=GOLD, buff=0.05, stroke_width=3)
            self.play(Create(hl1), run_time=d * 0.15)
            self.rej = VGroup(when, hl1)

        self.beat(split_at(para(4, 0), ['I tried to ask', 'In the end'])[0], rejection)

        reply = paper_card(['Which player did you find has the advantage,', 'and which winning strategies have you found?',
                            'I tried to solve the game using a minimax algorithm […]'], 'My reply', width=9, size=26)
        reply.next_to(email, DOWN, buff=0.35)

        def answer(d):
            self.play(VGroup(email, self.rej[1:]).animate.shift(UP * 0.9), FadeOut(self.rej[0]), run_time=0.6)
            reply.next_to(email, DOWN, buff=0.35)
            self.play(FadeIn(reply[0]), FadeIn(reply[1][0]), run_time=0.5)
            self.type_text(reply[1][1][0], d * 0.2)
            self.type_text(reply[1][1][1], d * 0.2)
            self.type_text(reply[1][1][2], d * 0.2)
            silence = caption('no reply', MUTED, 30).next_to(reply, DOWN, buff=0.3)
            self.until('never heard back')
            self.play(FadeIn(silence), run_time=0.6)

        self.beat(split_at(para(4, 0), ['I tried to ask', 'In the end'])[1], answer)
        self.clear_all(0.6)

        def timeline(d):
            axis = Line(LEFT * 6, RIGHT * 6, color=FAINT, stroke_width=3).shift(UP * 0.4)
            stops = [(-3.0, 'Dec 2019', 'Gigamic', 'declined', BAD), (3.0, '2020', 'Maty Moves', 'published', GOOD)]
            dots, texts = VGroup(), Group()
            for x, when, what, sub, col in stops:
                dots.add(Dot(np.array([x, 0.4, 0]), radius=0.11, color=col))
                if what == 'Maty Moves':  # the publisher's own logo instead of its name
                    texts.add(Group(caption(when, MUTED, 24).move_to(np.array([x, 1.0, 0])), maty_logo(0.8).move_to(np.array([x, -0.3, 0])),
                                    caption(sub, MUTED, 22).move_to(np.array([x, -0.95, 0]))))
                else:
                    texts.add(VGroup(caption(when, MUTED, 24).move_to(np.array([x, 1.0, 0])), caption(what, col, 28, BOLD).move_to(np.array([x, -0.2, 0])),
                                     caption(sub, MUTED, 22).move_to(np.array([x, -0.65, 0]))))
            self.play(Create(axis), run_time=0.6)
            for k in range(len(stops)):
                self.play(FadeIn(dots[k], scale=1.5), FadeIn(texts[k], shift=UP * 0.15), run_time=d * 0.17)
            box = photo('published.jpg', 2.3).move_to(np.array([3.0, -2.45, 0]))
            self.play(FadeIn(box), run_time=0.8)

        self.beat(split_at(para(4, 0), ['I tried to ask', 'In the end'])[2], timeline, pad=1.5)
        self.clear_all(0.6)

        def reception(d):
            # Snekagon's BoardGameGeek entry with the numbers from the site (October 2026), and beside it the
            # list of ratings as it looks there, the reviewers' names and avatars blurred (redact_bgg.py)
            site = caption('BoardGameGeek', MUTED, 22).move_to(np.array([-3.6, 3.35, 0]))
            badge = RegularPolygon(6, color='#5C6675', fill_opacity=1, stroke_width=0).rotate(PI / 6).scale(0.6)
            title = VGroup(caption('Snekagon (2020)', INK, 40, BOLD), caption('13 ratings · 5 comments', MUTED, 26))
            title.arrange(DOWN, aligned_edge=LEFT, buff=0.15)
            VGroup(badge, title).arrange(RIGHT, buff=0.35).move_to(np.array([-3.6, 2.45, 0]))
            avg = ValueTracker(0)
            num = always_redraw(lambda: caption('%.1f' % avg.get_value(), INK, 38, BOLD).move_to(badge))
            counts = {10: 0, 9: 0, 8: 1, 7: 1, 6: 1, 5: 5, 4: 3, 3: 2, 2: 0, 1: 0}
            colours = {10: GOOD, 9: GOOD, 8: GOOD, 7: '#4FA3E0', 6: '#5369A2', 5: '#5369A2', 4: BAD, 3: BAD, 2: BAD, 1: BAD}
            frame, bars = VGroup(), VGroup()
            for i, k in enumerate(range(10, 0, -1)):
                y = 1.25 - 0.36 * i
                track = RoundedRectangle(corner_radius=0.05, width=3.8, height=0.22, stroke_width=0, fill_color=FAINT, fill_opacity=1)
                track.move_to(np.array([-3.5, y, 0]))
                bar = RoundedRectangle(corner_radius=0.05, width=max(0.07, 3.8 * counts[k] / 5), height=0.22, stroke_width=0,
                                       fill_color=colours[k], fill_opacity=1).align_to(track, LEFT).set_y(y)
                frame.add(track, caption(str(k), MUTED, 22).next_to(track, LEFT, buff=0.25),
                          caption(str(counts[k]), MUTED, 20).next_to(track, RIGHT, buff=0.2))
                bars.add(bar)
            detail = caption('average 4.846 from 13 ratings', MUTED, 24).move_to(np.array([-3.6, -2.75, 0]))
            shot = ImageMobject(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'bgg_ratings.png')).set_height(6.4)
            paper = RoundedRectangle(corner_radius=0.12, width=shot.width + 0.35, height=shot.height + 0.35, stroke_width=0,
                                     fill_color=WHITE, fill_opacity=1)
            Group(paper, shot).move_to(np.array([3.7, 0, 0]))
            self.play(FadeIn(site), FadeIn(badge), FadeIn(title), run_time=0.6)
            self.add(num)
            self.play(avg.animate.set_value(4.8), run_time=1.0)
            num.clear_updaters()
            self.play(FadeIn(frame), LaggedStart(*[GrowFromEdge(b, LEFT) for b in bars], lag_ratio=0.12), run_time=1.6)
            self.play(FadeIn(detail), run_time=0.4)
            self.until('to put it kindly')
            self.play(FadeIn(Group(paper, shot)), run_time=0.8)

        self.beat(para(4, 1), reception)
        self.clear_all(0.5)

        hi = '<span foreground="%s" weight="bold">%%s</span>' % GOLD
        q1 = quote_card((hi % 'First player advantage') + ' seems to\n' + (hi % 'take the legs out from underneath this design') +
                        '\nas I can\'t see how you would ever get\nto a different board state aside from forcing\nplayers to play differently or something.',
                        width=12.5, size=36).move_to(ORIGIN)
        q2 = quote_card('Blokus, on a hex board, with fewer pieces.\nin my two games, ' + (hi % 'going first gave me\na near-unbeatable advantage') +
                        ' against opponents\nwho played too close. Maybe there\'s a little more\ngame here if player 2 starts near the edge\n(or ' +
                        (hi % 'player 1 doesn\'t take the center space...\nbut why wouldn\'t you?') + ').\nStill, ' + (hi % 'no fun to feel like a bully.'),
                        width=12.5, size=32).move_to(ORIGIN)
        self.beat("First player advantage seems to take the legs out from underneath this design.",
                  lambda d: self.play(FadeIn(q1, shift=RIGHT * 0.2), run_time=1.0))
        self.beat("Going first gave me a near-unbeatable advantage. Maybe if player one doesn't take the center space... but why wouldn't you? Still, no fun to feel like a bully.", lambda d: (self.play(FadeOut(q1), run_time=0.5), self.play(FadeIn(q2, shift=RIGHT * 0.2), run_time=1.0)), pad=1.2)
        self.clear_all(0.6)

        cam = on_camera_placeholder(para(4, 2), 'Adam on camera, calm', 8, 4.5)
        self.beat(para(4, 2),
                  lambda d: self.play(FadeIn(cam), run_time=1.0))
        self.clear_all()


# ================================================================================================
# 5. the rules (an ordinary engine game; situations drawn like the printed rules)
# ================================================================================================
class Ch05Rules(Narrated):
    def construct(self):
        chapter_card(self, 2, 'Diving deeper into the game', art_rules())
        board = Board(s=0.34)
        names = ['2', '3I', '3V', '4I', '4A', '4B', '4C']

        def board_and_pieces(d):
            self.play(FadeIn(board, scale=0.95), run_time=d * 0.12)
            n91 = caption('91 cells', INK, 34).next_to(board, DOWN, buff=0.25)
            side = [board.cells[i] for i in (5, 6, 7, 8, 9, 10)]
            self.play(FadeIn(n91), *[s.animate.set_stroke(GOLD, 3) for s in side], run_time=d * 0.12)
            six = caption('6 per side', GOLD, 26).next_to(VGroup(*side), UP, buff=0.12)
            self.play(FadeIn(six), run_time=0.5)
            self.play(*[s.animate.set_stroke(FAINT, 1.2) for s in side], FadeOut(six), run_time=0.4)
            self.play(VGroup(board, n91).animate.shift(LEFT * 3.4), run_time=d * 0.1)
            shapes = VGroup(*[loose_piece(n, P_BLUE, s=0.26) for n in names])
            top = VGroup(*shapes[:3]).arrange(RIGHT, buff=0.7)
            bottom = VGroup(*shapes[3:]).arrange(RIGHT, buff=0.55)
            fan = VGroup(top, bottom).arrange(DOWN, buff=0.75)
            fan.scale_to_fit_width(min(fan.width, 5.4)).move_to(RIGHT * 3.7 + UP * 0.4)  # between the board and the edge, room to turn
            self.play(LaggedStart(*[FadeIn(sh, shift=UP * 0.2) for sh in shapes], lag_ratio=0.15), run_time=d * 0.22)
            self.play(*[Rotate(sh, angle=PI / 3) for sh in shapes], run_time=0.8)
            self.play(*[sh.animate.stretch(-1, 0) for sh in shapes], run_time=0.8)
            self.play(*[Rotate(sh, angle=-PI / 3) for sh in shapes], run_time=0.6)
            labels = VGroup(*[caption(n, MUTED, 22).next_to(shapes[k], DOWN, buff=0.18) for k, n in enumerate(names)])
            self.play(FadeIn(labels), run_time=0.5)
            self.shapes, self.labels, self.n91 = shapes, labels, n91

        self.beat(para(5, 0), board_and_pieces)

        def single_hex(d):
            one = loose_piece('1', P_RED, s=0.34).move_to(RIGHT * 3.4 + UP * 3.0)
            lab = caption('only red', P_RED, 24).next_to(one, RIGHT, buff=0.25)
            self.play(GrowFromCenter(one), FadeIn(lab), run_time=d * 0.15)
            self.click(d * 0.19)
            self.play(one.animate.move_to(board.at(38)), run_time=d * 0.2)
            self.play(FadeOut(lab), run_time=0.4)
            turns = caption('1 hex, then 7 turns each', INK, 30).move_to(RIGHT * 3.4 + UP * 3.0)
            self.play(FadeIn(turns), run_time=d * 0.12)
            self.one, self.turns = one, turns

        self.beat(para(5, 1),
                  single_hex)

        def tallies(d):
            red_sum = caption('1 + 2 + 3 + 3 + 4 + 4 + 4 + 4 = 25', P_RED, 28)
            blue_sum = caption('2 + 3 + 3 + 4 + 4 + 4 + 4 = 24', P_BLUE, 28)
            self.sums = VGroup(red_sum, blue_sum).arrange(DOWN, aligned_edge=RIGHT, buff=0.25)
            self.sums.scale_to_fit_width(min(self.sums.width, 6.0)).move_to(RIGHT * 3.6 + DOWN * 2.6)  # inside the frame
            self.play(Write(red_sum), run_time=d * 0.3)
            self.play(Write(blue_sum), run_time=d * 0.3)
            self.play(Indicate(red_sum[-2:], color=GOLD), Indicate(blue_sum[-2:], color=GOLD), run_time=1.0)

        self.beat(para(5, 2), tallies)

        red, blue = self.counters(board)
        placed = {0: {38}, 1: set()}
        self.set_count(red, 1, P_RED)

        def opening(d):
            self.play(FadeOut(VGroup(self.shapes, self.labels, self.sums, self.turns, self.n91)),
                      VGroup(board, self.one).animate.shift(RIGHT * 3.4), run_time=1.0)
            self.play(FadeIn(red), FadeIn(blue), run_time=0.4)
            pcs, _ = self.play_moves(board, RULES_GAME[1:2], 1, per_move=min(1.2, d * 0.3), counters=(red, blue), placed=placed)
            two = caption('two ways', INK, 32).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(two), run_time=0.6)
            self.pieces = [self.one] + pcs
            self.cap = two

        self.beat(para(5, 3), opening)

        def new_snake(d):
            cap = caption('A)  new snake', INK, 32).to_edge(DOWN, buff=0.35)
            cells = move_cells(RULES_GAME[2])
            p = piece_on(board, cells, P_RED)
            self.click(d * 0.12)
            self.play(FadeOut(self.cap), FadeIn(cap), FadeIn(p, scale=1.25), run_time=d * 0.15)
            good = outline_on(board, cells)
            ok = caption('\u2713', GOOD, 30, BOLD).next_to(board, LEFT, buff=0.3).shift(DOWN * 1.6)
            placed[0] |= set(cells)
            self.play(Create(good), FadeIn(ok), self.count_to(red, longest_snake(placed[0]), P_RED), run_time=d * 0.15)
            # the same piece placed against blue: not allowed
            wrong_cells = [70, 71, 82, 93]
            ghost_board, ghost_pieces = board_with(RULES_GAME, 2, 0.2)
            ghost = VGroup(ghost_board, ghost_pieces, piece_on(ghost_board, wrong_cells, P_RED, 0.2),
                           outline_on(ghost_board, wrong_cells, WHITE, 3)).move_to(RIGHT * 4.9 + DOWN * 0.3)
            bad = caption('\u2717  touches blue', BAD, 28, BOLD).next_to(ghost, DOWN, buff=0.2)
            cross = Cross(ghost[2], stroke_color=BAD, stroke_width=5).scale(1.4)
            self.play(VGroup(board, *self.pieces, p, good, ok).animate.shift(LEFT * 1.6), FadeIn(ghost), run_time=d * 0.2)
            self.play(FadeIn(bad), Create(cross), run_time=d * 0.15)
            self.play(FadeOut(VGroup(ghost, bad, cross, ok, good)), VGroup(board, *self.pieces, p).animate.shift(RIGHT * 1.6), run_time=0.8)
            self.pieces.append(p)
            self.cap = cap

        self.beat(para(5, 4), new_snake)

        def grow(d):
            cap = caption('B)  grow', INK, 32).to_edge(DOWN, buff=0.35)
            self.play(FadeOut(self.cap), FadeIn(cap), run_time=0.5)
            pcs, _ = self.play_moves(board, RULES_GAME[3:4], 3, per_move=d * 0.1, counters=(red, blue), placed=placed)
            touch = outline_on(board, move_cells(RULES_GAME[3]))
            note = caption('may touch the opponent', P_BLUE, 26).to_edge(RIGHT, buff=0.35).shift(DOWN * 1.2)
            self.play(Create(touch), FadeIn(note), run_time=d * 0.12)
            self.play(FadeOut(touch), FadeOut(note), run_time=0.4)
            self.pieces += pcs
            pcs, _ = self.play_moves(board, RULES_GAME[4:8], 4, per_move=d * 0.06, counters=(red, blue), placed=placed)
            self.pieces += pcs
            pcs, _ = self.play_moves(board, RULES_GAME[8:9], 8, per_move=d * 0.08, counters=(red, blue), placed=placed)
            self.pieces += pcs
            join = move_cells(RULES_GAME[8])
            snake = snake_of(placed[0], join[0])
            glow = VGroup(*[board.cells[c].copy().set_fill(opacity=0).set_stroke(GOLD, 4) for c in snake])
            note2 = caption('joins two red snakes', P_RED, 26).next_to(board, LEFT, buff=0.3).shift(UP * 1.6)
            self.play(Create(outline_on(board, join)), Create(glow), FadeIn(note2), Flash(red[1], color=GOLD), run_time=d * 0.15)
            self.wait(0.5)
            self.play(FadeOut(glow), FadeOut(note2), run_time=0.5)
            self.cap = cap

        self.beat(para(5, 5), grow)

        def snake_shape(d):
            game = Group(*self.mobjects)
            self.play(FadeOut(game), run_time=0.8)
            examples = [([17, 28, 39], 'sharp turn'), ([63, 74, 85], 'branch'), ([30, 39, 40], 'two snakes touch')]
            boards = VGroup()
            for k, (cells, name) in enumerate(examples):
                b, pcs = board_with(RULES_GAME, 6, 0.2)
                ghost = piece_on(b, cells, P_RED, 0.2)
                frame = outline_on(b, cells, WHITE, 3)
                cross = Cross(ghost, stroke_color=BAD, stroke_width=5).scale(1.6)
                label = caption(name, BAD, 26, BOLD).next_to(b, DOWN, buff=0.25)
                boards.add(VGroup(b, pcs, ghost, frame, cross, label))
            boards.arrange(RIGHT, buff=0.35).shift(UP * 0.3)
            for g in boards:
                self.play(FadeIn(VGroup(g[0], g[1])), run_time=d * 0.05)
                self.play(FadeIn(g[2]), Create(g[3]), run_time=d * 0.07)
                self.play(Create(g[4]), FadeIn(g[5]), run_time=d * 0.07)
            rule = caption('path or ring', GOOD, 30).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(rule), run_time=0.8)
            self.shape_demo = VGroup(boards, rule)

        self.beat(para(5, 6), snake_shape)

        def ending(d):
            self.play(FadeOut(self.shape_demo), run_time=0.6)
            self.play(FadeIn(VGroup(board, *[p for p in self.pieces if p], red, blue)), run_time=0.6)
            pcs, _ = self.play_moves(board, RULES_GAME[9:14], 9, per_move=d * 0.07, counters=(red, blue), placed=placed)
            self.pieces += pcs
            left = loose_piece('4I', P_RED, s=0.3).set_opacity(0.35).move_to(LEFT * 5.7 + DOWN * 1.2)
            passed = caption('pass', P_RED, 30).next_to(left, UP, buff=0.25)
            self.play(FadeIn(left), FadeIn(passed), run_time=d * 0.1)
            glows = VGroup()
            for player in (0, 1):
                best = max(components(placed[player]), key=len)
                glows.add(VGroup(*[board.cells[c].copy().set_fill(opacity=0).set_stroke(GOLD, 4) for c in best]))
            self.play(Create(glows[0]), run_time=d * 0.1)
            self.play(Create(glows[1]), run_time=d * 0.1)
            result = caption('blue wins 21 : 18', P_BLUE, 40, BOLD).to_edge(DOWN, buff=0.35)
            self.play(FadeIn(result, shift=UP * 0.2), blue[1].animate.set_color(GOLD), run_time=0.8)

        self.beat(para(5, 7), ending)
        self.beat(para(5, 8), None, pad=1.0)
        self.clear_all()


# ================================================================================================
# 6. where the strategy hides (moments from the story game)
# ================================================================================================
class Ch06HowItIsPlayed(Narrated):
    def construct(self):
        self.wait(0.5)  # within a big chapter: no card, just a breath
        G = RULES_GAME  # the game from the rules: all three ideas show up in it (game_story.py)

        def overview(d):
            b, pcs = board_with(G, 14, 0.26)
            g = VGroup(b, pcs).shift(LEFT * 3.4)
            ideas = VGroup(*[caption(t, INK, 34) for t in ('1.  Blocking', '2.  Joining snakes', '3.  Flowing around')])
            ideas.arrange(DOWN, aligned_edge=LEFT, buff=0.45).shift(RIGHT * 3.2)
            self.play(FadeIn(g), run_time=1.0)
            self.play(LaggedStart(*[FadeIn(i, shift=RIGHT * 0.2) for i in ideas], lag_ratio=0.5), run_time=d * 0.5)
            self.ov = (g, b, pcs, ideas)

        self.beat(para(6, 0), overview)

        board = Board(s=0.34)
        red, blue = self.counters(board)
        placed = position_after(G, 3)
        pieces = VGroup(*[piece_on(board, move_cells(m), P_RED if j % 2 == 0 else P_BLUE) for j, m in enumerate(G[:3])])
        self.set_count(red, longest_snake(placed[0]), P_RED)
        self.set_count(blue, longest_snake(placed[1]), P_BLUE)
        tag = caption('1.  Blocking', GOLD, 30).to_corner(DL, buff=0.4)
        self.tag6 = tag
        # the finished game rewinds to its third move and grows into the big board
        g0, b0, pcs0, ideas = self.ov
        self.remove(g0)
        self.add(b0, *pcs0)
        self.play(FadeOut(ideas), LaggedStart(*[FadeOut(p, scale=0.9) for p in reversed(pcs0[3:])], lag_ratio=0.12), run_time=1.2)
        self.play(ReplacementTransform(b0, board), *[ReplacementTransform(p, q) for p, q in zip(pcs0[:3], pieces)], run_time=1.0)

        def ring(cells, color, width=4):
            return VGroup(*[board.cells[c].copy().set_fill(opacity=0).set_stroke(color, width) for c in cells])

        def blocking(d):
            # red has just started a second snake; blue's U lands against one of its ends
            self.play(FadeIn(red), FadeIn(blue), FadeIn(tag), run_time=0.6)
            self.play_moves(board, G[3:4], 3, per_move=1.0, counters=(red, blue), placed=placed)
            u = outline_on(board, move_cells(G[3]))
            self.play(Create(u), run_time=d * 0.15)
            dead = board.cells[81].copy().set_fill(MUTED, 0.6).set_stroke(MUTED, 2)  # the only cell left in front of that end
            x = Cross(dead, stroke_color=BAD, stroke_width=4).scale(0.7)
            lab = caption('dead end', BAD, 26).next_to(board, RIGHT, buff=0.3).set_y(dead.get_y())
            self.play(FadeIn(dead), Create(x), FadeIn(lab), run_time=d * 0.2)
            self.blk = VGroup(u, dead, x, lab)

        self.beat(para(6, 1), blocking)

        def joining(d):
            tag2 = caption('2.  Joining snakes', GOLD, 30).to_corner(DL, buff=0.4)
            self.play(FadeOut(self.blk), FadeTransform(self.tag6, tag2), run_time=0.6)
            self.tag6 = tag2
            self.play_moves(board, G[4:8], 4, per_move=max(MIN_MOVE, d * 0.05), counters=(red, blue), placed=placed)
            self.until('second snake started far away')
            snakes = VGroup(*[ring(c, WHITE, 3) for c in components(placed[0])])
            self.play(Create(snakes), run_time=0.8)
            self.until('a single two-piece joins')
            self.play(FadeOut(snakes), run_time=0.3)
            self.play_moves(board, G[8:9], 8, per_move=1.0, counters=(red, blue), placed=placed)
            j = outline_on(board, move_cells(G[8]))
            glow = ring(max(components(placed[0]), key=len), GOLD)
            self.play(Create(j), Create(glow), Flash(red[1], color=GOLD), run_time=1.0)
            self.wait(0.6)
            self.play(FadeOut(j), FadeOut(glow), run_time=0.5)

        self.beat(para(6, 2), joining)

        def dead_cells(player):
            mine, other = placed[player], placed[1 - player]
            body = {c for c in mine if sum(1 for n in neighbours(c) if n in mine) >= 2}
            return {n for c in body for n in neighbours(c) if n not in mine and n not in other}

        tints = {}

        def update_tints():
            """Each empty cell a player can never use again, tinted in the colour of the player who still can
            (purple where neither can); newly dead cells fade in, cells that got occupied fade out."""
            want = {(c, player) for player in (0, 1) for c in dead_cells(player)}
            new = [k for k in want if k not in tints]
            gone = [k for k in tints if k not in want]
            for c, player in new:
                tints[(c, player)] = board.cells[c].copy().set_fill(P_BLUE if player == 0 else P_RED, 0.3).set_stroke(width=0)
            return [FadeIn(tints[k]) for k in new] + [FadeOut(tints.pop(k)) for k in gone]

        def flowing(d):
            until = self.until

            def note(*lines, color=INK):
                return VGroup(*[caption(t, color, 26) for t in lines]).arrange(DOWN, aligned_edge=LEFT, buff=0.12)

            tag3 = caption('3.  Flowing around', GOLD, 30).to_corner(DL, buff=0.4)
            self.play(FadeTransform(self.tag6, tag3), run_time=0.6)
            self.tag6 = tag3
            # empty cells next to red's body cells: red can never use them, blue can, at any time
            body = {c for c in placed[0] if sum(1 for n in neighbours(c) if n in placed[0]) >= 2}
            reserved = sorted({n for c in body for n in neighbours(c) if n not in placed[0] and n not in placed[1]})
            spots = VGroup(*[board.cells[c].copy().set_fill(P_BLUE, 0.4).set_stroke(width=0) for c in reserved])
            until('The cells right next')
            self.play(LaggedStart(*[FadeIn(t) for t in spots], lag_ratio=0.08), run_time=1.6)
            ghost = piece_on(board, [21], P_RED).set_opacity(0.5)
            no = Cross(ghost, stroke_color=BAD, stroke_width=5).scale(0.6)
            lost = note('red can never', 'use these cells', color=MUTED).to_edge(LEFT, buff=0.5).shift(UP * 1.4)
            until('a piece there')
            self.click(0.3)
            self.play(FadeIn(ghost, scale=1.2), run_time=0.6)
            self.play(Create(no), FadeIn(lost), run_time=0.8)
            self.play(FadeOut(ghost), FadeOut(no), run_time=0.4)
            mine = note('reserved', 'for blue', color=P_BLUE).next_to(lost, DOWN, buff=0.5, aligned_edge=LEFT)
            until('for the opponent')
            self.play(FadeIn(mine), spots.animate.set_fill(P_BLUE, 0.6), run_time=0.8)
            later = note('no hurry:', 'use them any time', color=INK).next_to(mine, DOWN, buff=0.5, aligned_edge=LEFT)
            until('whenever needed')
            self.play(FadeIn(later), run_time=0.6)
            # the end of this very game: blue's last piece lands on two of these cells, up in the corner
            until('In this game')
            key = outline_on(board, [10, 21], GOLD, 5)
            self.play(Create(key), run_time=0.6)
            b2, pcs2 = board_with(G, 14, 0.14)
            inset = VGroup(b2, pcs2)
            frame = SurroundingRectangle(inset, color=MUTED, buff=0.15, corner_radius=0.1, stroke_width=2)
            label = note('move 14:', "blue's last piece", color=GOLD)
            score = caption('21 : 18', GOLD, 34, BOLD)
            VGroup(label, VGroup(inset, frame), score).arrange(DOWN, buff=0.3).move_to(RIGHT * 5.3 + DOWN * 0.3)
            last = outline_on(b2, move_cells(G[13]), GOLD, 4)
            self.play(FadeIn(inset), Create(frame), FadeIn(label), run_time=0.7)
            self.click(0.3)
            self.play(Create(last), FadeIn(score, shift=UP * 0.2), run_time=0.8)
            # the rest of the game: dead cells spread on both sides until the free board is split into pockets
            until('As the board fills')
            self.play(FadeOut(VGroup(spots, lost, mine, later, key, inset, frame, last, label, score)), *update_tints(), run_time=1.0)
            for k in range(9, 14):
                self.play_moves(board, G[k:k + 1], k, per_move=MIN_MOVE, counters=(red, blue), placed=placed)
                anims = update_tints()
                if anims:
                    self.play(*anims, run_time=0.5)

        self.beat(para(6, 3), flowing, pad=1.2)
        self.clear_all()


# ================================================================================================
# 7. the question that would not go away
# ================================================================================================
def solved_icon(kind):
    if kind == 'ttt':
        g = VGroup(*[Line(UP * 0.75, DOWN * 0.75, color=INK).shift(RIGHT * x) for x in (-0.25, 0.25)],
                   *[Line(LEFT * 0.75, RIGHT * 0.75, color=INK).shift(UP * y) for y in (-0.25, 0.25)])
        g.add(Text('X', font=FONT, font_size=26, color=P_RED).move_to(LEFT * 0.5 + UP * 0.5),
              Circle(0.15, color=P_BLUE).move_to(ORIGIN), Text('X', font=FONT, font_size=26, color=P_RED).move_to(RIGHT * 0.5 + DOWN * 0.5))
        return g
    if kind == 'c4':
        cols = [[P_RED, None, None, None], [P_BLUE, P_RED, None, None], [P_RED, P_BLUE, P_RED, None], [P_BLUE, P_BLUE, P_BLUE, P_RED]]
        g = VGroup(RoundedRectangle(width=1.6, height=1.4, corner_radius=0.1, stroke_color=FAINT, stroke_width=2).set_fill(EMPTY, 1))
        for x in range(4):
            for y in range(4):
                c = cols[x][y]
                g.add(Circle(0.14, stroke_width=1.5, color=c or FAINT).set_fill(c or BG, 1).move_to(np.array([-0.57 + 0.38 * x, -0.5 + 0.33 * y, 0])))
        return g
    if kind == 'checkers':
        g = VGroup()
        for x in range(4):
            for y in range(4):
                g.add(Square(0.37, stroke_width=0).set_fill(FAINT if (x + y) % 2 else EMPTY, 1).move_to(np.array([-0.55 + 0.37 * x, -0.55 + 0.37 * y, 0])))
        g.add(Circle(0.13, stroke_width=0).set_fill(P_RED, 1).move_to(np.array([-0.55, -0.55, 0])),
              Circle(0.13, stroke_width=0).set_fill(P_BLUE, 1).move_to(np.array([0.56, 0.56, 0])))
        return g
    if kind == 'othello':
        g = VGroup(Square(1.5, stroke_color=FAINT, stroke_width=2).set_fill(EMPTY, 1))
        g.add(*[Line(np.array([-0.75, y, 0]), np.array([0.75, y, 0]), color=FAINT, stroke_width=1.5) for y in (-0.375, 0, 0.375)],
              *[Line(np.array([x, -0.75, 0]), np.array([x, 0.75, 0]), color=FAINT, stroke_width=1.5) for x in (-0.375, 0, 0.375)])
        for (x, y, c) in ((0, 0, P_BLUE), (1, 1, P_BLUE), (0, 1, P_RED), (1, 0, P_RED), (2, 1, P_RED)):
            g.add(Circle(0.14, stroke_width=0).set_fill(c, 1).move_to(np.array([-0.5625 + 0.375 * x, -0.1875 + 0.375 * y, 0])))
        return g
    return VGroup(hex_shape(ORIGIN, 0.7, fill=EMPTY, stroke=GOLD, sw=3, scale=1.0), Text('?', font=FONT, font_size=60, color=GOLD, weight=BOLD))


def solved_row():
    """Games that have been solved, each with its year and result, and Snekagon at the end with a question mark."""
    games = [('ttt', 'tic-tac-toe', 'by hand', 'draw'), ('c4', 'Connect Four', '1988', 'first player wins'),
             ('checkers', 'checkers', '2007', 'draw'), ('othello', 'Othello', '2023', 'draw'), ('snek', 'Snekagon', '—', '?')]
    cards = VGroup()
    for kind, name, year, result in games:
        icon = solved_icon(kind)
        icon.scale_to_fit_height(1.5)
        txt = VGroup(caption(name, INK, 26, BOLD), caption(year or ' ', MUTED, 22), caption(result, GOLD if result == '?' else MUTED, 22))
        txt.arrange(DOWN, buff=0.12)
        cards.add(VGroup(icon, txt).arrange(DOWN, buff=0.3))
    return cards.arrange(RIGHT, buff=0.55).shift(UP * 0.2)


class Ch07Question(Narrated):
    def construct(self):
        chapter_card(self, 3, 'Solving the game', art_tree())  # where I tried to solve it myself

        def years(d):
            # spoken to the camera; the timeline of Adam's CGE games is laid over it (Ch07YearsOverlay, overlays.py)
            if camera_clip(para(7, 0)):
                return  # Adam's footage covers the whole frame (camera.py)
            ph = placeholder('Adam on camera, with the timeline of his CGE games over it', 8, 3.4).shift(UP * 1.5)
            self.play(FadeIn(ph), run_time=0.8)

        self.beat(para(7, 0), years)
        self.clear_all(0.6)

        def attempts(d):
            # a small board that looks easy, a lookahead tree that soon gets far too wide, talks with strong
            # players: none of it cracked the game
            b = Board(s=0.2).move_to(np.array([-4.6, 0.3, 0]))
            hex1 = piece_on(b, [60], P_RED, 0.2)
            facts = caption('7 turns · 91 cells', MUTED, 26).next_to(b, DOWN, buff=0.3)
            self.play(FadeIn(b), FadeIn(hex1), FadeIn(facts), run_time=0.8)
            root = np.array([2.2, 2.7, 0])
            lines, parents, width = VGroup(), [root], 8.0
            for depth, fan in enumerate([5, 4, 4, 3]):
                kids, n = [], len(parents) * fan
                y = root[1] - 1.05 * (depth + 1)
                for j, par in enumerate(parents):
                    for k in range(fan):
                        child = np.array([root[0] - width / 2 + width * (j * fan + k + 0.5) / n, y, 0])
                        lines.add(Line(par, child, stroke_width=max(0.4, 2.2 - 0.5 * depth), color=INK,
                                       stroke_opacity=max(0.15, 0.8 - 0.2 * depth)))
                        kids.append(child)
                parents = kids
            tag = caption('minimax search', INK, 28).next_to(root, UP, buff=0.15)
            self.until('I tried some basic minimax')
            self.play(FadeIn(tag), LaggedStart(*[Create(l) for l in lines], lag_ratio=0.004), run_time=2.6)
            self.until('just not enough')
            cross = caption('✗', BAD, 110, BOLD).move_to(np.array([root[0], 0.7, 0]))
            self.play(lines.animate.set_stroke(opacity=0.08), tag.animate.set_color(MUTED), FadeIn(cross, scale=1.4), run_time=0.7)
            self.until('long discussions with strong players')
            talk = VGroup()
            for x in (0.9, 3.5):
                head = Circle(0.32, color=MUTED, stroke_width=3).move_to(np.array([x, -2.9, 0]))
                bubble = RoundedRectangle(width=1.3, height=0.8, corner_radius=0.2, color=MUTED, stroke_width=2).next_to(head, UP, buff=0.15)
                talk.add(VGroup(head, bubble, caption('?', GOLD, 36, BOLD).move_to(bubble)))
            self.play(LaggedStart(*[FadeIn(t, shift=UP * 0.2) for t in talk], lag_ratio=0.4), run_time=1.0)
            self.until('quite a problem to solve')
            q = Text('?', font=FONT, font_size=96, color=GOLD, weight=BOLD).move_to(b)
            self.play(FadeIn(q, scale=1.6), b.animate.set_opacity(0.35), run_time=0.8)
            self.att = (b, hex1, q)

        self.beat(para(7, 1), attempts)
        keep = self.att
        self.play(*[FadeOut(m) for m in self.mobjects if all(m is not k for k in keep)], run_time=0.6)
        self.remove(*keep)
        self.att = VGroup(*keep)
        self.add(self.att)

        def llms(d):
            # a timeline: the game, then the language models that could not solve it, then Opus 5.5
            stops = [('2020', 'Snekagon', GOOD), ('Nov 2022', 'ChatGPT', INK), ('Mar 2023', 'Claude', INK), ('Dec 2023', 'Gemini', INK),
                     ('Mar 2024', 'Claude 3', INK), ('May 2025', 'Claude 4', INK), ('2026', 'Claude Opus 5.5', GOLD)]
            y = 1.7
            axis = Line(LEFT * 6.6, RIGHT * 6.6, color=FAINT, stroke_width=3).set_y(y)
            dots, labels = VGroup(), VGroup()
            for k, (x, (when, name, col)) in enumerate(zip(np.linspace(-5.9, 5.5, len(stops)), stops)):
                dot = Dot(np.array([x, y, 0]), radius=0.11, color=col if col != INK else MUTED)
                lab = VGroup(caption(when, MUTED, 20), caption(name, col, 26, BOLD)).arrange(DOWN, buff=0.08)
                lab.next_to(dot, UP if k % 2 == 0 else DOWN, buff=0.25)
                dots.add(dot)
                labels.add(lab)
            self.play(Create(axis), ReplacementTransform(self.att, dots[0]), FadeIn(labels[0], shift=UP * 0.15), run_time=1.0)
            self.until('large language models')
            self.play(LaggedStart(*[AnimationGroup(FadeIn(dt, scale=1.5), FadeIn(lb, shift=UP * 0.15))
                                    for dt, lb in zip(dots[1:-1], labels[1:-1])], lag_ratio=0.5), run_time=2.5)
            self.until('none of them could')
            crosses = VGroup(*[caption('✗', BAD, 40, BOLD).move_to(dt) for dt in dots[1:-1]])
            self.play(LaggedStart(*[ReplacementTransform(dt, cr) for dt, cr in zip(dots[1:-1], crosses)], lag_ratio=0.25),
                      *[lb[1].animate.set_color(MUTED) for lb in labels[1:-1]], run_time=1.6)
            self.until('Then Opus 5.5 came out')
            ring = Circle(radius=0.3, color=GOLD, stroke_width=4).move_to(dots[-1])
            self.play(FadeIn(dots[-1], scale=1.5), FadeIn(labels[-1], shift=UP * 0.15), Create(ring), run_time=1.0)
            self.until('Not by asking for the answer')
            term = RoundedRectangle(width=7.6, height=3.0, corner_radius=0.12, stroke_color=FAINT, stroke_width=2).set_fill('#0D1014', 1)
            term.move_to(DOWN * 1.9)
            out = ['$ snek perft 3', 'perft 1 = 91', 'perft 2 = 194388', 'perft 3 = 315706848', '$ snek verify 2000',
                   'ok: 31391 positions movegen identical']
            lines_ = VGroup(*[Text(t, font='Consolas', font_size=20, color=GOOD if t.startswith('$') else INK) for t in out])
            lines_.arrange(DOWN, aligned_edge=LEFT, buff=0.12).move_to(term).align_to(term, LEFT).shift(RIGHT * 0.35)
            self.play(FadeIn(term), run_time=0.5)
            for l in lines_:
                self.type_text(l, 0.55)

        self.beat(para(7, 2), llms, pad=1.0)
        self.clear_all()


# ================================================================================================
# overlays: rendered with a transparent background (overlays.py) and laid over Adam's camera footage
# ================================================================================================
CGE_GAMES = [(2021, 'arnak_leaders'), (2022, 'devil'), (2022, 'starship'), (2023, 'arnak_missing'),
             (2023, 'kutna_hora'), (2024, 'seti'), (2025, 'arnak_twisted'), (2025, 'seti_agencies'), (2026, 'arnak_shipment')]


class Ch07YearsOverlay(Narrated):
    """Over "Throughout the years, I focused on other projects at CGE...": a band along the bottom with the years
    and the boxes of the CGE games Adam worked on (box images from czechgames.com; not the Arnak base game, which he
    did not work on), appearing as he names them. Snekagon sits behind the band all along, above its year 2020, faintly visible through it, and rises from behind it at "But this game
    stayed in the back of my mind". Lasts exactly as long as the beat (Adam's recording once there is one)."""

    def construct(self):
        text = para(7, 0)
        dur = narrate(text)[1]
        self._beat = (text, 0.0, dur)  # self.until works as in a narrated beat, without adding the sound
        # along the top, above Adam's head, so it does not cover him
        x0, step, axis_y = -5.4, 1.8, 2.5
        band = Rectangle(width=config.frame_width + 0.2, height=2.2, stroke_width=0).set_fill(BG, 0.8)
        band.move_to(np.array([0, config.frame_height / 2 - 1.1, 0]))
        axis = Line(np.array([x0 - 0.9, axis_y, 0]), np.array([x0 + 6 * step + 0.9, axis_y, 0]), color=MUTED, stroke_width=3)
        years = VGroup(*[VGroup(Line(np.array([x0 + k * step, axis_y - 0.08, 0]), np.array([x0 + k * step, axis_y + 0.08, 0]), color=MUTED, stroke_width=3),
                                caption(str(2020 + k), MUTED, 22).move_to(np.array([x0 + k * step, axis_y - 0.32, 0]))) for k in range(7)])
        boxes = {}
        seen = {}
        for year, name in CGE_GAMES:
            img = ImageMobject(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'cge', name + '.png')).set_height(1.0)
            k = seen.get(year, 0)
            seen[year] = k + 1
            # a second game of the same year stands a little to the right, in front, like a small pile
            img.move_to(np.array([x0 + (year - 2020) * step + 0.42 * k - (0.21 if year in (2022, 2023, 2025) else 0), axis_y + 0.15, 0]), aligned_edge=DOWN)
            img.set_z_index(2 + k)
            boxes[name] = img
        snek = photo('published.jpg', 1.4, tilt=-4).move_to(np.array([-5.1, 2.95, 0]))  # Snekagon's own year, 2020
        snek.set_z_index(-1).set_opacity(0.4)  # behind the band: there all along, only just visible
        band.set_z_index(0)
        for m in (axis, years):
            m.set_z_index(1)
        self.add(snek)
        self.play(FadeIn(band), FadeIn(axis), FadeIn(years), run_time=0.6)

        def show(names):
            self.play(LaggedStart(*[FadeIn(boxes[n], shift=UP * 0.25, scale=0.9) for n in names], lag_ratio=0.35), run_time=0.4 + 0.25 * len(names))

        self.until('joining the Arnak team')
        show(['arnak_leaders', 'arnak_missing', 'arnak_twisted', 'arnak_shipment'])
        self.until('working on SETI')
        show(['seti', 'seti_agencies'])
        self.until('and more')
        show(['devil', 'starship', 'kutna_hora'])
        self.until('But this game stayed')
        # it stays behind the band while it comes down, so it really comes out from behind the timeline; it ends on the left
        self.play(*[boxes[n].animate.set_opacity(0.35) for n in boxes],
                  snek.animate.set_opacity(1).scale(2.0).move_to(np.array([-4.7, -0.9, 0])), run_time=1.0)
        left = dur - self.renderer.time - 0.5
        if left > 0:
            self.wait(left)
        self.play(FadeOut(Group(band, axis, years, snek, *boxes.values())), run_time=0.5)


CHAPTERS = ['Ch01ColdOpen', 'Ch02WhoIAm', 'Ch03HowItStarted', 'Ch04OutIntoTheWorld', 'Ch05Rules', 'Ch06HowItIsPlayed', 'Ch07Question']

"""Shared visuals for the Snekagon video: the board, pieces in the style of the printed rules,
the example games, chapter cards, placeholders and narration beats."""
import math
import json
import os
import re
import sys

import glob

import manimpango
import numpy as np
from manim import *
from manim.mobject.text import text_mobject as _text_mobject

if os.environ.get('SNEK_WATCHDOG'):  # debugging hangs: dump all stacks to this file every minute
    import faulthandler
    _watchdog_file = open(os.environ['SNEK_WATCHDOG'], 'w')
    faulthandler.dump_traceback_later(60, repeat=True, file=_watchdog_file)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tts import narrate, recording_key  # noqa: E402
from sfx import path as sfx_path  # noqa: E402

# ---- look ------------------------------------------------------------------------------------
BG = '#14171C'
INK = '#E8ECF1'
MUTED = '#8C97A5'
FAINT = '#3A414C'
EMPTY = '#232831'
P_RED = '#E5574B'
P_BLUE = '#4A8FE7'
MARK = {P_RED: '#9E2F27', P_BLUE: '#235A9E'}
SEAM = '#0B0D10'
GOOD = '#5BC47A'
BAD = '#FF6B5E'
GOLD = '#F2C14E'
PAPER = '#EDE6D6'
PAPER_INK = '#2B2620'
# Latin Modern Roman: the Computer Modern look of TeX (and of 3Blue1Brown). Install the four files in
# fonts/ (right click, Install): registered only for this process instead, every new text takes seconds.
# The few characters it lacks come from fallback fonts.
FONT = 'Latin Modern Roman 10'
if FONT not in manimpango.list_fonts():
    print('Latin Modern is not installed: falling back to the slow per-process font (see fonts/)', file=sys.stderr)
    for _font_file in glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fonts', '*.otf')):
        manimpango.register_font(_font_file)
FALLBACK = {'≈': 'Cambria', '²': 'Cambria', '⁹': 'Cambria', '✓': 'Segoe UI Symbol', '✗': 'Segoe UI Symbol'}
MIN_MOVE = 0.9  # seconds per placed piece: slow enough to follow every move
config.background_color = BG
# build.py renders chapters in parallel: each process keeps its own text cache (see build.py)
if os.environ.get('SNEK_TEXT_SUBDIR'):
    config.text_dir = os.path.join(config.get_dir('text_dir'), os.environ['SNEK_TEXT_SUBDIR'])

class Text(_text_mobject.Text):
    def __init__(self, text, *args, **kwargs):
        if kwargs.get('font', FONT) == FONT:
            t2f = dict(kwargs.get('t2f') or {})
            for ch, font in FALLBACK.items():
                if ch in text:
                    t2f.setdefault(ch, font)
            if t2f:
                kwargs['t2f'] = t2f
        try:
            super().__init__(text, *args, **kwargs)
        except Exception:
            # a cached SVG left empty by an interrupted run: drop the empty files and render again
            for f in glob.glob(os.path.join(config.get_dir('text_dir'), '*.svg')):
                if os.path.getsize(f) == 0:
                    os.remove(f)
            super().__init__(text, *args, **kwargs)


_text_mobject.Text = Text

# An exception inside a scene leaves Manim's file writer thread waiting for frames forever, so the process
# never exits. Print the error and exit at once instead.
_scene_render = Scene.render


def _render_or_exit(self, *args, **kwargs):
    try:
        return _scene_render(self, *args, **kwargs)
    except BaseException:
        import traceback
        traceback.print_exc()
        sys.stderr.flush()
        os._exit(1)


Scene.render = _render_or_exit


# ---- geometry (same layout as the engine: index = (r+5)*11 + (q+5), pointy-top hexes) ----------
SQ3 = math.sqrt(3)
DIRS = [(1, 0), (1, -1), (0, -1), (-1, 0), (-1, 1), (0, 1)]  # direction d points at screen angle 60*d


def qr(i):
    return i % 11 - 5, i // 11 - 5


def idx(q, r):
    return (r + 5) * 11 + q + 5


def on_board(q, r):
    return abs(q) <= 5 and abs(r) <= 5 and abs(q + r) <= 5


CELLS = [i for i in range(121) if on_board(*qr(i))]


def hex_xy(q, r, s):
    return np.array([s * SQ3 * (q + r / 2), -s * 1.5 * r, 0.0])


def neighbours(i):
    q, r = qr(i)
    return [idx(q + a, r + b) for a, b in DIRS if on_board(q + a, r + b)]


def components(cells):
    cells, out = set(cells), []
    while cells:
        stack = [cells.pop()]
        comp = set(stack)
        while stack:
            x = stack.pop()
            for n in neighbours(x):
                if n in cells:
                    cells.discard(n)
                    comp.add(n)
                    stack.append(n)
        out.append(comp)
    return out


def longest_snake(cells):
    return max((len(c) for c in components(cells)), default=0)


def snake_of(cells, start):
    return next(c for c in components(cells) if start in c)


def hex_shape(center, s, fill=EMPTY, stroke=FAINT, sw=1.2, fill_opacity=1.0, scale=0.93):
    h = RegularPolygon(6, start_angle=PI / 6, radius=s * scale)
    h.move_to(center).set_fill(fill, fill_opacity).set_stroke(stroke, sw)
    return h


def _corner(center, s, k):
    a = math.radians(30 + 60 * k)
    return center + s * np.array([math.cos(a), math.sin(a), 0.0])


def _unit(deg):
    a = math.radians(deg)
    return np.array([math.cos(a), math.sin(a), 0.0])


class Board(VGroup):
    """The 91-cell board; cells[i] is the hexagon of engine cell index i."""

    def __init__(self, s=0.34, **kw):
        super().__init__(**kw)
        self.s = s
        self.cells = {}
        for i in CELLS:
            h = hex_shape(hex_xy(*qr(i), s), s)
            self.cells[i] = h
            self.add(h)

    def at(self, i):
        return self.cells[i].get_center()

    @property
    def scale_s(self):
        # current hex radius (the board may have been scaled)
        return self.cells[60].width / (SQ3 * 0.93)

    def ring(self, k):
        return [self.cells[i] for i in CELLS if max(abs(qr(i)[0]), abs(qr(i)[1]), abs(sum(qr(i)))) == k]


# ---- pieces, drawn like the printed ones ----------------------------------------------------------
# A piece is one solid band. On every wall where the snake may NOT continue (it would turn too sharply
# or touch itself) the printed pieces carry a mark: zigzags on red pieces, slots on the other colour.

def _forbidden_walls(inside):
    if len(inside) == 1:
        return [(inside[0] + 1) % 6, (inside[0] - 1) % 6]
    if len(inside) == 2:
        return [d for d in range(6) if d not in inside]
    return []


def _mark(c, s, d, color):
    u, t = _unit(60 * d), _unit(60 * d + 90)
    base = c + 0.6 * s * u
    if color == P_RED:
        pts = [base + t * (x * s) + u * (0.07 * s if k % 2 else -0.07 * s) for k, x in enumerate((-0.24, -0.12, 0, 0.12, 0.24))]
        return VMobject(stroke_color=MARK[color], stroke_width=2.4).set_points_as_corners(pts)
    slot = RoundedRectangle(width=0.42 * s, height=0.11 * s, corner_radius=0.05 * s, stroke_width=0)
    slot.set_fill(MARK.get(color, SEAM), 1).rotate(math.radians(60 * d + 90)).move_to(base)
    return slot


def piece_art(cells_qr, centers, s, color, seam_width=2.2):
    """The look of a placed piece: cells_qr give its shape, centers where its cells are drawn."""
    shape = set(cells_qr)
    fills, marks, edges = VGroup(), VGroup(), VGroup()
    for (q, r), c in zip(cells_qr, centers):
        fills.add(RegularPolygon(6, start_angle=PI / 6, radius=s * 1.004).move_to(c).set_fill(color, 1).set_stroke(color, 0.5))
        inside = [d for d, (a, b) in enumerate(DIRS) if (q + a, r + b) in shape]
        for d in range(6):
            if d not in inside:
                edges.add(Line(_corner(c, s, d - 1), _corner(c, s, d), stroke_width=seam_width, color=SEAM))
        for d in _forbidden_walls(inside):
            marks.add(_mark(c, s, d, color))
    return VGroup(fills, marks, edges)


def piece_outline(cells_qr, centers, s, color=WHITE, width=5):
    """The white outline the rules use for the piece just placed."""
    shape = set(cells_qr)
    out = VGroup()
    for (q, r), c in zip(cells_qr, centers):
        for d, (a, b) in enumerate(DIRS):
            if (q + a, r + b) not in shape:
                out.add(Line(_corner(c, s, d - 1), _corner(c, s, d), stroke_width=width, color=color))
    return out


def piece_on(board, cells, color, s=None):
    s = s or board.scale_s
    return piece_art([qr(c) for c in cells], [board.at(c) for c in cells], s, color)


def outline_on(board, cells, color=WHITE, width=5):
    return piece_outline([qr(c) for c in cells], [board.at(c) for c in cells], board.scale_s, color, width)


PIECES = {
    '1': [(0, 0)],
    '2': [(0, 0), (1, 0)],
    '3I': [(0, 0), (0, 1), (0, 2)],
    '3V': [(0, 0), (1, 0), (1, 1)],
    '4I': [(0, 0), (-1, 1), (-2, 2), (-3, 3)],
    '4A': [(0, 0), (1, 0), (1, 1), (1, 2)],
    '4B': [(0, 0), (0, 1), (1, 1), (1, 2)],
    '4C': [(0, 0), (-1, 1), (-1, 2), (0, 2)],
}


def loose_piece(name, color, s=0.3):
    pts = [hex_xy(q, r, s) for q, r in PIECES[name]]
    centre = sum(pts) / len(pts)
    return piece_art(PIECES[name], [p - centre for p in pts], s, color)


def shape_on_patch(cells_qr, s, color, origin=ORIGIN):
    return piece_art(cells_qr, [origin + hex_xy(q, r, s) for q, r in cells_qr], s, color)


def patch(radius, s, origin=ORIGIN):
    g = VGroup()
    for q in range(-radius, radius + 1):
        for r in range(-radius, radius + 1):
            if abs(q + r) <= radius:
                g.add(hex_shape(origin + hex_xy(q, r, s), s))
    return g


def move_cells(m):
    return [] if m == 'pass' else [int(x) for x in m.split(':')[1].split(',')]


def position_after(moves, k):
    own = [set(), set()]
    for j, m in enumerate(moves[:k]):
        own[j % 2] |= set(move_cells(m))
    return own


def board_with(moves, k, s):
    """A board showing the position after k moves; returns (board, group of pieces)."""
    b = Board(s=s)
    pieces = VGroup(*[piece_on(b, move_cells(m), P_RED if j % 2 == 0 else P_BLUE) for j, m in enumerate(moves[:k]) if m != 'pass'])
    return b, pieces


def position_board(moves, s, marks=()):
    """A small board after `moves`; each (move, colour) in marks gets an outline."""
    b, pcs = board_with(moves, len(moves), s)
    outlines = VGroup(*[outline_on(b, move_cells(m), c, max(2.5, s * 16)) for m, c in marks])
    return VGroup(b, pcs, outlines)


def zoom_card(view, lines=(), color=GOLD, size=20):
    """A framed view with a few lines of text under it, covering whatever lies behind."""
    if isinstance(lines, str):
        lines = [lines]
    content = VGroup(view)
    if lines:
        text = VGroup(*[caption(t, color if k == 0 else MUTED, size) for k, t in enumerate(lines)]).arrange(DOWN, buff=0.08)
        content.add(text.next_to(view, DOWN, buff=0.12))
    frame = SurroundingRectangle(content, buff=0.14, corner_radius=0.08, color=color, stroke_width=1.5).set_fill(BG, 0.94)
    return VGroup(frame, content)


def edge_point(mob, toward):
    """Where the line from the centre of mob towards a point leaves its bounding box."""
    c = mob.get_center()
    d = np.array(toward) - c
    ts = [mob.width / 2 / abs(d[0]) if abs(d[0]) > 1e-9 else np.inf, mob.height / 2 / abs(d[1]) if abs(d[1]) > 1e-9 else np.inf]
    return c + d * min(ts)


# ---- the two example games ------------------------------------------------------------------------
# Story game: U block, then perfect play to a 17 : 17 draw (chapters 1, 6, 11).
DRAW_GAME = ['1:60', '4B:16,27,28,39', '3V:40,41,50', '4C:49,59,70,71', '4I:52,62,72,82', '4I:46,57,68,79',
             '4B:80,81,89,90', '4A:85,95,104,105', '3I:66,77,88', '3I:15,25,35', '2:45,56', '3V:102,103,112',
             '4A:8,19,20,21', '2:51,61', 'pass', 'pass']
# (red's last three moves in this order: the snake first, the four in the empty corner last; every move still
# holds the draw, checked with the exact solver)
# One of the sixteen proven replies of the opening line (chapters 1, 9 and 10), played out close: red's move 5
# is the closest defence among the engine's 48 favourites, after that both sides play perfectly for the score
# (margin_line.py, margin_dfs.py: exact solver with a score offset). Blue wins 22 : 18.
WIN_GAME = ['1:60', '4B:16,27,28,39', '4A:18,29,40,50', '4A:6,7,8,19', '4C:56,66,77,78', '4C:20,21,32,42', '4I:89,90,91,92',
            '3V:48,49,58', '4B:73,74,82,83', '3I:35,46,57', '3V:104,113,114', '4I:41,51,61,71', '3I:85,95,105', '2:110,111',
            '2:69,70']
# The toughest of the sixteen proofs (241 hours: red 3V:29,40,50, blue's winning answer 4A:6,7,8,19), played out
# for chapter 10 (hard_close.py): red's move 5 grows its snake, the closest of its 24 favourite growing moves; after
# that both sides play perfectly for the score. Both build a second snake (moves 6 and 7); blue's straight four
# down the middle (move 12) joins blue's two snakes. 21 : 15.
HARD_GAME = ['1:60', '4B:16,27,28,39', '3V:29,40,50', '4A:6,7,8,19', '4C:47,48,57,59', '3I:77,78,79', '4I:42,52,62,72',
             '4C:80,82,91,92', '2:66,67', '2:20,31', '4A:9,10,21,32', '4I:41,51,61,71', '3I:83,93,103', '3V:74,85,95',
             '4B:101,102,110,111']
# In the position of the story game before move 76 (DRAW_GAME[:5]): blue walls off red's road right under its two
# reserved cells (the engine's 8th choice), and the perfect line that refutes it: red takes the two cells and turns
# left around the wall (pv_line.py, data/block_line.log; red wins 16 : 15).
BLOCK_LOSS = DRAW_GAME[:5] + ['4A:89,90,91,102', '4A:58,69,80,81', '4I:6,7,8,9', '4C:55,57,66,67', '2:35,36', '3I:32,43,54',
                              '3V:10,21,31', '4B:64,74,85,95', '3I:110,111,112', '2:104,105']
# Another engine game, only used to show a board filling up (chapter 3).
SKETCH_GAME = ['1:62', '4A:39,50,60,70', '4A:48,58,68,79', '4C:78,80,89,90', '3V:28,29,38', '4I:5,15,25,35', '4C:20,21,30,32',
               '4B:45,56,66,77']
# An ordinary game (engine against engine) for the rules: blue wins 21 : 18, red passes with a piece left.
RULES_GAME = ['1:38', '4A:59,60,61,69', '4A:83,91,92,93', '4C:80,90,101,102', '3V:51,62,72', '4B:104,105,113,114',
              '4B:19,20,28,29', '4I:32,43,54,65', '2:31,41', '3V:67,78,88', '4C:36,37,46,57', '3I:75,85,95',
              '3I:5,6,7', '2:10,21', 'pass', 'pass']


# ---- text blocks ------------------------------------------------------------------------------------
def caption(text, color=MUTED, size=26, weight=NORMAL):
    return Text(text, font=FONT, font_size=size, color=color, weight=weight)


def placeholder(label, width=6.0, height=3.4):
    """Dashed frame standing in for a photo or footage."""
    frame = DashedVMobject(RoundedRectangle(width=width, height=height, corner_radius=0.15, color=MUTED, stroke_width=2), num_dashes=60)
    icon = VGroup(Circle(0.18, color=MUTED, stroke_width=2).shift(UP * 0.25 + LEFT * 0.45),
                  Polygon([-0.9, -0.35, 0], [-0.3, 0.2, 0], [0.1, -0.1, 0], [0.5, 0.35, 0], [0.9, -0.35, 0], color=MUTED, stroke_width=2))
    icon.scale(0.8).move_to(frame.get_center() + UP * 0.35)
    text = Paragraph(*wrap(label, 38), font=FONT, font_size=22, color=MUTED, alignment='center', line_spacing=0.8)
    text.next_to(icon, DOWN, buff=0.3)
    return VGroup(frame, icon, text)


def maty_logo(height):
    """The Maty Moves logo, burnt into wood like their games (from matymoves.com, cropped to a rounded tile)."""
    return ImageMobject(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'logos', 'maty_moves.png')).set_height(height)


def photo(name, height, label=None, tilt=0.0):
    """A photo from assets/ as a print: a paper border, wider at the bottom when it carries a caption;
    tilt in degrees."""
    img = ImageMobject(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', name)).set_height(height)
    pad = height * 0.035
    bottom = height * (0.11 if label else 0.035)
    frame = Rectangle(width=img.width + 2 * pad, height=img.height + pad + bottom, stroke_width=0, fill_color=PAPER, fill_opacity=1)
    frame.move_to(img.get_center() + DOWN * (bottom - pad) / 2)
    print_ = Group(frame, img)
    if label:
        print_.add(caption(label, PAPER_INK, 26).move_to(frame.get_bottom() + UP * bottom / 2))
    return print_.rotate(tilt * DEGREES)


def wrap(text, width):
    words, lines, line = text.split(), [], ''
    for w in words:
        if len(line) + len(w) + 1 > width and line:
            lines.append(line)
            line = w
        else:
            line = (line + ' ' + w).strip()
    lines.append(line)
    return lines


def paper_card(lines, title=None, width=7.5, size=24):
    body = Paragraph(*lines, font=FONT, font_size=size, color=PAPER_INK, line_spacing=0.9)
    parts = [body]
    if title:
        parts.insert(0, Text(title, font=FONT, font_size=20, color='#6B6256'))
    content = VGroup(*parts).arrange(DOWN, aligned_edge=LEFT, buff=0.3)
    card = RoundedRectangle(width=max(width, content.width + 0.8), height=content.height + 0.8, corner_radius=0.12,
                            stroke_width=0).set_fill(PAPER, 1)
    content.move_to(card)
    return VGroup(card, content)


def quote_card(markup, width=10.5, size=26):
    body = MarkupText(markup, font=FONT, font_size=size, color=INK, line_spacing=1.2)
    if body.width > width - 1.0:
        body.scale_to_fit_width(width - 1.0)
    bar = Rectangle(width=0.08, height=body.height + 0.4, stroke_width=0).set_fill(GOLD, 1)
    g = VGroup(bar, body).arrange(RIGHT, buff=0.35, aligned_edge=UP)
    return g


# ---- chapter cards ----------------------------------------------------------------------------------
def art_question():
    """A question mark made of hexes on a real hex grid (pointy-top, like the board): a gold snake for the hook
    and the stem, one hex for the dot."""
    size = 0.3

    def cell(q, r):
        return np.array([math.sqrt(3) * size * (q + r / 2), -1.5 * size * r, 0])

    # (q, r) in axial coordinates, row r going down: the arc on top, the right side bulging out, the stem down to the middle
    path = [(0, 1), (1, 0), (2, 0), (3, 0), (3, 1), (3, 2), (2, 3), (1, 4), (0, 5), (-1, 6)]
    hook = VGroup(*[hex_shape(cell(q, r), size, fill=GOLD, stroke=BG, sw=2) for q, r in path])
    dot = hex_shape(cell(-2, 8), size, fill=GOLD, stroke=BG, sw=2)
    return VGroup(hook, dot).move_to(ORIGIN)


def art_tree():
    lines = VGroup()
    root = UP * 1.4
    kids = [root + np.array([x, -1.2, 0]) for x in (-1.8, 0, 1.8)]
    for k in kids:
        lines.add(Line(root, k, color=INK, stroke_width=3))
        for x in (-0.5, 0, 0.5):
            lines.add(Line(k, k + np.array([x, -1.1, 0]), color=MUTED, stroke_width=2))
    return VGroup(lines, Dot(root, color=GOLD, radius=0.12))


def camera_clip(text):
    """Adam's footage for this narrated text (camera.py), if there is any: the scene leaves the frame empty there."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'assets', 'camera', recording_key(text.strip()) + '.mp4')
    return path if os.path.exists(path) else None


def on_camera_placeholder(text, label, w, h):
    """The placeholder for a passage spoken to the camera, or nothing once the footage exists."""
    return VGroup() if camera_clip(text) else placeholder(label, w, h)


def chapter_card(scene, number, title, art, hold=1.0):
    num = caption('Chapter %d' % number, MUTED, 30)
    name = caption(title, INK, 58, BOLD)
    if name.width > 12:
        name.scale_to_fit_width(12)
    art.scale_to_fit_height(min(art.height, 3.2)).move_to(UP * 1.3)
    texts = VGroup(num, name).arrange(DOWN, buff=0.25).next_to(art, DOWN, buff=0.6)
    line = Line(LEFT * 1.6, RIGHT * 1.6, color=GOLD, stroke_width=3).next_to(texts, DOWN, buff=0.35)
    # no chime: it clashed with the background music
    scene.play(LaggedStart(*[FadeIn(m, scale=0.9) for m in art], lag_ratio=0.08), run_time=1.2)
    scene.play(FadeIn(num, shift=UP * 0.2), FadeIn(name, shift=UP * 0.2), Create(line), run_time=0.9)
    scene.wait(hold)
    scene.play(FadeOut(VGroup(art, texts, line)), run_time=0.8)


# ---- score counters -------------------------------------------------------------------------------
_FULL_WIDTH = []


def counter_spot(counter):
    """Where a counter's number sits: in the empty corner beside the top row of its board (red left, blue right),
    sized with the board."""
    board = counter.board
    if not _FULL_WIDTH:
        _FULL_WIDTH.append(Board(s=0.34).width)
    f = board.width / _FULL_WIDTH[0]
    spot = board.get_corner(UP + counter.side) - counter.side * 0.55 * f + DOWN * 0.3 * f
    return spot, max(0.2, 0.42 * f)


def stick_counter(counter):
    spot, height = counter_spot(counter)
    counter[1].scale_to_fit_height(height).move_to(spot)
    counter[0].move_to(spot)


# ---- narration ------------------------------------------------------------------------------------
class Narrated(MovingCameraScene):
    """beat(text, action): plays one narration paragraph and runs action(duration) within it."""

    def beat(self, text, action=None, pad=0.35):
        text = re.sub(r'\s*\[ADAM:[^\]]*\]', '', text).strip()
        if camera_clip(text):
            pad = 0.0  # Adam's footage fades out on its own; what comes next starts right away
        path, dur = narrate(text)
        self.add_sound(path)
        t0 = self.renderer.time
        self._beat = (text, t0, dur)
        # when each beat is spoken (data/timeline/<Scene>.json): for camera footage over a beat, and for subtitles
        self.__dict__.setdefault('_timeline', []).append({'key': recording_key(text), 'text': text, 'start': round(t0, 3),
                                                           'duration': round(dur, 3)})
        if action:
            action(dur)
        left = dur + pad - (self.renderer.time - t0)
        if left > 0.02:
            self.wait(left)

    def tear_down(self):
        super().tear_down()
        if self.__dict__.get('_timeline') and not config.dry_run:
            folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'timeline')
            os.makedirs(folder, exist_ok=True)
            with open(os.path.join(folder, type(self).__name__ + '.json'), 'w', encoding='utf-8') as f:
                json.dump({'scene': type(self).__name__, 'beats': self._timeline}, f, ensure_ascii=False, indent=1)

    def until(self, phrase):
        """Waits until the current beat's narration reaches phrase (estimated by its position in the text)."""
        text, t0, dur = self._beat
        left = t0 + dur * text.index(phrase) / len(text) - self.renderer.time
        if left > 0.05:
            self.wait(left)

    def zoom_in(self, square, card, target, run_time=1.0):
        """A grid square opens up into the position it stands for: the square lights up, a thin line runs
        from it to `target`, and the card grows out of the square to its place."""
        card.move_to(target)
        line = Line(square.get_center(), edge_point(card, square.get_center()), color=GOLD, stroke_width=1.5, stroke_opacity=0.8)
        card.save_state()
        card.scale(0.04).move_to(square)
        self.play(square.animate.set_fill(GOLD, 1), run_time=0.3)
        self.play(Restore(card), Create(line), run_time=run_time)
        return VGroup(line, card)

    def search_flicker(self, board, trace, run_time, per_line=0.1, fade=0.6):
        """The lines a solver tries (a trace from search_trace.py: whole continuations of the start position),
        switching fast, about ten a second. The whole layer of changing pieces fades in at the start and out at
        the end; the pieces of the start position stay fixed, outlined in gold.
        Nothing is shown at alpha 0: Cairo bakes whatever an animated group holds when the animation starts
        into the static background, and that line would then stay on screen for the whole animation."""
        start = trace['start']
        n = max(2, int(round(run_time / per_line)))
        cache = {}

        def piece(ply, move):
            if (ply, move) not in cache:
                cache[(ply, move)] = piece_on(board, move_cells(move), P_RED if ply % 2 == 0 else P_BLUE)
            return cache[(ply, move)]

        lines = [[piece(len(start) + j, m) for j, m in enumerate(state) if m != 'pass'] for state in trace['states']]
        width = max(1.5, 15 * board.scale_s)  # the gold outline used everywhere, 4 to 5 on a full-size board
        fixed = [outline_on(board, move_cells(m), GOLD, width) for m in start if m != 'pass']
        shown = VGroup()
        full = 0.9
        fade = min(fade, run_time / 3)

        def update(group, alpha):
            if alpha <= 0 or alpha >= 1:
                group.submobjects = []
                return
            t = alpha * run_time
            layer = smooth(min(1.0, t / fade, (run_time - t) / fade))  # the layer's fade in and out
            cur = lines[min(n - 1, int(alpha * n)) % len(lines)]  # a long run starts over from the first line
            for x in cur:
                x.set_opacity(full * layer)
            for x in fixed:
                x.set_stroke(opacity=layer)
            group.submobjects = cur + fixed

        return UpdateFromAlphaFunc(shown, update, run_time=run_time, rate_func=linear)

    _sfx_count = 0

    def sfx(self, name, variants=1, offset=0.0, gain=-10):
        """A sound effect from sfx/ (variants cycle so repeated sounds are not identical)."""
        suffix = str(Narrated._sfx_count % variants) if variants > 1 else ''
        Narrated._sfx_count += 1
        self.add_sound(sfx_path(name + suffix), time_offset=offset, gain=gain)

    def click(self, offset=0.0):
        self.sfx('click', 4, offset, gain=-9)

    def type_text(self, text, run_time):
        """Types text letter by letter with key taps (at most one tap every 70 ms)."""
        taps = max(1, min(len(text), int(run_time / 0.07)))
        for j in range(taps):
            self.sfx('key', 5, run_time * j / taps, gain=-17)
        self.play(AddTextLetterByLetter(text), run_time=run_time)

    def clear_all(self, run_time=0.8):
        # empty groups (a placeholder left out because Adam's footage covers it) need no fade, and no time
        shown = [m for m in self.mobjects if not (isinstance(m, VGroup) and not m.family_members_with_points())]
        if shown:
            self.play(FadeOut(Group(*shown)), run_time=run_time)
        self.remove(*self.mobjects)

    # counters "red 12   blue 8" in the top corners
    def counters(self, board):
        """Each player's longest snake as a coloured number in the empty top corners of the board (red left,
        blue right). The numbers follow the board wherever it moves and scale with it; counter[1] is the number."""
        out = []
        for color, side in ((P_RED, LEFT), (P_BLUE, RIGHT)):
            c = VGroup(VectorizedPoint(), Text('0', font=FONT, font_size=40, color=color, weight=BOLD))
            c.board, c.side = board, side
            c.add_updater(stick_counter)
            stick_counter(c)
            out.append(c)
        self._counters = getattr(self, '_counters', []) + out
        return out

    def play(self, *animations, **kwargs):
        # counters catch up with their board before every animation: one that fades in does not update meanwhile
        for c in getattr(self, '_counters', []):
            c.update(0)
        super().play(*animations, **kwargs)

    def count_to(self, counter, value, color):
        spot, height = counter_spot(counter)
        return Transform(counter[1], Text(str(value), font=FONT, font_size=40, color=color, weight=BOLD).scale_to_fit_height(height).move_to(spot))

    def set_count(self, counter, value, color):
        spot, height = counter_spot(counter)
        counter[1].become(Text(str(value), font=FONT, font_size=40, color=color, weight=BOLD).scale_to_fit_height(height).move_to(spot))

    def play_moves(self, board, moves, start_ply, per_move, counters=None, placed=None, highlight_last=False, supply=None):
        """Animates moves onto the board; returns (piece mobjects, placed cells per player). With a supply
        (see piece_supply) each piece moves in from it."""
        placed = placed if placed is not None else {0: set(), 1: set()}
        per_move = max(per_move, MIN_MOVE)
        pieces = []
        for k, m in enumerate(moves):
            player = (start_ply + k) % 2
            cells = move_cells(m)
            if not cells:
                self.wait(per_move * 0.5)
                pieces.append(None)
                continue
            color = P_RED if player == 0 else P_BLUE
            placed[player] |= set(cells)
            source = supply.pop(m, None) if supply else None
            if source is not None:  # the very piece from the supply grows into its place
                p, target, k = source
                anims = [p.animate.scale(1 / k).move_to(target)]
            else:
                p = piece_on(board, cells, color)
                anims = [FadeIn(p, scale=1.25)]
            if counters:
                anims.append(self.count_to(counters[player], longest_snake(placed[player]), color))
            self.click(per_move * 0.5)
            self.play(*anims, run_time=per_move)
            pieces.append(p)
        return pieces, placed


def within_frame(mob, margin=0.35):
    """Shifts (and if it is too big, shrinks) mob so all of it lies inside the frame, `margin` from the edges."""
    w, h = config.frame_width / 2 - margin, config.frame_height / 2 - margin
    if mob.width > 2 * w:
        mob.scale_to_fit_width(2 * w)
    if mob.height > 2 * h:
        mob.scale_to_fit_height(2 * h)
    dx = max(0, -w - mob.get_left()[0]) - max(0, mob.get_right()[0] - w)
    dy = max(0, -h - mob.get_bottom()[1]) - max(0, mob.get_top()[1] - h)
    return mob.shift(np.array([dx, dy, 0]))


def piece_supply(board, moves, extra=None, scale=0.6):
    """Both players' pieces beside the board, red on the left, blue on the right. Each piece of `moves` is
    already the piece it will be on the board, only smaller, and turned the way it will be played, so placing
    it is one seamless move of the same object. extra: {player: [piece names]} never played.
    Returns ({move: (mobject, its centre on the board, scale)}, the two groups)."""
    supply, groups = {}, VGroup()
    for player in (0, 1):
        color = P_RED if player == 0 else P_BLUE
        items = []
        for j, m in enumerate(moves):
            if j % 2 == player and m != 'pass':
                piece = piece_on(board, move_cells(m), color)
                supply[m] = (piece, piece.get_center().copy(), scale)
                items.append(piece.scale(scale))
        items += [loose_piece(n, color, s=board.scale_s * scale) for n in (extra or {}).get(player, [])]
        g = VGroup(*items).arrange_in_grid(rows=4, cols=2, buff=(0.35, 0.3))
        g.move_to(np.array([-5.6 if player == 0 else 5.6, -0.3, 0]))
        groups.add(within_frame(g))
    return supply, groups


def apply_symmetry(t, q, r):
    """Symmetry t of the board (0..11), the same numbering as the engine: mirror when t >= 6, then rotate."""
    if t >= 6:
        q, r = r, q
    for _ in range(t % 6):
        q, r = -r, q + r
    return q, r


def territory(moves, k):
    """Who reaches which empty cell first (the hand-made evaluation's 'Voronoi' split), after k moves."""
    own = position_after(moves, k)
    occ = own[0] | own[1]
    empty = set(CELLS) - occ
    usable, starts = {}, {}
    for c in (0, 1):
        deg = {x: sum(n in own[c] for n in neighbours(x)) for x in own[c]}
        bad = {n for x, dg in deg.items() if dg >= 2 for n in neighbours(x)}
        usable[c] = empty - bad
        starts[c] = {n for x, dg in deg.items() if dg <= 1 for n in neighbours(x)} & usable[c]
    a = k % 2
    b = 1 - a
    claim, reached, front = {0: set(), 1: set()}, set(), {0: set(), 1: set()}
    for step in range(60):
        na = (starts[a] if step == 0 else {n for x in front[a] for n in neighbours(x)} & usable[a]) - reached
        nb = set() if step < 1 else ((starts[b] if step == 1 else {n for x in front[b] for n in neighbours(x)} & usable[b]) - reached)
        both = na & nb
        na, nb = na - both, nb - both
        reached |= na | nb | both
        claim[a] |= na
        claim[b] |= nb
        front[a], front[b] = na, nb
        if step > 1 and not na and not nb:
            break
    return claim

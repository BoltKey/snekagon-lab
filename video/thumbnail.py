"""YouTube thumbnails (1280 x 720) in the style of the video.

    .venv/Scripts/manim -s -r 1280,720 --disable_caching thumbnail.py ThumbPuzzle ThumbUnsolved
"""
from snekviz import *  # noqa: F401,F403

TEXT_WIDTH = 6.3  # the right half of the frame, with a margin


class ThumbPuzzle(Scene):
    """The position before move 76 (blue to move) and the challenge. Nothing hints at where the answer goes."""

    def construct(self):
        b, pcs = board_with(DRAW_GAME, 5, 0.37)
        VGroup(b, pcs).move_to(LEFT * 3.55)
        line1 = Text('Can you find', font=FONT, font_size=80, weight=BOLD, color=INK)
        line2 = VGroup(Text('move', font=FONT, font_size=80, weight=BOLD, color=INK),
                       Text('76', font=FONT, font_size=150, weight=BOLD, color=GOLD),
                       Text('?', font=FONT, font_size=80, weight=BOLD, color=INK)).arrange(RIGHT, buff=0.22, aligned_edge=DOWN)
        sub = Text('blue to move', font=FONT, font_size=48, color=P_BLUE)
        text = VGroup(line1, line2, sub).arrange(DOWN, buff=0.35, aligned_edge=LEFT)
        text.scale_to_fit_width(TEXT_WIDTH).move_to(RIGHT * 3.7)
        self.add(b, pcs, text)


class ThumbRedWins(Scene):
    """The players' belief as a question, over a red snake winding through the centre."""

    def construct(self):
        b, pcs = board_with(WIN_GAME, 9, 0.37)
        VGroup(b, pcs).move_to(LEFT * 3.55)
        line1 = Text('Red', font=FONT, font_size=150, weight=BOLD, color=P_RED)
        line2 = Text('always wins?', font=FONT, font_size=96, weight=BOLD, color=INK)
        text = VGroup(line1, line2).arrange(DOWN, buff=0.3, aligned_edge=LEFT)
        text.scale_to_fit_width(TEXT_WIDTH).move_to(RIGHT * 3.7)
        self.add(b, pcs, text)


class ThumbSolverUnsure(Scene):
    """What players say, and the twist under it."""

    def construct(self):
        # the board runs off the left edge, so the text can take most of the frame
        b, pcs = board_with(WIN_GAME, 9, 0.37)
        VGroup(b, pcs).move_to(LEFT * 5.0)
        line1 = Text('Does red really', font=FONT, font_size=110, weight=BOLD, color=INK, t2c={'red': P_RED})
        line2 = Text('always win?', font=FONT, font_size=110, weight=BOLD, color=INK)
        text = VGroup(line1, line2).arrange(DOWN, buff=0.4, aligned_edge=LEFT)
        text.scale_to_fit_width(8.2).move_to(RIGHT * 2.75)
        self.add(b, pcs, text)


class ThumbUnsolved(Scene):
    """A finished game on a big board, and the title in a few words."""

    def construct(self):
        b, pcs = board_with(WIN_GAME, 15, 0.37)
        VGroup(b, pcs).move_to(LEFT * 3.55)
        top = Text('7 turns.', font=FONT, font_size=120, weight=BOLD, color=INK)
        bottom = Text('Still unsolved.', font=FONT, font_size=84, weight=BOLD, color=GOLD)
        text = VGroup(top, bottom).arrange(DOWN, buff=0.35, aligned_edge=LEFT)
        text.scale_to_fit_width(TEXT_WIDTH).move_to(RIGHT * 3.7)
        self.add(b, pcs, text)

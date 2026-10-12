"""Blurs the reviewers' usernames, badges and avatars in a BoardGameGeek ratings screenshot, keeping the
rating, the month and the comment. The original stays outside the project; only the blurred copy is kept.

    .venv/Scripts/python.exe redact_bgg.py <screenshot.png> assets/bgg_ratings.png
"""
import sys

import numpy as np
from PIL import Image, ImageFilter

BOX = (240, 240, 242)  # the grey strip behind each reviewer's name


def strips(px):
    """(top, bottom) of every grey reviewer strip."""
    grey = (np.abs(px - np.array(BOX)).sum(2) < 6).sum(1) > 100
    rows, start = [], None
    for y, g in enumerate(grey):
        if g and start is None:
            start = y
        if not g and start is not None:
            rows.append([start, y - 1])
            start = None
    merged = []
    for a, b in rows:  # the text lines split a strip into pieces a few pixels apart
        if merged and a - merged[-1][1] <= 12:
            merged[-1][1] = b
        else:
            merged.append([a, b])
    return [(a, b) for a, b in merged if b - a > 30]


def date_left(px, top):
    """Where the month ("Mar 2023") begins: the grey text at the right end of the name line is about 45-50 px
    wide, and the blank strip just left of it separates it from the last badge."""
    line = px[top + 3:top + 22]
    r, g, b = line[..., 0], line[..., 1], line[..., 2]
    text = (np.abs(r - g) < 10) & (np.abs(g - b) < 10) & (r > 90) & (r < 160)
    right = np.where(text.any(0))[0].max()
    blank = (np.abs(line - np.array(BOX)).max(2) <= 4).all(0)
    for x in range(right - 44, right - 62, -1):
        if blank[x - 2:x + 1].all():
            while blank[x + 1]:
                x += 1
            return x + 1
    return right - 50


def blur(im, box):
    region = im.crop(box)
    w, h = region.size
    small = region.resize((max(1, w // 9), max(1, h // 9)), Image.BILINEAR)
    im.paste(small.resize((w, h), Image.NEAREST).filter(ImageFilter.GaussianBlur(3)), box)


def main(src, dst):
    im = Image.open(src).convert('RGB')
    px = np.array(im).astype(int)
    for top, bottom in strips(px):
        blur(im, (92, top + 1, 134, bottom))                          # avatar
        blur(im, (134, top + 1, date_left(px, top) - 1, top + 22))     # username and badges
    im.save(dst)
    print('blurred', len(strips(px)), 'reviewers ->', dst)


if __name__ == '__main__':
    main(*sys.argv[1:3])

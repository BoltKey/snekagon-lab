"""Narration taken from the script document, so edits to the text flow into the video.

    python script.py <docs read result .json>   # refresh script_snapshot.md from a fresh read of the doc

In the snapshot, every chapter is a '## N. Title' heading followed by paragraphs. Narration = every
paragraph except Visual lines, sub-headings, quotes, diagrams and notes; [ADAM: ...] placeholders are
removed before speaking.
"""
import html
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SNAPSHOT = os.path.join(HERE, 'script_snapshot.md')
# Passages Adam speaks to the camera: recorded as video, not in the narration recorder (matched by their start)
ON_CAMERA = ('Hi, I am Adam, a Czech game designer', 'To be clear, this video is not here to prove anyone wrong',
             'So why did players feel that the first player cannot lose?', 'Throughout the years, I focused on other projects')


def on_camera(text):
    return text.strip().startswith(ON_CAMERA)


SKIP = ('**Visual:**', '[DIAGRAM]', '[QUOTE]', 'Reference for the animation', 'Numbers from our opening line', '#')


def refresh(json_path):
    raw = json.load(open(json_path, encoding='utf-8'))
    text = [b['text'] for b in raw if b['type'] == 'text' and b['text'].startswith('{')][0]
    xml = json.loads(text)['data']['xml']
    xml = re.sub(r'<blockquote.*?</blockquote>', "<paragraph id='q' h='q'>[QUOTE]</paragraph>", xml, flags=re.S)
    out = []
    pattern = r"<paragraph id='[^']*'(?: rev='\d+')? h='[^']*'(?: heading='(\d)')?[^>]*>(.*?)</paragraph>|<embed [^>]*/>"
    for m in re.finditer(pattern, xml, re.S):
        if m.group(0).startswith('<embed'):
            out.append('[DIAGRAM]')
            continue
        level, body = m.group(1), m.group(2)
        body = re.sub(r'<comment[^>]*/>', '', body)
        body = re.sub(r'<bold>(.*?)</bold>', r'**\1**', body)
        body = re.sub(r'<[^>]+>', '', body)
        out.append(('#' * int(level) + ' ' if level else '') + html.unescape(body))
    open(SNAPSHOT, 'w', encoding='utf-8').write('\n\n'.join(out))


def load():
    chapters, current = {}, None
    for block in open(SNAPSHOT, encoding='utf-8').read().split('\n\n'):
        b = block.strip()
        m = re.match(r'^## (\d+)\. ', b)
        if m:
            current = int(m.group(1))
            chapters[current] = []
            continue
        if b.startswith('## '):
            current = None
            continue
        if current is None or not b or b.startswith(SKIP):
            continue
        b = re.sub(r'\s*\[ADAM:[^\]]*\]', '', b).replace('**', '').strip()
        if b:
            chapters[current].append(b)
    return chapters


NARRATION = load() if os.path.exists(SNAPSHOT) else {}


def para(chapter, i):
    return NARRATION[chapter][i]


def split_at(text, anchors):
    """Splits a paragraph before each anchor phrase (anchors missing from the text are ignored)."""
    parts, rest = [], text
    for a in anchors:
        k = rest.find(a)
        if k > 0:
            parts.append(rest[:k].strip())
            rest = rest[k:]
    parts.append(rest.strip())
    return parts


if __name__ == '__main__':
    refresh(sys.argv[1])
    for ch, ps in sorted(load().items()):
        print(ch, len(ps), 'paragraphs')

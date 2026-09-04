#!/usr/bin/env python3
"""mdpage.py — Markdown の文書を HTML の中身にする（docs/gen.py から使う）

書いてある Markdown の書き方だけを扱う小さなもの。**外の道具には頼らない**
（この処理系そのものと同じ考え方で、入れるものを増やさない）。

扱うもの:
    見出し（# 〜 ######。目次のために id を付ける）
    段落・空行
    囲みコード（``` … ```）
    表（| … | の並び）
    箇条書き（- ・番号つき 1.）
    行の中の `コード` **太字** [文字](道)

扱わないもの（書いてある文書に出てこないため）:
    引用（>）・画像・入れ子の箇条書き・生の HTML
"""
import html
import re

import shkdoc

__all__ = ['render', 'headings']


def esc(s):
    return html.escape(s, quote=False)


def slug(text):
    """見出しから id を作る。日本語はそのまま残す（URL には %xx で載る）"""
    s = re.sub(r'`|\*\*|\[|\]\([^)]*\)', '', text).strip()
    s = re.sub(r'[\s/]+', '-', s)
    return re.sub(r'[^\w\-（）()・。、！？:：.]', '', s, flags=re.UNICODE)


def inline(text, link=None):
    """行の中の書き方を直す。link は道の書き換え（None ならそのまま）"""
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        # `コード`（中は何も直さない）
        if c == '`':
            j = text.find('`', i + 1)
            if j > 0:
                out.append('<code>%s</code>' % esc(text[i + 1:j]))
                i = j + 1
                continue
        # **太字**
        if text.startswith('**', i):
            j = text.find('**', i + 2)
            if j > 0:
                out.append('<strong>%s</strong>' % inline(text[i + 2:j], link))
                i = j + 2
                continue
        # [文字](道)
        if c == '[':
            m = re.match(r'\[([^\]]*)\]\(([^)]*)\)', text[i:])
            if m:
                label, href = m.group(1), m.group(2)
                href = link(href) if link else href
                if href is None:      # 行き先が無いものは、ただの文字にする
                    out.append(inline(label, link))
                else:
                    out.append('<a href="%s">%s</a>' % (esc(href), inline(label, link)))
                i += m.end()
                continue
        out.append(esc(c))
        i += 1
    return ''.join(out)


def _table(rows, link):
    out = ['<table><thead><tr>']
    for cell in rows[0]:
        out.append('<th>%s</th>' % inline(cell, link))
    out.append('</tr></thead><tbody>')
    for r in rows[2:]:                      # 1 行目は見出し、2 行目は区切り
        out.append('<tr>')
        for cell in r:
            out.append('<td>%s</td>' % inline(cell, link))
        out.append('</tr>')
    out.append('</tbody></table>')
    return '\n'.join(out)


def _cells(line):
    line = line.strip()
    if line.startswith('|'):
        line = line[1:]
    if line.endswith('|'):
        line = line[:-1]
    return [c.strip() for c in line.split('|')]


def headings(text):
    """[(深さ, 文字, id), …]。目次を作るのに使う"""
    out = []
    fence = False
    for line in text.split('\n'):
        if line.startswith('```'):
            fence = not fence
            continue
        if fence:
            continue
        m = re.match(r'^(#{1,6})\s+(.*)$', line)
        if m:
            out.append((len(m.group(1)), m.group(2).strip(), slug(m.group(2))))
    return out


def render(text, link=None):
    """Markdown → HTML の中身（<body> に入れるところだけ）"""
    lines = text.split('\n')
    out = []
    para = []
    items = None       # 箇条書きをためるところ
    table = None
    i = 0

    def flush_para():
        if para:
            # 段落の中の改行はつなぐ。日本語どうしのあいだに空白を入れない（shkdoc.flow）
            out.append('<p>%s</p>' % inline(shkdoc.flow('\n'.join(para)), link))
            del para[:]

    def flush_list():
        nonlocal items
        if items is not None:
            out.append('<%s>' % items[0])
            for it in items[1]:
                out.append('<li>%s</li>' % inline(it, link))
            out.append('</%s>' % items[0])
            items = None

    def flush_table():
        nonlocal table
        if table is not None:
            out.append(_table(table, link))
            table = None

    def flush_all():
        flush_para()
        flush_list()
        flush_table()

    while i < len(lines):
        line = lines[i]

        # 囲みコード
        if line.startswith('```'):
            flush_all()
            i += 1
            code = []
            while i < len(lines) and not lines[i].startswith('```'):
                code.append(lines[i])
                i += 1
            i += 1
            out.append('<pre class="ex">%s</pre>' % esc('\n'.join(code)))
            continue

        # 見出し
        m = re.match(r'^(#{1,6})\s+(.*)$', line)
        if m:
            flush_all()
            depth, title = len(m.group(1)), m.group(2).strip()
            out.append('<h%d id="%s">%s</h%d>' % (depth, esc(slug(title)), inline(title, link), depth))
            i += 1
            continue

        # 区切り線
        if re.match(r'^\s*---+\s*$', line):
            flush_all()
            out.append('<hr>')
            i += 1
            continue

        # 表
        if line.lstrip().startswith('|'):
            flush_para()
            flush_list()
            if table is None:
                table = []
            table.append(_cells(line))
            i += 1
            continue
        flush_table()

        # 箇条書き
        m = re.match(r'^\s*([-*]|\d+\.)\s+(.*)$', line)
        if m:
            flush_para()
            kind = 'ul' if m.group(1) in ('-', '*') else 'ol'
            if items is None or items[0] != kind:
                flush_list()
                items = (kind, [])
            items[1].append(m.group(2))
            i += 1
            continue

        # 空行と、ふつうの行
        if not line.strip():
            flush_all()
        else:
            flush_list()
            para.append(line.strip())
        i += 1

    flush_all()
    return '\n'.join(out)

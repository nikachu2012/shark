#!/usr/bin/env python3
"""mdpage.py — Markdown 文書を HTML コンテンツに変換（docs/gen.py から利用）

リポジトリ内のドキュメントで使用されている構文のみを対象とする軽量パーサー。
外部ライブラリへの依存なし（Shark 処理系本体と同様のゼロ依存方針）。

対応構文:
    見出し（# 〜 ######、目次リンク用 id 属性を自動付与）
    段落・空行
    フェンスドコードブロック（``` … ```）
    テーブル（| … |）
    リスト（箇条書き - / *、番号付き 1.）
    インライン装飾: `コード`, **太字**, [テキスト](リンク先)

非対応構文（本リポジトリの文書で未使用のため）:
    引用（>）、画像構文、ネストされたリスト、raw HTML
"""
import html
import re

import shkdoc

__all__ = ['render', 'headings']


def esc(s):
    return html.escape(s, quote=False)


def slug(text):
    """見出し文字列からアンカー用 id を生成。日本語はそのまま保持（URL エンコード対応）"""
    s = re.sub(r'`|\*\*|\[|\]\([^)]*\)', '', text).strip()
    s = re.sub(r'[\s/]+', '-', s)
    return re.sub(r'[^\w\-（）()・。、！？:：.]', '', s, flags=re.UNICODE)


def inline(text, link=None):
    """インラインマークダウンを展開。link は URL 変換関数（None の場合はそのまま出力）"""
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
                if href is None:      # リンク先が無効な場合はプレーンテキストとして出力
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
    for r in rows[2:]:                      # 1 行目はヘッダー、2 行目は区切り線
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
    """[(見出しレベル, テキスト, id), …]。目次（TOC）生成用"""
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
    """Markdown → HTML コンテンツ変換（<body> 直下の要素を出力）"""
    lines = text.split('\n')
    out = []
    para = []
    items = None       # リスト項目バッファ
    table = None
    i = 0

    def flush_para():
        if para:
            # 段落内の改行を連結。日本語間の不要なスペースは除去（shkdoc.flow）
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

        # コードブロック
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

        # 空行と通常テキスト行
        if not line.strip():
            flush_all()
        else:
            flush_list()
            para.append(line.strip())
        i += 1

    flush_all()
    return '\n'.join(out)

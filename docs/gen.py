#!/usr/bin/env python3
# gen.py — stdlib/*.shk から HTML のリファレンスを作る（make docs）。
#
#   python3 docs/gen.py [出力先]              # 既定は docs/reference/
#   python3 docs/gen.py out --stdlib mylib    # 自分の宣言ファイルから作る
#
# ライブラリごとに1枚と、目次（索引つき）を作る。中身は宣言ファイルが正で、
# ここは並べるだけ。実装（core/lib/*.cpp）と食い違っていれば最後に知らせる。
import html
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mdpage  # noqa: E402
import shkdoc  # noqa: E402

# Windows の端末は既定が UTF-8 ではない。Shark も、この道具の知らせも UTF-8 なので、
# 出す側と読む側の両方をそろえておく（そろえないと日本語が化け、読むときは落ちる）
if sys.platform == 'win32':
    import ctypes
    ctypes.windll.kernel32.SetConsoleOutputCP(65001)
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')


def esc(s):
    return html.escape(s, quote=True)


def anchor(page, item, owner=''):
    return shkdoc.qualified(page, item, owner) or item['name']


def call_name(page, item, owner):
    """一覧と見出しに出す呼び名。math.sqrt / string.len()"""
    mod = page['module']
    short = mod.split('.')[-1] if mod.startswith('std.') else ''
    if owner:
        return '%s.%s' % (owner, item['name'])
    return '%s.%s' % (short, item['name']) if short else item['name']


# ---------------------------------------------------------------- 1件ずつ
def render_item(page, item, owner=''):
    out = []
    name = call_name(page, item, owner)
    aid = anchor(page, item, owner)
    if item['kind'] == 'const':
        head = '<span class="nm">%s</span>: <span class="ty">%s</span>' % (esc(name), esc(item['ret']))
    else:
        params = ', '.join('<span class="pm">%s</span>' % esc(p) for p in item['params'])
        ret = ' &rarr; <span class="ty">%s</span>' % esc(item['ret']) if item['ret'] else ''
        head = '<span class="nm">%s</span>%s(%s)%s' % (esc(name), esc(item['generic']), params, ret)

    out.append('<dl class="item" id="%s">' % esc(aid))
    out.append('<dt>%s<a class="pl" href="#%s" title="ここへのリンク">¶</a></dt>' % (head, esc(aid)))
    out.append('<dd>')
    for para in item['doc'].split('\n\n'):
        if para.strip():
            out.append('<p>%s</p>' % esc(shkdoc.flow(para.strip())))
    if item['overloads']:
        pre = name[:-len(item['name'])]        # math. / string. などの前置き
        out.append('<p class="ovl">ほかの形: %s</p>' %
                   ' '.join('<code>%s%s</code>' % (esc(pre), esc(o)) for o in item['overloads']))
    if any(a[2] for a in item['args']):      # 説明が書かれているときだけ出す
        out.append('<table class="args"><tbody>')
        for a in item['args']:
            out.append('<tr><th>%s</th><td class="ty">%s</td><td>%s</td></tr>'
                       % (esc(a[0]), esc(a[1]), esc(a[2])))
        out.append('</tbody></table>')
    if item['ret_doc']:
        out.append('<p class="ret">戻り値: %s</p>' % esc(item['ret_doc']))
    for note in item['note']:
        out.append('<p class="note">%s</p>' % esc(note))
    if item['example']:
        out.append('<pre class="ex">%s</pre>' % esc(item['example']))
    out.append('</dd></dl>')
    return '\n'.join(out)


def quick_index(page):
    """ページの頭に置く、名前だけの一覧"""
    links = []
    for it in page['items']:
        links.append((call_name(page, it, ''), anchor(page, it)))
    for cls in page['classes']:
        for it in cls['items']:
            links.append((call_name(page, it, cls['name']), anchor(page, it, cls['name'])))
    if not links:
        return ''
    cells = ' '.join('<a href="#%s">%s</a>' % (esc(a), esc(n)) for n, a in links)
    return '<div class="quick">%s</div>' % cells


# ---------------------------------------------------------------- ページ
def render_page(page, pages):
    body = [nav(pages, page['file'])]
    body.append('<h1>%s</h1>' % esc(page['title']))
    if page['doc']:
        for para in page['doc'].split('\n\n'):
            body.append('<p class="lead">%s</p>' % esc(shkdoc.flow(para.strip())))
    if page.get('example'):
        body.append('<pre class="ex">%s</pre>' % esc(page['example']))
    body.append(quick_index(page))

    if page['items']:
        if page['classes']:
            body.append('<h2>関数</h2>')
        for it in page['items']:
            body.append(render_item(page, it))
    for cls in page['classes']:
        body.append('<h2 id="%s">%s%s</h2>' % (esc(cls['name']), esc(cls['name']), esc(cls['generic'])))
        if cls['doc']:
            for para in cls['doc'].split('\n\n'):
                body.append('<p class="lead">%s</p>' % esc(shkdoc.flow(para.strip())))
        for it in cls['items']:
            body.append(render_item(page, it, cls['name']))
    return frame(page['title'], '\n'.join(body))


def nav(pages, here):
    links = ['<a href="index.html">リファレンス</a>']
    mark = ' class="here"' if here == 'guide' else ''
    links.append('<a href="guide.html"%s>言語</a>' % mark)
    for p in pages:
        mark = ' class="here"' if p['file'] == here else ''
        links.append('<a href="%s.html"%s>%s</a>' % (esc(p['file']), mark, esc(p['title'])))
    return '<nav>%s</nav>' % ' '.join(links)


# --- 言語そのものの使い方（docs/reference.md）------------------------------
# 中身は Markdown が正。ここは HTML に直して、リファレンスと同じ見た目で並べるだけ。
# **ブラウザ版（web/dist）にも入る**ので、外へのリンクは作らない（tools/mdpage.py）
def render_guide(root, pages):
    path = os.path.join(root, 'docs/reference.md')
    with open(path, encoding='utf-8') as f:
        text = f.read()

    def link(href):
        # 同じところに出しているページへは繋ぐ。それ以外（元のファイル）は文字のまま
        if href.startswith('#'):
            return href
        name = os.path.basename(href)
        if name == 'reference.md':
            return 'guide.html'
        for p in pages:
            if name == p['file'] + '.md':
                return p['file'] + '.html'
        return None

    body = [nav(pages, 'guide')]
    # 見出しからの目次。長い文書なので、上から飛べるようにする
    heads = [h for h in mdpage.headings(text) if h[0] == 2]
    if heads:
        body.append('<nav class="toc"><b>目次</b> ')
        body.append(' '.join('<a href="#%s">%s</a>' % (esc(h[2]), esc(h[1])) for h in heads))
        body.append('</nav>')
    body.append(mdpage.render(text, link))
    return frame('言語リファレンス', '\n'.join(body))


def render_index(pages):
    body = [nav(pages, 'index')]
    body.append('<h1>Shark リファレンス</h1>')
    body.append('<p class="lead">この処理系が持っている関数と型の一覧。'
                '中身は <code>stdlib/*.shk</code>（宣言ファイル）から作っている。'
                '言語そのものの使い方は <a href="guide.html">言語リファレンス</a>。</p>')

    body.append('<h2>ライブラリ</h2>')
    body.append('<table class="toc"><tbody>')
    for p in pages:
        n = len(p['items']) + sum(len(c['items']) for c in p['classes'])
        body.append('<tr><th><a href="%s.html">%s</a></th><td>%s</td><td class="n">%d</td></tr>'
                    % (esc(p['file']), esc(p['title']), esc(p['summary']), n))
    body.append('</tbody></table>')

    body.append('<h2 id="genindex">索引</h2>')
    entries = []
    for p in pages:
        for it in p['items']:
            entries.append((call_name(p, it, ''), p['file'], anchor(p, it)))
        for cls in p['classes']:
            for it in cls['items']:
                entries.append((call_name(p, it, cls['name']), p['file'], anchor(p, it, cls['name'])))
    entries.sort(key=lambda e: e[0].lower())
    body.append('<ul class="index">')
    for name, file, aid in entries:
        body.append('<li><a href="%s.html#%s">%s</a></li>' % (esc(file), esc(aid), esc(name)))
    body.append('</ul>')
    return frame('Shark リファレンス', '\n'.join(body))


def frame(title, body):
    return """<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%s — Shark リファレンス</title>
<link rel="stylesheet" href="style.css">
<link rel="icon" href="data:image/svg+xml,%%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%%3E%%3Ctext y='26' font-size='26'%%3E%%F0%%9F%%A6%%88%%3C/text%%3E%%3C/svg%%3E">
</head>
<body>
<main>
%s
<footer>docs/gen.py が stdlib/*.shk から作ったもの。直すのは宣言ファイルの方。</footer>
</main>
</body>
</html>
""" % (esc(title), body)


STYLE = """/* style.css — docs/gen.py が置く。Shark リファレンスの見た目 */
:root {
  --bg: #ffffff; --ink: #1a2430; --dim: #5b6b7a; --line: #dde5ec;
  --link: #0e7490; --code-bg: #f5f8fa; --mark: #f0f6f9;
  --mono: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, "Noto Sans Mono", monospace;
  --ui: system-ui, -apple-system, "Hiragino Sans", "Noto Sans JP", sans-serif;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #10191f; --ink: #dbe6ee; --dim: #8ba0b0; --line: #24333e;
    --link: #56c8e8; --code-bg: #16222b; --mark: #16222b;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink);
       font-family: var(--ui); font-size: 15px; line-height: 1.75; }
main { max-width: 52em; margin: 0 auto; padding: 0 20px 72px; }
a { color: var(--link); }

nav.toc { margin: 0 0 28px; padding: 12px 14px; border: 1px solid var(--line);
          border-radius: 8px; background: var(--mark); font-size: 14px; line-height: 2; }
nav.toc b { margin-right: 8px; color: var(--dim); font-weight: 600; }
nav.toc a { margin-right: 12px; white-space: nowrap; }
nav { margin: 0 -20px 28px; padding: 10px 20px; border-bottom: 1px solid var(--line);
      font-size: 13px; line-height: 2.1; }
nav a { margin-right: 14px; text-decoration: none; color: var(--dim); }
nav a:hover { color: var(--link); text-decoration: underline; }
nav a.here { color: var(--ink); font-weight: 600; }

h1 { font-size: 26px; margin: 22px 0 10px; }
h2 { font-size: 19px; margin: 40px 0 8px; padding-bottom: 5px; border-bottom: 1px solid var(--line); }
p.lead { margin: 8px 0; color: var(--dim); }
footer { margin-top: 56px; padding-top: 12px; border-top: 1px solid var(--line);
         color: var(--dim); font-size: 12px; }

.quick { margin: 18px 0 8px; padding: 12px 14px; background: var(--mark);
         border: 1px solid var(--line); border-radius: 6px; font-family: var(--mono);
         font-size: 12.5px; line-height: 2; }
.quick a { margin-right: 12px; text-decoration: none; white-space: nowrap; }
.quick a:hover { text-decoration: underline; }

dl.item { margin: 26px 0 0; }
dl.item dt { font-family: var(--mono); font-size: 14px; padding: 4px 0 4px 10px;
             border-left: 3px solid var(--line); }
dl.item dt .nm { font-weight: 700; }
dl.item dt .ty, dl.item dt .pm { color: var(--dim); }
dl.item dd { margin: 6px 0 0 13px; }
dl.item dd p { margin: 6px 0; }
.pl { float: right; color: var(--line); text-decoration: none; font-size: 13px; }
dl.item:hover .pl { color: var(--dim); }

table.args { border-collapse: collapse; margin: 8px 0; font-size: 14px; }
table.args th { text-align: left; font-family: var(--mono); font-weight: 500;
                padding: 2px 14px 2px 0; vertical-align: top; white-space: nowrap; }
table.args td { padding: 2px 0; color: var(--dim); }
table.args td.ty { font-family: var(--mono); padding-right: 14px; white-space: nowrap; }
p.ret, p.note, p.ovl { color: var(--dim); font-size: 13.5px; }
p.ovl code { font-family: var(--mono); font-size: 12.5px; background: var(--code-bg);
             border: 1px solid var(--line); border-radius: 4px; padding: 1px 5px; }

pre.ex { margin: 10px 0 0; padding: 10px 12px; overflow-x: auto;
         background: var(--code-bg); border: 1px solid var(--line); border-radius: 6px;
         font-family: var(--mono); font-size: 12.5px; line-height: 1.65; }
code { font-family: var(--mono); font-size: 0.92em; }

table.toc { border-collapse: collapse; width: 100%; margin: 12px 0; }
table.toc th { text-align: left; font-weight: 600; padding: 5px 14px 5px 0;
               white-space: nowrap; vertical-align: top; }
table.toc td { padding: 5px 0; color: var(--dim); }
table.toc td.n { text-align: right; white-space: nowrap; padding-left: 14px; }

/* 言語リファレンス（guide.html。Markdown から作ったところ）*/
main > table { border-collapse: collapse; width: 100%; margin: 14px 0; font-size: 14px; }
main > table th, main > table td { border: 1px solid var(--line); padding: 6px 10px;
                                   text-align: left; vertical-align: top; }
main > table th { background: var(--mark); font-weight: 600; white-space: nowrap; }
main > ul, main > ol { padding-left: 1.4em; }
main > ul li, main > ol li { margin: 3px 0; }
hr { border: 0; border-top: 1px solid var(--line); margin: 32px 0; }

ul.index { columns: 3 12em; list-style: none; margin: 12px 0; padding: 0;
           font-family: var(--mono); font-size: 12.5px; line-height: 1.9; }
ul.index a { text-decoration: none; }
ul.index a:hover { text-decoration: underline; }
"""


def build(root, out_dir, stdlib='stdlib'):
    pages = shkdoc.parse(root, stdlib)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'style.css'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(STYLE)
    with open(os.path.join(out_dir, 'index.html'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(render_index(pages))
    for page in pages:
        with open(os.path.join(out_dir, page['file'] + '.html'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(render_page(page, pages))
    with open(os.path.join(out_dir, 'guide.html'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(render_guide(root, pages))
    return pages


def report(root, pages):
    """例が無いもの・実装と食い違うものを知らせる"""
    total = no_ex = 0
    for page in pages:
        items = list(page['items']) + [i for c in page['classes'] for i in c['items']]
        for it in items:
            total += 1
            if not it['example']:
                no_ex += 1
                print('  例が無い: %s.%s' % (page['file'], it['name']))
    missing, extra = shkdoc.crosscheck(pages, shkdoc.core_names(root))
    for n in missing:
        print('  宣言が無い（実装にはある）: %s' % n)
    for n in extra:
        print('  実装に無い（宣言にはある）: %s' % n)
    return total, no_ex, len(missing) + len(extra)


def main():
    args, out_dir, stdlib = sys.argv[1:], os.path.join(ROOT, 'docs/reference'), 'stdlib'
    while args:
        a = args.pop(0)
        if a == '--stdlib':
            stdlib = args.pop(0)
        else:
            out_dir = a
    if os.path.isdir(out_dir):
        for fn in os.listdir(out_dir):       # 消えた宣言のページを残さない
            if fn.endswith(('.html', '.css')):
                os.remove(os.path.join(out_dir, fn))
    pages = build(ROOT, out_dir, stdlib)
    total, no_ex, drift = report(ROOT, pages)
    print('%s に %d ページ（%d 件、例つき %d 件）'
          % (os.path.relpath(out_dir, ROOT), len(pages) + 2, total, total - no_ex))
    return 1 if drift else 0


if __name__ == '__main__':
    sys.exit(main())

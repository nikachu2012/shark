#!/usr/bin/env python3
# shkdoc.py — stdlib/*.shk（宣言ファイル）を読む。
#
# 標準ライブラリの「名前・型・説明・例」は stdlib/ の宣言ファイルが正。
# ここはそれを読んで構造にするだけで、出力は持たない。使う側は2つ。
#
#   docs/gen.py   HTML のリファレンス（ライブラリごとに1枚）
#   web/api.py    プレイグラウンドの入力補完（api.js）
#
# 宣言ファイルの書き方は stdlib/README.md にある。要点だけ:
#
#   /// 説明。1行目が一覧に出る。
#   ///
#   /// 引数:
#   ///   x   説明
#   ///
#   /// 例:
#   ///   print(math.sqrt(2.0));   // 1.4142135623730951
#   func sqrt(x: float) -> float;
#
# 実装（core/lib/*.cpp）と突き合わせて、片方にしか無いものは呼ぶ側に返す。
import os
import re

SECTIONS = ('引数', '戻り値', '例', '注意')
DOC = re.compile(r'^\s*///\s?(.*)$')
MODULE = re.compile(r'^\s*module\s+([\w.]+)\s*;')
CLASS = re.compile(r'^\s*class\s+([A-Za-z_]\w*)\s*(<[^>]*>)?\s*\{')
FUNC = re.compile(r'^\s*func\s+([A-Za-z_]\w*)\s*(<[^>]*>)?\s*\((.*)\)\s*(?:->\s*([^;{]+?))?\s*;')
CONST = re.compile(r'^\s*const\s+([A-Za-z_]\w*)\s*:\s*([^;]+);')


def split_params(s):
    """かっこの中を , で分ける。list<map<K, V>> のような入れ子は分けない"""
    out, depth, cur = [], 0, ''
    for c in s:
        if c in '<([':
            depth += 1
        elif c in '>)]':
            depth -= 1
        if c == ',' and depth == 0:
            if cur.strip():
                out.append(cur.strip())
            cur = ''
            continue
        cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def parse_doc(lines):
    """/// の中身を、説明・引数・例・注意に分ける"""
    doc = {'text': [], 'args': [], 'example': '', 'note': [], 'run': True}
    section, body = '', []

    def close():
        if section == '例':
            doc['example'] = dedent(body)
        elif section == '引数':
            for line in body:
                m = re.match(r'^\s*(\S+)\s\s+(.*)$', line) or re.match(r'^\s*(\S+)\s+—\s*(.*)$', line)
                if m:
                    doc['args'].append([m.group(1), m.group(2).strip()])
        elif section == '注意':
            doc['note'] = [l.strip() for l in body if l.strip()]
        elif section == '戻り値':
            doc['ret_doc'] = ' '.join(l.strip() for l in body if l.strip())

    for line in lines:
        m = re.match(r'^(%s)(（動かさない）)?\s*:\s*$' % '|'.join(SECTIONS), line.strip())
        if m:
            close()
            section, body = m.group(1), []
            if m.group(2):
                doc['run'] = False
            continue
        if section:
            body.append(line)
        else:
            doc['text'].append(line.strip())
    close()
    while doc['text'] and not doc['text'][-1]:
        doc['text'].pop()
    return doc


def summarize(text):
    """一覧に出す1行。最初の段落を1行にして、長ければ最初の文まで"""
    para = flow(text.split('\n\n')[0])
    if len(para) > 60 and '。' in para:
        para = para.split('。')[0] + '。'
    return para


def flow(text):
    """段落の中の改行をつなぐ。日本語どうしはそのまま、英数字の間には空白を入れる"""
    out = ''
    for line in text.split('\n'):
        if not line:
            continue
        if out and (ord(out[-1]) < 0x2E80 or ord(line[0]) < 0x2E80):
            out += ' '
        out += line
    return out


def dedent(lines):
    keep = [l for l in lines if l.strip()]
    pad = min([len(l) - len(l.lstrip()) for l in keep] or [0])
    return '\n'.join(l[pad:] if l.strip() else '' for l in lines).strip('\n')


def signature(name, params, ret):
    return '%s(%s)%s' % (name, ', '.join(params), (' -> ' + ret) if ret else '')


def arg_rows(params, argdocs):
    """['p: string'] と {'p': '説明'} → [['p', 'string', '説明']]"""
    out = []
    for p in params:
        name, _, typ = p.partition(':')
        name = name.strip()
        if name.startswith('ref '):
            name = name[4:].strip()
        out.append([name, typ.strip(), argdocs.get(name, '')])
    return out


def make_item(kind, name, owner, params, ret, doc, generic=''):
    d = parse_doc(doc)
    text = '\n'.join(d['text']).strip()
    item = {
        'kind': kind, 'name': name, 'owner': owner, 'generic': generic or '',
        'params': params, 'ret': ret,
        'sig': signature(name + (generic or ''), params, ret) if kind == 'func'
               else '%s: %s' % (name, ret),
        'doc': text, 'summary': summarize(text), 'args': arg_rows(params, dict(d['args'])),
        'example': d['example'], 'run': d['run'], 'note': d['note'],
        'ret_doc': d.get('ret_doc', ''), 'overloads': [],
    }
    return item


def parse_file(path):
    """1つの宣言ファイル → {'module':..., 'title':..., 'doc':..., 'items':[...]}"""
    name = os.path.basename(path)[:-4]
    out = {'file': name, 'module': '', 'title': name, 'doc': '', 'summary': '',
           'items': [], 'classes': []}
    doc, cls = [], None
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')

    for line in lines:
        m = DOC.match(line)
        if m:
            doc.append(m.group(1))
            continue
        if not line.strip() and not doc:
            continue

        m = MODULE.match(line)
        if m:
            d = parse_doc(doc)
            text = '\n'.join(d['text']).strip()
            out['module'] = m.group(1)
            out['title'] = text.split('\n')[0] if text else m.group(1)
            out['doc'] = '\n'.join(text.split('\n')[1:]).strip()
            out['summary'] = summarize(out['doc'])
            out['example'] = d['example']
            doc = []
            continue

        m = CLASS.match(line)
        if m:
            d = parse_doc(doc)
            text = '\n'.join(d['text']).strip()
            cls = {'name': m.group(1), 'generic': m.group(2) or '', 'doc': text,
                   'summary': summarize(text), 'items': []}
            out['classes'].append(cls)
            doc = []
            continue

        if cls is not None and line.strip() == '}':
            cls = None
            doc = []
            continue

        m = FUNC.match(line)
        if m:
            params = split_params(m.group(3))
            ret = (m.group(4) or '').strip()
            owner = cls['name'] if cls else ''
            item = make_item('func', m.group(1), owner, params, ret, doc, m.group(2))
            (cls['items'] if cls else out['items']).append(item)
            doc = []
            continue

        m = CONST.match(line)
        if m:
            item = make_item('const', m.group(1), cls['name'] if cls else '', [],
                             m.group(2).strip(), doc)
            (cls['items'] if cls else out['items']).append(item)
            doc = []
            continue

        doc = []
    return out


def merge_overloads(items):
    """同じ名前の宣言を1つにまとめる（print(v: string) と print(v: int)）"""
    out, seen = [], {}
    for it in items:
        first = seen.get(it['name'])
        if first is None:
            seen[it['name']] = it
            out.append(it)
            continue
        first['overloads'].append(it['sig'])
        for key in ('doc', 'summary', 'example'):
            if not first[key] and it[key]:
                first[key] = it[key]
        if not first['args']:
            first['args'] = it['args']
    return out


def qualified(page, item, owner=''):
    """実装（registry）での名前。math.sqrt / time.Time.year / string.len"""
    mod = page['module']
    short = mod.split('.')[-1] if mod and mod != 'builtin' else ''
    parts = [p for p in (short, owner, item['name']) if p]
    return '.'.join(parts)


# 宣言ではなく実装のファイル（Shark 自身で書いた部分）。ページにはしない
IMPL_FILES = ('prelude.shk', 'prelude_ui.shk')


def parse(root, stdlib='stdlib'):
    """宣言ファイルを全部読む。prelude*.shk は宣言ではなく実装なので読まない"""
    d = os.path.join(root, stdlib)
    pages = []
    for fn in sorted(os.listdir(d)):
        if fn.endswith('.shk') and fn not in IMPL_FILES:
            page = parse_file(os.path.join(d, fn))
            page['items'] = merge_overloads(page['items'])
            for cls in page['classes']:
                cls['items'] = merge_overloads(cls['items'])
            pages.append(page)
    order = {'builtin': 0, 'types': 1}
    pages.sort(key=lambda p: (order.get(p['file'], 2), p['file']))
    return pages


# ---------------------------------------------------------------- 実装との突き合わせ
def lib_sources(root):
    lib = os.path.join(root, 'core/lib')
    for fn in sorted(os.listdir(lib)):
        if fn.endswith('.cpp'):
            with open(os.path.join(lib, fn), encoding='utf-8') as f:
                yield f.read()


def core_names(root):
    """実装が本当に持っている名前（core/lib/*.cpp の r.add と、core/check.cpp のメソッド）"""
    names, modules = set(), set()
    for src in lib_sources(root):
        for m in re.finditer(r'r\.add(?:_untyped)?\("([^"]+)"', src):
            names.add(m.group(1))
        for m in re.finditer(r'enable_module\("std\.(\w+)"\)', src):
            modules.add(m.group(1))
    # Shark 自身で書いたモジュールの関数（stdlib/prelude_ui.shk の public func）
    ui_impl = os.path.join(root, 'stdlib/prelude_ui.shk')
    if os.path.exists(ui_impl):
        with open(ui_impl, encoding='utf-8') as f:
            for m in re.finditer(r'^public func (\w+)', f.read(), re.M):
                names.add('ui.' + m.group(1))

    # 型のメソッドは型検査の表にも載る（Shark 自身で書いた sort、比較の compare）
    with open(os.path.join(root, 'core/check.cpp'), encoding='utf-8') as f:
        src = f.read()
    methods = {}
    parts = re.split(r'\n\s*case (T_\w+):', src)
    for i in range(1, len(parts) - 1, 2):
        recv = CASE_TO_TYPE.get(parts[i])
        if not recv:
            continue
        block = parts[i + 1].split('\n      break;')[0]
        for m in re.finditer(r'name == "(\w+)"', block):
            if m.group(1) not in ('This', 'init') and not m.group(1).startswith('__'):
                methods.setdefault(recv, set()).add(m.group(1))
    # 処理系が持つクラス（Error / Comparable）。make_builtin_classes に書いてある
    body = re.search(r'void Checker::make_builtin_classes\(\)(.*?)\n\}\n', src, re.S)
    cls = None
    for m in re.finditer(r'c_\w+_->name = Str\("(\w+)"\)|f->name = Str\("(\w+)"\)'
                         r'|\{"(\w+)", n_\w+', body.group(1) if body else ''):
        if m.group(1):
            cls = m.group(1)
        elif cls:
            methods.setdefault(cls, set()).add(m.group(2) or m.group(3))
    return {'names': names, 'modules': sorted(modules), 'methods': methods}


HIDDEN = ('conv.', '__', 'task.channel_cap')
# 型変換は中では conv.* という名前で入っている
CONV = {'int': 'conv.int_from_float', 'float': 'conv.float_from_int',
        'string': 'conv.string_from', 'bool': 'conv.bool_from_string'}
# check.cpp の case → 宣言ファイルでの型名
CASE_TO_TYPE = {
    'T_String': 'string', 'T_Bytes': 'bytes', 'T_List': 'list', 'T_Map': 'map',
    'T_Result': 'Result', 'T_Int': 'int', 'T_Float': 'float', 'T_Bool': 'bool',
    'T_Time': 'Time', 'T_Duration': 'Duration', 'T_File': 'File', 'T_Task': 'Task',
    'T_Channel': 'channel', 'T_Json': 'Json', 'T_Regex': 'Regex', 'T_Match': 'Match',
    'T_Output': 'Output',
}


def canonical(name):
    """突き合わせ用の形。registry と型検査で名前の付け方が違うのを吸収する

    time.Time.year → Time.year（モジュールを落とす）／ result.ok → Result.ok
    """
    part = name.split('.')
    if len(part) == 3:
        part = part[1:]
    if part[0] == 'result':
        part[0] = 'Result'
    return '.'.join(part)


def crosscheck(pages, facts):
    """宣言と実装のずれ。(実装にあるのに宣言が無い, 宣言はあるのに実装に無い)"""
    declared = set()
    for page in pages:
        for it in page['items']:
            declared.add(canonical(qualified(page, it)))
        for cls in page['classes']:
            for it in cls['items']:
                declared.add(canonical(qualified(page, it, cls['name'])))

    real = set()
    for n in facts['names']:
        if n.startswith(HIDDEN) or n in HIDDEN:
            continue
        real.add(canonical(n))
    for recv, ms in facts['methods'].items():
        for m in ms:
            real.add('%s.%s' % (recv, m))
    for name, reg in CONV.items():          # int() float() string() bool()
        if reg in facts['names']:
            real.add(name)

    return sorted(real - declared), sorted(declared - real)

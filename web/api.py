#!/usr/bin/env python3
# api.py — 入力補完（IntelliSense）に使う API の表を作る。web/build.sh から呼ばれる。
#
#   使い方: api.py <リポジトリの場所> <出力先 api.js>
#
# 手で書いた表は持たない。3つの出どころを突き合わせて作る。
#
#   1. モジュールの関数 core/lib/*.cpp の r.add("...")  ← この処理系に本当に入っているもの
#   2. 型のメソッド     core/check.cpp のメソッドの表     ← 受け手ごとの引数と戻り値
#   3. 署名と説明       spec/library/*.md, spec/types/*.md ← 引数の名前と、日本語の説明
#
# 1 と 2 に無いものは出さない。仕様にあっても、この実装が持たないものは補完に出さない。
import json
import os
import re
import sys

root, out_path = sys.argv[1], sys.argv[2]

# registry の名前の前置き → 補完で使う型の名前
RECEIVERS = [
    ('time.Time.', 'Time'), ('time.Duration.', 'Duration'), ('file.File.', 'File'),
    ('task.Task.', 'Task'), ('task.channel.', 'channel'), ('json.Json.', 'Json'),
    ('text.Regex.', 'Regex'), ('text.Match.', 'Match'), ('os.Output.', 'Output'),
]
MODULES = ['math', 'time', 'task', 'fmt', 'path', 'file', 'text', 'json', 'os', 'test']
HIDDEN = ('conv.', '__', 'task.channel_cap')
# check.cpp の case → 型の名前
CASE_TO_TYPE = {
    'T_String': 'string', 'T_Bytes': 'bytes', 'T_List': 'list', 'T_Map': 'map',
    'T_Result': 'Result', 'T_Int': 'int', 'T_Float': 'float', 'T_Bool': 'bool',
    'T_Time': 'Time', 'T_Duration': 'Duration', 'T_File': 'File', 'T_Task': 'Task',
    'T_Channel': 'channel', 'T_Json': 'Json', 'T_Regex': 'Regex', 'T_Match': 'Match',
    'T_Output': 'Output',
}
SIMPLE = {'ts': 'string', 'ti': 'int', 'tb': 'bool', 'tf': 'float', 'tv': 'void',
          'et': 'T', 'kt': 'K', 'vt': 'V'}
# 型のメソッドが載っている仕様書と、その見出しに出る型
TYPE_DOCS = {'collection': ['string', 'bytes', 'list', 'map'],
             'primitive': ['int', 'float', 'bool'],
             'optional': [], 'conversion': [], 'class': [], 'generics': [], 'inference': []}


def read(path):
    with open(os.path.join(root, path), encoding='utf-8') as f:
        return f.read()


# ------------------------------------------------ 1. モジュールの関数（registry）
def registered_names():
    names = set()
    lib = os.path.join(root, 'core/lib')
    for fn in sorted(os.listdir(lib)):
        if fn.endswith('.cpp'):
            with open(os.path.join(lib, fn), encoding='utf-8') as f:
                for m in re.finditer(r'r\.add(?:_untyped)?\("([^"]+)"', f.read()):
                    names.add(m.group(1))
    return names


# ------------------------------------------------ 2. 型のメソッド（check.cpp）
def decode_type(expr, recv=None):
    expr = expr.strip()
    if expr in SIMPLE:
        t = SIMPLE[expr]
        # Task<T> / channel<T> / Result<T> の中身は T と書く
        if t == 'V' and recv in ('Task', 'channel', 'Result'):
            return 'T'
        return t
    for fn, fmt in (('optional_of', '%s?'), ('list_of', 'list<%s>'), ('result_of', 'Result<%s>')):
        m = re.match(r't_\.%s\((.*)\)$' % fn, expr)
        if m:
            return fmt % decode_type(m.group(1), recv)
    m = re.match(r't_\.simple\(T_(\w+)\)$', expr)
    if m:
        return CASE_TO_TYPE.get('T_' + m.group(1), m.group(1))
    m = re.match(r't_\.class_type\(c_(\w+)_\)$', expr)
    if m:
        return m.group(1).capitalize()
    if expr in ('t_.t_bytes()', 't_.t_string()'):
        return expr[6:-2].replace('t_', '')
    return ''


def method_types():
    """{'list': {'push': {'params': ['T'], 'ret': 'void'}}}

    型検査が持つこの表が、メソッドについての正。registry に載らないもの
    （Shark 自身で書いた sort、比較の compare）もここから拾える。
    """
    src = read('core/check.cpp')
    out = {}
    parts = re.split(r'\n\s*case (T_\w+):', src)
    for i in range(1, len(parts) - 1, 2):
        recv = CASE_TO_TYPE.get(parts[i])
        if not recv:
            continue
        block = parts[i + 1].split('\n      break;')[0]
        for chunk in re.split(r'(?=(?:else )?if \(name == ")', block):
            m = re.search(r'name == "(\w+)" && n == (\d+)', chunk)
            if not m:
                continue
            name, arity = m.group(1), int(m.group(2))
            body = chunk[m.end():]
            params = [decode_type(p, recv) for p in re.findall(r'want\.push\(([^;]+?)\);', body)]
            rm = re.search(r'ret = ([^;]+?);', body)
            ret = decode_type(rm.group(1), recv) if rm else ''
            if len(params) < arity:
                params += [''] * (arity - len(params))
            out.setdefault(recv, {}).setdefault(name, {'params': params[:arity], 'ret': ret})
    return out


# ------------------------------------------------ 3. 署名と説明（仕様書）
SIG = re.compile(r'^([A-Za-z_][A-Za-z0-9_.]*)\s*(<[^()<>]*>)?\((.*)\)\s*(?:->\s*(.+))?$')


def clean(text):
    text = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', text)     # [名前](リンク) → 名前
    return text.replace('`', '').replace('&lt;', '<').replace('&gt;', '>').strip()


def split_params(s):
    out, depth, cur = [], 0, ''
    for c in s:
        if c in '<([':
            depth += 1
        elif c in '>)]':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def doc_entries():
    """仕様書の表から、署名と説明を集める"""
    items = []
    for d in ('spec/library', 'spec/types'):
        full = os.path.join(root, d)
        for fn in sorted(os.listdir(full)):
            if not fn.endswith('.md'):
                continue
            base = fn[:-3]
            heading = ''
            for line in read(d + '/' + fn).split('\n'):
                if line.startswith('#'):
                    heading = clean(line.lstrip('#').strip())
                    continue
                if not line.startswith('|'):
                    continue
                cells = [c.strip() for c in line.strip().strip('|').split('|')]
                if len(cells) < 2 or set(cells[0]) <= set('-: '):
                    continue
                doc = clean(cells[1])
                for span in re.findall(r'`([^`]+)`', cells[0]):
                    span = span.strip()
                    m = SIG.match(span)
                    if m:
                        name, _gen, params, ret = m.groups()
                        items.append({'qualified': name, 'file': base, 'heading': heading,
                                      'params': split_params(params), 'ret': (ret or '').strip(),
                                      'doc': doc})
                        continue
                    # 定数（math.PI など。括弧が無い）
                    if re.match(r'^[A-Za-z_][A-Za-z0-9_.]*$', span) and '.' in span:
                        items.append({'qualified': span, 'file': base, 'heading': heading,
                                      'params': [], 'ret': '', 'doc': doc})
    return items


def doc_index(entries, mtypes):
    """仕様書の項目を「registry の名前」または「型.メソッド」に結び付ける"""
    idx = {}
    for c in entries:
        short = c['qualified'].split('.')[-1]
        key = None
        f = c['file']
        if f in MODULES:
            if c['qualified'].startswith(f + '.'):
                key = c['qualified']                        # 例: math.sqrt
            else:                                            # 例: f.read_line（受け手つき）
                owners = [r for p, r in RECEIVERS if p.startswith(f + '.')]
                hit = [r for r in owners if short in mtypes.get(r, {})]
                # 見出しに型の名前があれば、それを優先する
                by_head = [r for r in hit if r.lower() in c['heading'].lower()]
                if len(by_head) == 1:
                    key = ('m', by_head[0], short)
                elif len(hit) == 1:
                    key = ('m', hit[0], short)
        elif f in TYPE_DOCS:
            for t in TYPE_DOCS[f]:
                if re.search(r'(^|\W)%s(\W|$)' % re.escape(t), c['heading']):
                    if short in mtypes.get(t, {}):
                        key = ('m', t, short)
                    break
        elif f == 'builtin' and '.' not in c['qualified']:
            key = short
        if key:
            idx.setdefault(key, []).append(c)
    return idx


# ------------------------------------------------ 組み立て
def signature(name, params, ret):
    return '%s(%s)%s' % (name, ', '.join(params), (' -> ' + ret) if ret else '')


def entry(name, docs, params, ret, kind='function'):
    """仕様書の説明があればそれを、無ければ型だけの署名を使う"""
    d = docs[0] if docs else None
    p = d['params'] if d else [t for t in params if t]
    r = (d['ret'] if d and d['ret'] else ret)
    e = {'name': name, 'sig': signature(name, p, r), 'params': p, 'ret': r,
         'doc': d['doc'] if d else '', 'kind': kind}
    if docs and len(docs) > 1:      # オーバーロード
        e['overloads'] = [signature(name, o['params'], o['ret']) for o in docs[1:]]
    return e


def build():
    names = registered_names()
    mtypes = method_types()
    idx = doc_index(doc_entries(), mtypes)
    api = {'modules': {}, 'methods': {}, 'builtins': []}
    undocumented = []

    # モジュールの関数と定数
    for reg in sorted(names):
        if reg.startswith(HIDDEN) or reg in HIDDEN or '.' not in reg:
            continue
        if any(reg.startswith(p) for p, _ in RECEIVERS):
            continue
        module, short = reg.split('.', 1)
        if module not in MODULES or '.' in short:
            continue
        docs = idx.get(reg, [])
        if not docs:
            undocumented.append(reg)
        if short.isupper():                      # math.PI のような定数
            e = {'name': short, 'sig': reg, 'params': [], 'ret': '',
                 'doc': docs[0]['doc'] if docs else '', 'kind': 'const'}
        else:
            e = entry(reg, docs, [], '')
            e['name'] = short
        api['modules'].setdefault(module, {'members': []})['members'].append(e)

    # 型のメソッド
    for recv, ms in sorted(mtypes.items()):
        for name, info in ms.items():
            docs = idx.get(('m', recv, name), [])
            if not docs:
                undocumented.append('%s.%s' % (recv, name))
            e = entry(name, docs, info['params'], info['ret'], kind='method')
            e['sig'] = signature(name, e['params'], e['ret'])
            api['methods'].setdefault(recv, []).append(e)

    # 組み込み関数
    for reg in sorted(n for n in names if '.' not in n and not n.startswith('__')):
        docs = idx.get(reg, [])
        if not docs:
            undocumented.append(reg)
        api['builtins'].append(entry(reg, docs, [], ''))
    for name in ('int', 'float', 'string', 'bool'):    # 型変換（中では conv.*）
        api['builtins'].append(entry(name, idx.get(name, []), ['v'], name))

    for mod in api['modules'].values():
        mod['members'] = dedup(mod['members'])
    for k in api['methods']:
        api['methods'][k] = dedup(api['methods'][k])
    api['builtins'] = dedup(api['builtins'])
    return api, undocumented


def dedup(items):
    seen, out = {}, []
    for it in items:
        old = seen.get(it['name'])
        if old:
            if not old['doc'] and it['doc']:
                old.update(it)
            continue
        seen[it['name']] = it
        out.append(it)
    return out


api, undocumented = build()

with open(out_path, 'w', encoding='utf-8') as f:
    f.write('// api.js — web/api.py が spec/ と core/ から作る。直に書き換えない\n')
    f.write('window.SHARK_API = ')
    json.dump(api, f, ensure_ascii=False, indent=1)
    f.write(';\n')

total = len(api['builtins']) + sum(len(m['members']) for m in api['modules'].values()) + \
    sum(len(v) for v in api['methods'].values())
print('API %d 件（モジュール %d / 型 %d）' % (total, len(api['modules']), len(api['methods'])))
if undocumented:
    print('  仕様書に説明が無いもの %d 件: %s' % (len(undocumented), ' '.join(undocumented[:10])))

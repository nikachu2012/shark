// lang.js — Monaco に Shark を教える（色分けと入力補完）
//
// 補完の中身は3つから来ている。
//   * 標準ライブラリ  api.js（web/api.py が spec/ と core/ から作る）
//   * 書いている中身  この画面の文字を軽く読んで、関数・クラス・変数を拾う
//   * 誤りの指摘      本物の型検査（app.js が shk_load を呼んで印を付ける）
//
// 型推論はここでは簡単なものしか行わない。厳密な判定はコアの型検査に任せる。
window.SharkLang = (function () {
  'use strict';

  var ID = 'shark';

  // spec/syntax.md の予約語
  var KEYWORDS = ('func return var const if else while for in break continue class this super ' +
    'This public private virtual override ref import as task parallel try panic true false none').split(' ');
  var TYPES = 'int float bool string bytes void list map channel Task Result Error Range'.split(' ');

  var KEYWORD_DOC = {
    func: '関数を作る。`func name(a: int) -> int { }`',
    var: '書き換えられる変数。`var x = 1;`',
    const: '書き換えられない値。`const N = 10;`',
    ref: '引数を借りて受け取る。書き換えが呼び出し元に伝わる。`func f(ref xs: list<int>)`',
    class: 'クラスを作る。`class Fish : Comparable { }`',
    this: '自分自身',
    super: '親クラス',
    This: '自分のクラスの型',
    virtual: '子が上書きしてよい関数',
    override: '親の virtual を上書きする関数',
    public: '外から使える',
    private: '同じクラスの中だけ',
    import: 'モジュールを取り込む。`import std.math;`',
    as: '別の名前で取り込む。`import std.text as t;`',
    task: '別の流れとして走らせる。`var t = task f();`',
    parallel: 'まとめて走らせて全部待つ。`var xs = parallel { task f(); };`',
    try: '失敗したら、その場で呼び出し元に返す。`var s = try file.read(p);`',
    panic: '記録を残して止める。戻ってこない',
    none: '値が無いこと（`T?` の中身）',
    'for': '繰り返す。`for var i in range(3) { }`',
    'while': '条件が真の間くり返す',
    'return': '値を返す',
    'break': '繰り返しを抜ける',
    'continue': '次の回へ進む'
  };

  var SNIPPETS = [
    { label: 'main', doc: '入口の関数', body: 'func main() -> int {\n\t$0\n\treturn 0;\n}' },
    { label: 'func', doc: '関数', body: 'func ${1:name}(${2}) -> ${3:void} {\n\t$0\n}' },
    { label: 'for', doc: '数え上げ', body: 'for var ${1:i} in range(${2:10}) {\n\t$0\n}' },
    { label: 'forin', doc: '中身を順に', body: 'for var ${1:x} in ${2:xs} {\n\t$0\n}' },
    { label: 'while', doc: '繰り返し', body: 'while ${1:cond} {\n\t$0\n}' },
    { label: 'if', doc: '分岐', body: 'if ${1:cond} {\n\t$0\n}' },
    { label: 'ifvar', doc: '値があるときだけ', body: 'if var ${1:v} = ${2:expr} {\n\t$0\n}' },
    { label: 'class', doc: 'クラス', body: 'class ${1:Name} {\n\tpublic var ${2:field}: ${3:int};\n\n\tfunc init(${2:field}: ${3:int}) {\n\t\tthis.${2:field} = ${2:field};\n\t}\n}' },
    { label: 'test', doc: 'テスト', body: 'public func test_${1:name}() {\n\ttest.eq(${2:actual}, ${3:expected});\n}' }
  ];

  var api = { modules: {}, methods: {}, builtins: [] };
  var monaco = null;
  var cache = { key: '', info: null };

  // ============================================================ 中身を読む
  function scan(text) {
    var info = { funcs: [], classes: [], vars: [], imports: [], text: text };
    var lines = text.split('\n');
    for (var i = 0; i < lines.length; i++) {
      var line = lines[i];
      var ln = i + 1;

      var im = /^\s*import\s+([\w./]+)(?:\s+as\s+(\w+))?/.exec(line);
      if (im) {
        var p = im[1].replace(/^std\./, '');
        info.imports.push({ path: p, alias: im[2] || p.split(/[./]/).pop(), line: ln });
        continue;
      }

      var fm = /^\s*(?:(?:public|private|virtual|override)\s+)*func\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(?:->\s*([^\s{;]+))?/.exec(line);
      if (fm) {
        var params = splitTop(fm[2]);
        info.funcs.push({
          name: fm[1], params: params, ret: fm[3] || 'void', line: ln,
          col: line.indexOf(fm[1]) + 1, indent: /^\s*/.exec(line)[0].length
        });
        for (var k = 0; k < params.length; k++) {
          var pm = /^\s*(?:ref\s+)?([A-Za-z_]\w*)\s*:\s*(.+)$/.exec(params[k]);
          if (pm) info.vars.push({ name: pm[1], type: pm[2].trim(), line: ln, kind: 'param' });
        }
        continue;
      }

      var cm = /^\s*class\s+([A-Za-z_]\w*)\s*(?::\s*([^{]+))?/.exec(line);
      if (cm) {
        info.classes.push({ name: cm[1], base: (cm[2] || '').trim(), line: ln, col: line.indexOf(cm[1]) + 1 });
        continue;
      }

      var re = /(?:^|[\s({;])(?:var|const)\s+([A-Za-z_]\w*)\s*(?::\s*([^=;)]+?))?\s*(?:=\s*([^;]+))?[;)]?/g;
      var vm;
      while ((vm = re.exec(line)) !== null) {
        info.vars.push({ name: vm[1], type: (vm[2] || '').trim(), expr: (vm[3] || '').trim(), line: ln, kind: 'var' });
      }
      var fo = /for\s+var\s+([A-Za-z_]\w*)\s+in\s+([^{]+)/.exec(line);
      if (fo) info.vars.push({ name: fo[1], expr: '@each ' + fo[2].trim(), line: ln, kind: 'var' });
    }
    return info;
  }

  function scanModel(model) {
    var key = model.id + ':' + model.getVersionId();
    if (cache.key !== key) cache = { key: key, info: scan(model.getValue()) };
    return cache.info;
  }

  function splitTop(s) {
    var out = [], depth = 0, cur = '';
    for (var i = 0; i < s.length; i++) {
      var c = s[i];
      if (c === '<' || c === '(' || c === '[') depth++;
      if (c === '>' || c === ')' || c === ']') depth--;
      if (c === ',' && depth === 0) { if (cur.trim()) out.push(cur.trim()); cur = ''; continue; }
      cur += c;
    }
    if (cur.trim()) out.push(cur.trim());
    return out;
  }

  // { } を数えながら1行ずつ進む。文字列と（入れ子にできる）コメントの中は数えない。
  // visit(行, 行の前の深さ, 行の後の深さ, その行での最大の深さ) が false を返すと止まる
  function braceWalk(lines, from, visit) {
    var depth = 0, comment = 0;
    for (var i = from; i < lines.length; i++) {
      var s = lines[i], j = 0, before = depth, peak = depth;
      while (j < s.length) {
        var c = s[j], d = s[j + 1];
        if (comment > 0) {
          if (c === '/' && d === '*') { comment++; j += 2; continue; }
          if (c === '*' && d === '/') { comment--; j += 2; continue; }
          j++;
          continue;
        }
        if (c === '/' && d === '*') { comment++; j += 2; continue; }
        if (c === '/' && d === '/') break;
        if (c === '"') {                          // 文字列（f" b" もここに来る）
          j++;
          while (j < s.length && s[j] !== '"') j += (s[j] === '\\' ? 2 : 1);
          j++;
          continue;
        }
        if (c === '{') { depth++; if (depth > peak) peak = depth; }
        else if (c === '}') depth--;
        j++;
      }
      if (visit(i + 1, before, depth, peak) === false) return;
    }
  }

  // 1行ずつに分けたもの。scan のたびに作り直す
  function linesOf(info) {
    if (!info.lines) info.lines = info.text.split('\n');
    return info.lines;
  }

  // クラスの本体が閉じる行
  function classEnd(info, cls) {
    if (cls.end) return cls.end;
    var lines = linesOf(info);
    cls.end = lines.length;
    braceWalk(lines, cls.line - 1, function (ln, before, after, peak) {
      if (peak > 0 && after <= 0) { cls.end = ln; return false; }
    });
    return cls.end;
  }

  // その行が入っているクラス
  function classAt(info, line) {
    for (var i = info.classes.length - 1; i >= 0; i--) {
      var c = info.classes[i];
      if (c.line <= line && classEnd(info, c) >= line) return c;
    }
    return null;
  }

  // クラスの本体の直下か（メソッドの中ではないか）
  function atMemberLevel(info, cls, line) {
    var depth = 0;
    braceWalk(linesOf(info), cls.line - 1, function (ln, before) {
      if (ln === line) { depth = before; return false; }
    });
    return depth === 1;
  }

  var MEMBER_FUNC = /^\s*((?:(?:public|private|virtual|override)\s+)*)func\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(?:->\s*([^\s{;]+))?\s*(.*)$/;

  // クラスの中に書かれたメソッド。上書きしてよいか（virtual / override）と、
  // 本体が無いか（純粋仮想）も見る
  function methodsIn(info, cls) {
    if (cls.methods) return cls.methods;
    var lines = linesOf(info);
    var end = classEnd(info, cls);
    cls.methods = [];
    for (var i = cls.line - 1; i < end && i < lines.length; i++) {
      var m = MEMBER_FUNC.exec(lines[i]);
      if (!m) continue;
      cls.methods.push({
        name: m[2], params: splitTop(m[3]), ret: m[4] || '',
        virtual: /\b(?:virtual|override)\b/.test(m[1]), pub: /\bpublic\b/.test(m[1]),
        pure: /^;/.test((m[5] || '').trim()), line: i + 1
      });
    }
    return cls.methods;
  }

  // ============================================================ 型を見当づける
  function head(type) {
    if (!type) return '';
    var t = type.trim();
    while (t.slice(-1) === '?' || t.slice(-1) === '!') t = t.slice(0, -1);
    var lt = t.indexOf('<');
    return (lt > 0 ? t.slice(0, lt) : t).trim();
  }

  function args(type) {
    var t = (type || '').trim();
    var lt = t.indexOf('<');
    if (lt < 0) return [];
    return splitTop(t.slice(lt + 1, t.lastIndexOf('>')));
  }

  // list<int> の pop() -> T? を int? にする
  function substitute(ret, recvType) {
    var a = args(recvType);
    if (!a.length || !ret) return ret;
    var names = head(recvType) === 'map' ? ['K', 'V'] : ['T'];
    var r = ret;
    for (var i = 0; i < names.length; i++) {
      if (a[i]) r = r.replace(new RegExp('\\b' + names[i] + '\\b', 'g'), a[i]);
    }
    return r;
  }

  function unwrap(type, kind) {
    var t = (type || '').trim();
    if (kind === '?' && t.slice(-1) === '?') return t.slice(0, -1);
    if (head(t) === kind) return args(t)[0] || '';
    return t;
  }

  function memberOf(module, name) {
    var m = api.modules[module];
    if (!m) return null;
    for (var i = 0; i < m.members.length; i++) if (m.members[i].name === name) return m.members[i];
    return null;
  }

  function methodOf(type, name) {
    var list = api.methods[head(type)];
    if (!list) return null;
    for (var i = 0; i < list.length; i++) if (list[i].name === name) return list[i];
    return null;
  }

  function builtinOf(name) {
    for (var i = 0; i < api.builtins.length; i++) if (api.builtins[i].name === name) return api.builtins[i];
    return null;
  }

  function funcOf(info, name) {
    for (var i = 0; i < info.funcs.length; i++) if (info.funcs[i].name === name) return info.funcs[i];
    return null;
  }

  function classOf(info, name) {
    for (var i = 0; i < info.classes.length; i++) if (info.classes[i].name === name) return info.classes[i];
    return null;
  }

  // 変数の型。宣言の型注釈があればそれ、無ければ右辺から見当をつける
  function typeOfVar(info, name, upto) {
    var best = null;
    for (var i = 0; i < info.vars.length; i++) {
      var v = info.vars[i];
      if (v.name !== name) continue;
      if (upto && v.line > upto) continue;
      best = v;
    }
    if (!best) return '';
    if (best.type) return best.type;
    return typeOfExpr(info, best.expr || '', upto);
  }

  function typeOfExpr(info, expr, upto) {
    var e = (expr || '').trim();
    if (!e) return '';
    var m;

    if ((m = /^@each\s+(.*)$/.exec(e))) {          // for var x in xs
      var src = typeOfExpr(info, m[1], upto);
      if (head(src) === 'list') return args(src)[0] || '';
      if (head(src) === 'map') return args(src)[0] || '';
      if (head(src) === 'Range') return 'int';
      if (head(src) === 'string') return 'string';
      return '';
    }
    if (/^try\s+/.test(e)) return unwrap(typeOfExpr(info, e.replace(/^try\s+/, ''), upto), 'Result');
    if (/^task\s+/.test(e)) return 'Task<' + (typeOfExpr(info, e.replace(/^task\s+/, ''), upto) || '') + '>';
    if (/^parallel\b/.test(e)) return 'list<>';
    if (/^\[/.test(e)) {
      var first = typeOfExpr(info, e.slice(1).split(/[,\]]/)[0], upto);
      return 'list<' + first + '>';
    }
    if (/^\{/.test(e)) return 'map<>';
    if (/^f?"/.test(e)) return 'string';
    if (/^b"/.test(e)) return 'bytes';
    if (/^(true|false)\b/.test(e)) return 'bool';
    if (/^-?\d+\.\d/.test(e)) return 'float';
    if (/^-?\d+\b/.test(e)) return 'int';
    if ((m = /^(channel<[^>]*>)\s*\(/.exec(e))) return m[1];

    if ((m = /^([A-Za-z_]\w*)\s*\.\s*([A-Za-z_]\w*)\s*\(/.exec(e))) {   // 何かの呼び出し
      var mod = moduleAlias(info, m[1]);
      if (mod) {
        var me = memberOf(mod, m[2]);
        if (me) return me.ret;
      }
      var recv = typeOfVar(info, m[1], upto);
      var mm = methodOf(recv, m[2]);
      if (mm) return substitute(mm.ret, recv);
      return '';
    }
    if ((m = /^([A-Za-z_]\w*)\s*\(/.exec(e))) {
      if (classOf(info, m[1])) return m[1];
      var f = funcOf(info, m[1]);
      if (f) return f.ret;
      var b = builtinOf(m[1]);
      if (b) return b.ret;
      return '';
    }
    if ((m = /^([A-Za-z_]\w*)\s*$/.exec(e))) return typeOfVar(info, m[1], upto);
    return '';
  }

  // import した名前（別名を含む）→ モジュール名
  function moduleAlias(info, word) {
    for (var i = 0; i < info.imports.length; i++) {
      if (info.imports[i].alias === word) return api.modules[info.imports[i].path] ? info.imports[i].path : null;
    }
    return api.modules[word] ? word : null;   // import を書く前でも候補は出す
  }

  // ============================================================ 補完の中身
  function kindFor(kind) {
    var K = monaco.languages.CompletionItemKind;
    return { 'function': K.Function, method: K.Method, const: K.Constant, module: K.Module,
             keyword: K.Keyword, type: K.Class, variable: K.Variable, snippet: K.Snippet,
             field: K.Field, klass: K.Class }[kind] || K.Text;
  }

  function snippetFor(e) {
    if (e.kind === 'const') return e.name;
    var ps = e.params || [];
    if (!ps.length) return e.name + '()';
    var parts = [];
    for (var i = 0; i < ps.length; i++) {
      parts.push('${' + (i + 1) + ':' + paramName(ps[i]).replace(/[${}]/g, '') + '}');
    }
    return e.name + '(' + parts.join(', ') + ')';
  }

  // 引数ごとの説明（仕様書の 引数 の表）
  function argRows(e) {
    var rows = [];
    for (var i = 0; e.args && i < e.args.length; i++) {
      if (e.args[i][2]) rows.push('`' + (e.args[i][0] || e.args[i][1]) + '` — ' + e.args[i][2]);
    }
    return rows.join('\n\n');
  }

  function docFor(e) {
    var md = [];
    if (e.doc) md.push(e.doc);
    var rows = argRows(e);
    if (rows) md.push(rows);
    if (e.example) md.push('```shark\n' + e.example + '\n```');
    if (e.overloads && e.overloads.length) md.push('ほかの書き方:\n\n```shark\n' + e.overloads.join('\n') + '\n```');
    return md.length ? { value: md.join('\n\n'), isTrusted: false } : undefined;
  }

  function item(e, range, kind, sortPrefix) {
    return {
      label: e.name,
      kind: kindFor(kind),
      detail: e.sig || '',
      documentation: docFor(e),
      insertText: snippetFor(e),
      insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
      range: range,
      sortText: (sortPrefix || 'm') + e.name
    };
  }

  // import されていないモジュールを使ったときは、import も一緒に書き足す
  function importEdit(info, module, model) {
    for (var i = 0; i < info.imports.length; i++) if (info.imports[i].path === module) return null;
    var line = 1;
    for (var k = 0; k < info.imports.length; k++) line = Math.max(line, info.imports[k].line + 1);
    var text = 'import std.' + module + ';\n';
    if (!info.imports.length) {
      // 先頭のコメントの後ろに入れる
      var lines = model.getValue().split('\n');
      var at = 0;
      while (at < lines.length && (/^\s*\/\//.test(lines[at]) || !lines[at].trim())) at++;
      line = at + 1;
      if (at < lines.length && lines[at].trim()) text = text + '\n';
    }
    return [{
      range: new monaco.Range(line, 1, line, 1),
      text: text
    }];
  }

  function completions(model, position) {
    var info = scanModel(model);
    var upto = position.lineNumber;
    var line = model.getValueInRange({
      startLineNumber: position.lineNumber, startColumn: 1,
      endLineNumber: position.lineNumber, endColumn: position.column
    });
    var word = model.getWordUntilPosition(position);
    var range = new monaco.Range(position.lineNumber, word.startColumn, position.lineNumber, word.endColumn);
    var out = [];
    var m;

    // import std.
    if ((m = /import\s+(?:std\.)?(\w*)$/.exec(line))) {
      for (var mod in api.modules) {
        out.push({
          label: 'std.' + mod, kind: kindFor('module'), detail: 'モジュール',
          documentation: { value: memberList(mod) },
          insertText: (/std\.$/.test(line.slice(0, line.length - m[1].length)) ? mod : 'std.' + mod),
          range: range, sortText: 'a' + mod
        });
      }
      return { suggestions: out };
    }

    // 何かの後ろの「.」
    if ((m = /([A-Za-z_]\w*(?:\s*\([^()]*\))?)\s*\.\s*(\w*)$/.exec(line))) {
      var recvText = m[1].trim();
      var bare = /^[A-Za-z_]\w*$/.test(recvText) ? recvText : '';
      var mod = bare ? moduleAlias(info, bare) : null;
      if (mod) {
        var members = api.modules[mod].members;
        for (var i = 0; i < members.length; i++) {
          var it = item(members[i], range, members[i].kind === 'const' ? 'const' : 'function', 'a');
          out.push(it);
        }
        return { suggestions: out };
      }
      var t = bare ? typeOfVar(info, bare, upto) : typeOfExpr(info, recvText, upto);
      if (!t && bare) t = typeOfExpr(info, bare, upto);
      var list = api.methods[head(t)];
      if (list) {
        for (var j = 0; j < list.length; j++) {
          var e = list[j];
          var shown = { name: e.name, params: e.params, doc: e.doc,
                        sig: e.name + '(' + (e.params || []).join(', ') + ')' +
                             (e.ret ? ' -> ' + substitute(e.ret, t) : '') };
          out.push(item(shown, range, 'method', 'a'));
        }
      }
      if (classOf(info, head(t))) out = out.concat(classMembers(info, head(t), range));
      return { suggestions: out };
    }

    // クラスの本体。親から受け継いだ関数は override の雛形として出す
    var mem = memberContext(info, position, line, word);
    if (mem) {
      var stubs = overrideItems(info, mem.cls, range, mem.typed);
      if (mem.typed.override) return { suggestions: stubs };   // override と打った後は、雛形だけ
      out = out.concat(stubs);
    }

    // ふつうの位置
    for (var s = 0; s < SNIPPETS.length; s++) {
      out.push({
        label: SNIPPETS[s].label, kind: kindFor('snippet'), detail: SNIPPETS[s].doc,
        insertText: SNIPPETS[s].body,
        insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
        range: range, sortText: 'c' + SNIPPETS[s].label
      });
    }
    for (var w = 0; w < KEYWORDS.length; w++) {
      out.push({
        label: KEYWORDS[w], kind: kindFor('keyword'),
        documentation: KEYWORD_DOC[KEYWORDS[w]] ? { value: KEYWORD_DOC[KEYWORDS[w]] } : undefined,
        insertText: KEYWORDS[w], range: range, sortText: 'd' + KEYWORDS[w]
      });
    }
    for (var y = 0; y < TYPES.length; y++) {
      out.push({ label: TYPES[y], kind: kindFor('type'), detail: '型', insertText: TYPES[y],
                 range: range, sortText: 'e' + TYPES[y] });
    }
    for (var b = 0; b < api.builtins.length; b++) {
      out.push(item(api.builtins[b], range, 'function', 'b'));
    }
    for (var f = 0; f < info.funcs.length; f++) {
      var uf = info.funcs[f];
      out.push(item({ name: uf.name, params: uf.params, ret: uf.ret,
                      sig: uf.name + '(' + uf.params.join(', ') + ') -> ' + uf.ret,
                      doc: 'この画面で定義した関数' }, range, 'function', 'a'));
    }
    for (var c = 0; c < info.classes.length; c++) {
      out.push({ label: info.classes[c].name, kind: kindFor('klass'), detail: 'class',
                 insertText: info.classes[c].name, range: range, sortText: 'a' + info.classes[c].name });
    }
    var seen = {};
    for (var v = 0; v < info.vars.length; v++) {
      var vr = info.vars[v];
      if (seen[vr.name] || vr.line > upto) continue;
      seen[vr.name] = true;
      var vt = vr.type || typeOfExpr(info, vr.expr || '', upto);
      out.push({ label: vr.name, kind: kindFor('variable'), detail: vt || (vr.kind === 'param' ? '引数' : '変数'),
                 insertText: vr.name, range: range, sortText: 'a' + vr.name });
    }
    // モジュール名（import 済みでなければ import も足す）
    for (var mo in api.modules) {
      var edits = importEdit(info, mo, model);
      out.push({
        label: mo, kind: kindFor('module'),
        detail: edits ? 'モジュール（import を足します）' : 'モジュール',
        documentation: { value: memberList(mo) },
        insertText: mo + '.', range: range, sortText: 'b' + mo,
        additionalTextEdits: edits || undefined,
        command: { id: 'editor.action.triggerSuggest', title: '' }
      });
    }
    return { suggestions: out };
  }

  var MEMBER_VAR = /^\s*(public\s+|private\s+)?var\s+([A-Za-z_]\w*)\s*:\s*([^=;]+)/;

  function classMembers(info, name, range) {
    // クラスの中で書かれた func と var を、そのまま候補にする。
    // 親から受け継いだものは public だけ（spec/types/class.md）
    var out = [], seen = {};
    var cls = classOf(info, name);
    if (!cls) return out;
    var chain = [cls], up = ancestors(info, cls);
    for (var a = 0; a < up.length; a++) if (up[a].cls) chain.push(up[a].cls);

    for (var c = 0; c < chain.length; c++) {
      var owner = chain[c], from = c > 0;
      var ms = methodsIn(info, owner);
      for (var i = 0; i < ms.length; i++) {
        var m = ms[i];
        if (m.name === 'init' || seen[m.name] || (from && !m.pub)) continue;
        seen[m.name] = true;
        out.push(item({ name: m.name, params: m.params, ret: m.ret || 'void',
                        sig: m.name + '(' + m.params.join(', ') + ')' + (m.ret ? ' -> ' + m.ret : ''),
                        doc: from ? owner.name + ' から受け継いだメソッド' : name + ' のメソッド' },
                      range, 'method', 'a'));
      }
      var lines = linesOf(info);
      var end = classEnd(info, owner);
      for (var j = owner.line; j < end && j < lines.length; j++) {
        var vm = MEMBER_VAR.exec(lines[j]);
        if (!vm || seen[vm[2]] || (from && !/public/.test(vm[1] || ''))) continue;
        seen[vm[2]] = true;
        out.push({ label: vm[2], kind: kindFor('field'),
                   detail: vm[3].trim() + (from ? '（' + owner.name + '）' : ''),
                   insertText: vm[2], range: range, sortText: 'a' + vm[2] });
      }
    }
    return out;
  }

  // ============================================================ 親の関数の雛形
  // 親をたどる。1番目が実装を持つ親、2番目以降はインタフェース（spec/types/class.md）
  function ancestors(info, cls, seen) {
    seen = seen || {};
    seen[cls.name] = true;
    var out = [], bases = splitTop(cls.base || '');
    for (var i = 0; i < bases.length; i++) {
      var name = head(bases[i]);
      if (!name || seen[name]) continue;
      seen[name] = true;
      var parent = classOf(info, name);
      out.push({ name: name, type: bases[i].trim(), cls: parent });
      if (parent) out = out.concat(ancestors(info, parent, seen));
    }
    return out;
  }

  // 親から受け継いで上書きできるメソッド。もう書いたものは外す
  function inherited(info, cls) {
    var own = {}, mine = methodsIn(info, cls);
    for (var i = 0; i < mine.length; i++) own[mine[i].name] = true;
    var chain = ancestors(info, cls), out = [], seen = {};
    for (var b = 0; b < chain.length; b++) {
      var base = chain[b];
      var list = base.cls ? methodsIn(info, base.cls) : (api.methods[base.name] || []);
      for (var j = 0; j < list.length; j++) {
        var m = list[j];
        if (base.cls && !m.virtual) continue;        // 画面のクラスは virtual だけ上書きできる
        if (m.name === 'init' || own[m.name] || seen[m.name]) continue;
        seen[m.name] = true;
        out.push({ name: m.name, params: m.params || [], ret: m.ret || '', doc: m.doc || '',
                   // 宣言だけの型（Comparable など）はインタフェースで、中身はすべて public な純粋仮想
                   pure: base.cls ? m.pure : true, pub: base.cls ? m.pub : true,
                   from: base.name, base: base.type });
      }
    }
    return out;
  }

  var ZERO = { int: '0', float: '0.0', bool: 'false', string: '""', bytes: 'b""' };

  // 戻り値の型に合う、とりあえずの値
  function zeroOf(type) {
    var t = (type || '').trim();
    if (!t || t === 'void') return '';
    if (t.slice(-1) === '?') return 'none';
    var h = head(t);
    if (h === 'list') return '[]';
    if (h === 'map') return '{}';
    return ZERO[h] || '';
  }

  function paramName(p) {
    var m = /^\s*(?:ref\s+)?([A-Za-z_]\w*)\s*:/.exec(p || '');
    return m ? m[1] : (p || '').trim();
  }

  // 親の書き方を子に合わせる。インタフェースの This と、総称の T を置き換える
  function forChild(text, baseType, child) {
    return substitute((text || '').replace(/\bThis\b/g, child), baseType);
  }

  // override の雛形。純粋仮想は空の本体、実装がある親は super を呼ぶところまで書く。
  // 親の public は引き継ぐ（落とすと、その関数は子の中からしか呼べなくなる）
  function overrideStub(m, child, typed) {
    var params = [];
    for (var i = 0; i < m.params.length; i++) params.push(forChild(m.params[i], m.base, child));
    var ret = forChild(m.ret, m.base, child);
    var core = 'func ' + m.name + '(' + params.join(', ') + ')' + (ret ? ' -> ' + ret : '');
    var value = ret && ret !== 'void';
    var tail = '';
    if (!m.pure) {
      var call = 'super.' + m.name + '(' + m.params.map(paramName).join(', ') + ')';
      tail = '\n\t' + (value ? 'return ' + call + ';' : call + ';');
    } else if (value && zeroOf(ret)) {
      tail = '\n\treturn ' + zeroOf(ret) + ';';
    }
    var mods = (m.pub && !typed.vis ? 'public ' : '') + (typed.override ? '' : 'override ');
    return { sig: (m.pub ? 'public ' : '') + 'override ' + core,
             text: mods + core + ' {\n\t$0' + tail + '\n}' };
  }

  // クラスの中で出す、親から受け継いだ関数の雛形
  function overrideItems(info, cls, range, typed) {
    var list = inherited(info, cls), out = [];
    for (var i = 0; i < list.length; i++) {
      var m = list[i];
      var stub = overrideStub(m, cls.name, typed);
      var md = ['```shark\n' + stub.sig + '\n```'];
      md.push(m.pure ? '`' + m.from + '` の純粋仮想。上書きするまで、このクラスも抽象クラスのまま'
                     : '`' + m.from + '` の virtual を上書きする');
      if (m.doc) md.push(m.doc);
      out.push({
        label: m.name, kind: kindFor('method'), detail: stub.sig, filterText: m.name,
        documentation: { value: md.join('\n\n'), isTrusted: false },
        insertText: stub.text,
        insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet,
        range: range, sortText: '0' + (m.pure ? '0' : '1') + m.name
      });
    }
    return out;
  }

  // 「クラスの本体で、修飾子まで打ったところ」なら、そのクラスと、打ってある修飾子を返す
  function memberContext(info, position, line, word) {
    var before = line.slice(0, word.startColumn - 1);
    if (!/^\s*(?:(?:public|private|override)\s+)*$/.test(before)) return null;   // virtual は新しく作る側
    var cls = classAt(info, position.lineNumber);
    if (!cls || !cls.base || !atMemberLevel(info, cls, position.lineNumber)) return null;
    return { cls: cls,
             typed: { override: /\boverride\b/.test(before), vis: /\b(?:public|private)\b/.test(before) } };
  }

  function memberList(mod) {
    var m = api.modules[mod];
    if (!m) return '';
    var names = [];
    for (var i = 0; i < m.members.length && i < 12; i++) names.push(m.members[i].name);
    return '`std.' + mod + '`\n\n' + names.join(', ') + (m.members.length > 12 ? ' …' : '');
  }

  // ============================================================ 説明（hover）
  function hover(model, position) {
    var info = scanModel(model);
    var w = model.getWordAtPosition(position);
    if (!w) return null;
    var line = model.getLineContent(position.lineNumber);
    var before = line.slice(0, w.startColumn - 1);
    var range = new monaco.Range(position.lineNumber, w.startColumn, position.lineNumber, w.endColumn);
    var name = w.word;
    var e = null, extra = '';

    var dot = /([A-Za-z_]\w*(?:\s*\([^()]*\))?)\s*\.\s*$/.exec(before);
    if (dot) {
      var recvText = dot[1].trim();
      var bare = /^[A-Za-z_]\w*$/.test(recvText) ? recvText : '';
      var mod = bare ? moduleAlias(info, bare) : null;
      if (mod) {
        e = memberOf(mod, name);
      } else {
        var t = bare ? typeOfVar(info, bare, position.lineNumber) : typeOfExpr(info, recvText, position.lineNumber);
        var me = methodOf(t, name);
        if (me) {
          e = { name: name, sig: me.name + '(' + (me.params || []).join(', ') + ')' +
                (me.ret ? ' -> ' + substitute(me.ret, t) : ''), doc: me.doc,
                args: me.args, example: me.example };
          extra = t ? '\n\n受け手: `' + t + '`' : '';
        }
      }
    }
    if (!e) {
      e = builtinOf(name);
      if (!e) {
        var uf = funcOf(info, name);
        if (uf) e = { name: name, sig: uf.name + '(' + uf.params.join(', ') + ') -> ' + uf.ret,
                      doc: 'この画面で定義した関数' };
      }
      if (!e && classOf(info, name)) {
        var cl = classOf(info, name);
        e = { name: name, sig: 'class ' + name + (cl.base ? ' : ' + cl.base : ''), doc: 'この画面で定義したクラス' };
      }
      if (!e && api.modules[name]) e = { name: name, sig: 'std.' + name, doc: memberList(name) };
      if (!e && KEYWORD_DOC[name]) e = { name: name, sig: name, doc: KEYWORD_DOC[name] };
      if (!e && TYPES.indexOf(name) >= 0) e = { name: name, sig: name, doc: '組み込みの型' };
      if (!e) {
        var t2 = typeOfVar(info, name, position.lineNumber);
        if (t2) e = { name: name, sig: name + ': ' + t2, doc: '' };
      }
    }
    if (!e) return null;
    var md = [{ value: '```shark\n' + (e.sig || e.name) + '\n```' }];
    if (e.doc) md.push({ value: e.doc + extra });
    else if (extra) md.push({ value: extra });
    var rows = argRows(e);
    if (rows) md.push({ value: rows });
    if (e.example) md.push({ value: '```shark\n' + e.example + '\n```' });
    if (e.overloads && e.overloads.length) md.push({ value: 'ほかの書き方:\n```shark\n' + e.overloads.join('\n') + '\n```' });
    return { range: range, contents: md };
  }

  // ============================================================ 引数の案内
  function signatureHelp(model, position) {
    var info = scanModel(model);
    var text = model.getValueInRange({
      startLineNumber: Math.max(1, position.lineNumber - 30), startColumn: 1,
      endLineNumber: position.lineNumber, endColumn: position.column
    });
    var depth = 0, argIndex = 0, i = text.length - 1, callEnd = -1;
    for (; i >= 0; i--) {
      var c = text[i];
      if (c === ')') depth++;
      else if (c === '(') {
        if (depth === 0) { callEnd = i; break; }
        depth--;
      } else if (c === ',' && depth === 0) argIndex++;
      else if (c === ';' || c === '}') return null;
    }
    if (callEnd < 0) return null;
    var head2 = /([A-Za-z_]\w*)\s*(?:\.\s*([A-Za-z_]\w*)\s*)?$/.exec(text.slice(0, callEnd));
    if (!head2) return null;
    var owner = head2[2] ? head2[1] : '';
    var name = head2[2] || head2[1];
    var e = null;
    if (owner) {
      var mod = moduleAlias(info, owner);
      if (mod) e = memberOf(mod, name);
      if (!e) {
        var t = typeOfVar(info, owner, position.lineNumber);
        var me = methodOf(t, name);
        if (me) e = { name: name, sig: me.name + '(' + (me.params || []).join(', ') + ')' +
                      (me.ret ? ' -> ' + substitute(me.ret, t) : ''), params: me.params, doc: me.doc };
      }
    } else {
      e = builtinOf(name);
      if (!e) {
        var uf = funcOf(info, name);
        if (uf) e = { name: name, params: uf.params, doc: 'この画面で定義した関数',
                      sig: uf.name + '(' + uf.params.join(', ') + ') -> ' + uf.ret };
      }
      if (!e) {
        var cl = classOf(info, name);
        if (cl) {
          var init = null;
          var lines = linesOf(info);
          for (var k = cl.line; k < lines.length; k++) {
            var im2 = /^\s*func\s+init\s*\(([^)]*)\)/.exec(lines[k]);
            if (im2) { init = splitTop(im2[1]); break; }
          }
          if (init) e = { name: name, params: init, sig: name + '(' + init.join(', ') + ')',
                          doc: cl.name + ' を作る' };
        }
      }
    }
    if (!e) return null;
    var sigs = [{
      label: e.sig || e.name,
      documentation: e.example ? { value: (e.doc ? e.doc + '\n\n' : '') +
                                          '```shark\n' + e.example + '\n```' } : (e.doc || ''),
      parameters: (e.params || []).map(function (p, i) {
        var a = e.args && e.args[i];
        return a && a[2] ? { label: p, documentation: a[2] } : { label: p };
      })
    }];
    if (e.overloads) {
      for (var o = 0; o < e.overloads.length; o++) {
        sigs.push({ label: e.overloads[o], parameters: paramsOf(e.overloads[o]).map(function (p) { return { label: p }; }) });
      }
    }
    return {
      value: { signatures: sigs, activeSignature: 0, activeParameter: Math.min(argIndex, Math.max(0, (e.params || []).length - 1)) },
      dispose: function () {}
    };
  }

  function paramsOf(sig) {
    var m = /\(([^)]*)\)/.exec(sig);
    return m ? splitTop(m[1]) : [];
  }

  // ============================================================ 取り付ける
  function install(m, apiData) {
    monaco = m;
    if (apiData) api = apiData;

    monaco.languages.register({ id: ID, extensions: ['.shk'], aliases: ['Shark', 'shark'] });

    monaco.languages.setLanguageConfiguration(ID, {
      comments: { lineComment: '//', blockComment: ['/*', '*/'] },
      brackets: [['{', '}'], ['[', ']'], ['(', ')']],
      autoClosingPairs: [
        { open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' },
        { open: '"', close: '"', notIn: ['string', 'comment'] }
      ],
      surroundingPairs: [
        { open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' }, { open: '"', close: '"' }
      ],
      indentationRules: {
        increaseIndentPattern: /^((?!\/\/).)*(\{[^}"']*|\([^)"']*)$/,
        decreaseIndentPattern: /^\s*[})]/
      }
    });

    monaco.languages.setMonarchTokensProvider(ID, {
      defaultToken: '',
      keywords: KEYWORDS,
      typeKeywords: TYPES,
      builtins: api.builtins.map(function (b) { return b.name; }),
      operators: ['=', '==', '!=', '<', '>', '<=', '>=', '+', '-', '*', '/', '%', '&&', '||', '!',
                  '+=', '-=', '*=', '/=', '%=', '??', '?.', '->', '?',
                  '&', '|', '^', '~', '<<', '>>', '**', '&=', '|=', '^=', '<<=', '>>='],
      symbols: /[=><!~?:&|^+\-*\/%]+/,
      tokenizer: {
        root: [
          [/[a-zA-Z_]\w*(?=\s*\()/, {
            cases: { '@keywords': 'keyword', '@typeKeywords': 'type', '@builtins': 'function.builtin',
                     '@default': 'function' }
          }],
          [/[a-zA-Z_]\w*/, {
            cases: { '@keywords': 'keyword', '@typeKeywords': 'type', '@builtins': 'function.builtin',
                     '@default': 'identifier' }
          }],
          { include: '@whitespace' },
          [/[{}()\[\]]/, '@brackets'],
          [/f"/, { token: 'string.quote', next: '@fstring' }],
          [/b"/, { token: 'string.quote', next: '@string' }],
          [/"/, { token: 'string.quote', next: '@string' }],
          [/\d+\.\d+([eE][\-+]?\d+)?/, 'number.float'],
          [/0[xX][0-9a-fA-F_]+/, 'number.hex'],
          [/0[bB][01_]+/, 'number'],
          [/\d[\d_]*/, 'number'],
          [/@symbols/, { cases: { '@operators': 'operator', '@default': '' } }],
          [/[;,.]/, 'delimiter']
        ],
        whitespace: [
          [/[ \t\r\n]+/, ''],
          [/\/\/.*$/, 'comment'],
          [/\/\*/, 'comment', '@comment']
        ],
        comment: [                       // 入れ子にできる（spec/syntax.md）
          [/[^\/*]+/, 'comment'],
          [/\/\*/, 'comment', '@push'],
          [/\*\//, 'comment', '@pop'],
          [/[\/*]/, 'comment']
        ],
        string: [
          [/[^\\"]+/, 'string'],
          [/\\u\{[0-9a-fA-F]+\}/, 'string.escape'],
          [/\\./, 'string.escape'],
          [/"/, { token: 'string.quote', next: '@pop' }]
        ],
        fstring: [                       // f"{name} が泳いでいる"
          [/\{/, { token: 'interp.bracket', next: '@interp' }],
          [/[^\\"{]+/, 'string'],
          [/\\./, 'string.escape'],
          [/"/, { token: 'string.quote', next: '@pop' }]
        ],
        interp: [
          [/\}/, { token: 'interp.bracket', next: '@pop' }],
          [/:[^}"]*/, 'string.escape'],  // 書式指定 f"{v:>8}"
          [/[a-zA-Z_]\w*/, {
            cases: { '@keywords': 'keyword', '@typeKeywords': 'type', '@default': 'interp' }
          }],
          [/\d[\d_.]*/, 'number'],
          [/[^}"]/, 'interp']
        ]
      }
    });

    defineThemes();

    monaco.languages.registerCompletionItemProvider(ID, {
      triggerCharacters: ['.', ' '],
      provideCompletionItems: function (model, position) { return completions(model, position); }
    });
    monaco.languages.registerHoverProvider(ID, {
      provideHover: function (model, position) { return hover(model, position); }
    });
    monaco.languages.registerSignatureHelpProvider(ID, {
      signatureHelpTriggerCharacters: ['(', ','],
      signatureHelpRetriggerCharacters: [')'],
      provideSignatureHelp: function (model, position) { return signatureHelp(model, position); }
    });
    monaco.languages.registerDefinitionProvider(ID, {
      provideDefinition: function (model, position) {
        var info = scanModel(model);
        var w = model.getWordAtPosition(position);
        if (!w) return null;
        var t = funcOf(info, w.word) || classOf(info, w.word);
        if (!t) return null;
        return [{ uri: model.uri, range: new monaco.Range(t.line, t.col || 1, t.line, (t.col || 1) + w.word.length) }];
      }
    });
    monaco.languages.registerDocumentSymbolProvider(ID, {
      provideDocumentSymbols: function (model) {
        var info = scanModel(model);
        var out = [];
        var K = monaco.languages.SymbolKind;
        for (var i = 0; i < info.classes.length; i++) {
          out.push(symbol(info.classes[i].name, 'class', K.Class, info.classes[i].line));
        }
        for (var f = 0; f < info.funcs.length; f++) {
          var fn = info.funcs[f];
          out.push(symbol(fn.name, '(' + fn.params.join(', ') + ') -> ' + fn.ret,
                          fn.name.indexOf('test_') === 0 ? K.Event : K.Function, fn.line));
        }
        return out;
      }
    });

    function symbol(name, detail, kind, line) {
      var r = new monaco.Range(line, 1, line, 1);
      return { name: name, detail: detail, kind: kind, tags: [], range: r, selectionRange: r };
    }
  }

  function defineThemes() {
    var light = [
      { token: 'comment', foreground: '6e7d89', fontStyle: 'italic' },
      { token: 'keyword', foreground: '8250df' },
      { token: 'type', foreground: '0550ae' },
      { token: 'string', foreground: '0a7a3f' },
      { token: 'string.quote', foreground: '0a7a3f' },
      { token: 'string.escape', foreground: '0e7490' },
      { token: 'number', foreground: 'a04a00' },
      { token: 'number.float', foreground: 'a04a00' },
      { token: 'number.hex', foreground: 'a04a00' },
      { token: 'function', foreground: '953800' },
      { token: 'function.builtin', foreground: '953800', fontStyle: 'bold' },
      { token: 'interp', foreground: '0e7490' },
      { token: 'interp.bracket', foreground: '0e7490', fontStyle: 'bold' },
      { token: 'operator', foreground: '16242f' },
      { token: 'identifier', foreground: '16242f' }
    ];
    var dark = [
      { token: 'comment', foreground: '7d8f9c', fontStyle: 'italic' },
      { token: 'keyword', foreground: 'd2a8ff' },
      { token: 'type', foreground: '79c0ff' },
      { token: 'string', foreground: '7ee787' },
      { token: 'string.quote', foreground: '7ee787' },
      { token: 'string.escape', foreground: '56d4dd' },
      { token: 'number', foreground: 'ffa657' },
      { token: 'number.float', foreground: 'ffa657' },
      { token: 'number.hex', foreground: 'ffa657' },
      { token: 'function', foreground: 'ffc08a' },
      { token: 'function.builtin', foreground: 'ffc08a', fontStyle: 'bold' },
      { token: 'interp', foreground: '56d4dd' },
      { token: 'interp.bracket', foreground: '56d4dd', fontStyle: 'bold' },
      { token: 'operator', foreground: 'dce8f1' },
      { token: 'identifier', foreground: 'dce8f1' }
    ];
    monaco.editor.defineTheme('shark-light', {
      base: 'vs', inherit: true, rules: light,
      colors: { 'editor.background': '#ffffff', 'editorLineNumber.foreground': '#96a8b6',
                'editor.lineHighlightBackground': '#f2f7fa', 'editorIndentGuide.background1': '#e6eef4' }
    });
    monaco.editor.defineTheme('shark-dark', {
      base: 'vs-dark', inherit: true, rules: dark,
      colors: { 'editor.background': '#111d27', 'editorLineNumber.foreground': '#546878',
                'editor.lineHighlightBackground': '#16242f', 'editorIndentGuide.background1': '#1e2f3b' }
    });
  }

  return { install: install, id: ID, scan: scan };
})();

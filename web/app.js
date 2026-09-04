// app.js — プレイグラウンドの画面まわり
//
// 処理系そのものは web/shark_web.cpp を WebAssembly にしたもの（shark.js / shark.wasm）。
// ここがしているのは、ホストの仕事だけ:
//   ソースを渡す → 少しずつ進める → 出力と診断を受け取って画面に出す
// （spec/runtime/embedding.md）
//
// 右がわは端末と同じにしてある。
//   ・出力も、打った文字も、1本の流れに並ぶ
//   ・input() は打たれるまで待つ。Ctrl + D で終端（none）
//   ・出す形（診断・panic・テストの結果）は shark コマンド（frontend/main.cpp）と同じ
//
// 書くところは Monaco Editor。Shark の色分けと入力補完は web/lang.js にある。
(function () {
  'use strict';

  var FILE = 'playground.shk';
  var MAX_OUT_NODES = 3000;
  var CHECK_DELAY = 400;      // 打つ手が止まってから、型検査を走らせるまで
  var PS1 = '$ ';             // 端末の促し
  var HISTORY_MAX = 100;

  var $ = function (id) { return document.getElementById(id); };
  var decoder = new TextDecoder();

  var M = null;         // WebAssembly の中身
  var api = null;       // shk_* の呼び出し口
  var monaco = null;
  var editor = null;
  var pending = '';     // Monaco ができるまでの、書きかけの中身
  var running = false;
  var mode = 'run';
  var aborting = false;
  var waiting = false;  // input() が打たれるのを待っている
  var startedAt = 0;
  var rafId = 0;
  var budget = 200000;  // 1回に進める命令の数。時間を見て増減させる（結果は変わらない）
  var checkTimer = 0;
  var color = true;     // --no-color で消せる

  function code() { return editor ? editor.getValue() : pending; }
  function setCode(text) {
    pending = text;
    if (editor) editor.setValue(text);
  }

  // 走らせるときの設定。上の帯の選びが既定で、打ったコマンドの --lang などが上書きする
  function flags() {
    return {
      lang: $('sel-lang').value,
      memory: Number($('sel-memory').value),
      strict: false,
      color: true
    };
  }

  // ================================================================ ターミナル
  // 端末と同じ1本の流れ。log が済んだぶん、line がいま打っている行。
  // 打つ文字は見えない textarea（term-key）が受ける。かな漢字変換のためで、
  // 変換の窓が正しい場所に出るように、印（カーソル）へ位置を合わせておく。
  var term = $('term');
  var log = $('term-log');
  var ps1 = $('term-ps1'), lineHead = $('term-pre'), caret = $('term-caret'), lineTail = $('term-post');
  var key = $('term-key');
  var hist = [], histPos = 0, histDraft = '';
  var atCol0 = true;    // 直前に出したものが改行で終わっているか

  function nearBottom() {
    return term.scrollHeight - term.scrollTop - term.clientHeight < 40;
  }
  function toBottom() { term.scrollTop = term.scrollHeight; }

  // 流れに出す。cls は色付けの種類（端末の色にあたる）
  function append(text, cls) {
    if (!text) return;
    var stick = nearBottom();
    var node;
    if (cls && color) {
      node = document.createElement('span');
      node.className = cls;
      node.textContent = text;
    } else {
      node = document.createTextNode(text);
    }
    log.appendChild(node);
    while (log.childNodes.length > MAX_OUT_NODES) log.removeChild(log.firstChild);
    atCol0 = text.charAt(text.length - 1) === '\n';
    if (stick) toBottom();
  }
  function newlineIfNeeded() { if (!atCol0) append('\n'); }

  function clearLog() {
    log.textContent = '';
    atCol0 = true;
    renderLine();
  }

  // 促しを出す（走っていないときは line の頭に $ が出るので、行を改めるだけ）
  function prompt() {
    newlineIfNeeded();
    renderLine();
  }

  function renderLine() {
    ps1.textContent = running ? '' : PS1;
    var v = key.value;
    var pos = key.selectionStart;
    if (pos == null || pos < 0 || pos > v.length) pos = v.length;
    var head = v.slice(0, pos), tail = v.slice(pos);
    var ch = tail.length ? Array.from(tail)[0] : '';
    lineHead.textContent = head;
    caret.textContent = ch || ' ';
    lineTail.textContent = ch ? tail.slice(ch.length) : '';
    placeKey();
  }

  // 見えない textarea を印の位置へ。変換中の候補がそこに出る
  function placeKey() {
    var a = caret.getBoundingClientRect();
    var b = term.getBoundingClientRect();
    key.style.left = (a.left - b.left + term.scrollLeft) + 'px';
    key.style.top = (a.top - b.top + term.scrollTop) + 'px';
  }

  function focusTerm() {
    key.focus({ preventScroll: true });
  }

  // 1行ぶんを送る。走っていればプログラムの input() へ、そうでなければコマンドとして読む
  function commit() {
    var text = key.value;
    key.value = '';
    append((running ? '' : PS1) + text + '\n');   // 端末と同じで、打った行はそのまま流れに残る
    renderLine();
    toBottom();

    if (running) {
      api.pushInput(text + '\n');
      return;
    }
    if (text.trim()) {
      hist.push(text);
      if (hist.length > HISTORY_MAX) hist.shift();
    }
    histPos = hist.length;
    histDraft = '';
    shell(text);
    if (!running) prompt();
  }

  function interrupt() {                       // Ctrl + C
    append((running ? '' : PS1) + key.value + '^C');   // 端末はここで改行しない（次の促しで改まる）
    key.value = '';
    if (running) stop();
    else prompt();
  }

  // Ctrl + D（入力の終わり）。端末は ^D をいちど出してすぐ消すので、そのとおりにする
  function endInput() {
    if (!running || key.value.length) return;  // 端末と同じで、書きかけの行があるときは効かない
    var was = atCol0;
    var mark = document.createElement('span');
    mark.textContent = '^D';
    log.appendChild(mark);
    atCol0 = false;
    setTimeout(function () {
      var last = log.lastChild === mark;
      if (mark.parentNode) mark.parentNode.removeChild(mark);
      if (last) atCol0 = was;
    }, 120);
    api.pushEof();
  }

  function recall(delta) {                     // ↑ ↓ で打った覚えをたどる
    if (!hist.length) return;
    if (histPos === hist.length) histDraft = key.value;
    var p = histPos + delta;
    if (p < 0) p = 0;
    if (p > hist.length) p = hist.length;
    histPos = p;
    key.value = p === hist.length ? histDraft : hist[p];
    key.selectionStart = key.selectionEnd = key.value.length;
    renderLine();
  }

  key.addEventListener('keydown', function (e) {
    if (e.isComposing || e.keyCode === 229) return;   // かな漢字変換の最中は触らない
    if (e.ctrlKey && !e.altKey && !e.metaKey) {
      var k = e.key.toLowerCase();
      if (k === 'c') { e.preventDefault(); interrupt(); return; }
      if (k === 'd') { e.preventDefault(); endInput(); return; }
      if (k === 'l') { e.preventDefault(); clearLog(); return; }
      if (k === 'u') { e.preventDefault(); key.value = ''; renderLine(); return; }
    }
    if (e.key === 'Enter' && !e.ctrlKey && !e.metaKey) { e.preventDefault(); commit(); return; }
    if (e.key === 'Escape') { e.preventDefault(); if (editor) editor.focus(); return; }
    if (!running && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
      e.preventDefault();
      recall(e.key === 'ArrowUp' ? -1 : 1);
      return;
    }
    setTimeout(renderLine, 0);   // 打った文字や選び位置が動いたあとに映す
  });

  key.addEventListener('input', function () {
    var v = key.value;
    if (v.indexOf('\n') >= 0) {          // 貼り付けた複数行は、1行ずつ送る
      var parts = v.split('\n');
      for (var i = 0; i < parts.length - 1; i++) {
        key.value = parts[i];
        commit();
      }
      key.value = parts[parts.length - 1];
    }
    renderLine();
  });
  key.addEventListener('focus', function () {
    term.classList.add('on');
    term.classList.remove('off');
    renderLine();
  });
  key.addEventListener('blur', function () {
    term.classList.remove('on');
    term.classList.add('off');
  });
  term.addEventListener('mouseup', function () {
    var sel = window.getSelection && window.getSelection();
    if (sel && String(sel).length) return;   // 文字を選んでいるときは邪魔しない
    focusTerm();
  });
  term.classList.add('off');

  function drain() {
    var len = M._shk_out_len();
    if (!len) return;
    var ptr = M._shk_out_ptr();
    var bytes = M.HEAPU8.slice(ptr, ptr + len);   // 伸びると位置が変わるので、その場で写す
    M._shk_out_clear();
    append(decoder.decode(bytes));
  }

  // ================================================================ 診断
  // 桁は「文字の数」で来る（spec/runtime/diagnostics.md）。
  // Monaco は UTF-16 の数で数えるので、絵文字などのために直す。
  function toColumn(line, col) {
    if (!editor) return col;
    var text = editor.getModel().getLineContent(line) || '';
    var chars = Array.from(text);
    return chars.slice(0, Math.max(0, col - 1)).join('').length + 1;
  }
  function spanEnd(line, col, len) {
    if (!editor) return col + Math.max(1, len);
    var text = editor.getModel().getLineContent(line) || '';
    var chars = Array.from(text);
    var start = Math.max(0, col - 1);
    var body = chars.slice(start, start + Math.max(1, len)).join('');
    return toColumn(line, col) + (body.length || 1);
  }

  function setMarkers(diags) {
    if (!editor || !monaco) return;
    var model = editor.getModel();
    var marks = [];
    for (var i = 0; i < diags.length; i++) {
      var d = diags[i];
      if (!d.line) continue;
      var msg = d.message;
      if (d.spans.length && d.spans[0].label) msg += '\n' + d.spans[0].label;
      for (var h = 0; h < d.help.length; h++) msg += '\n直し方: ' + d.help[h];
      marks.push({
        severity: d.severity === 'error' ? monaco.MarkerSeverity.Error : monaco.MarkerSeverity.Warning,
        message: msg,
        code: d.code,
        source: 'shark',
        startLineNumber: d.line,
        startColumn: toColumn(d.line, d.col || 1),
        endLineNumber: d.line,
        endColumn: spanEnd(d.line, d.col || 1, d.len || 1)
      });
    }
    monaco.editor.setModelMarkers(model, 'shark', marks);
  }

  // 端末と同じ整形（コアが作った text をそのまま出す）
  function printDiags(diags) {
    for (var i = 0; i < diags.length; i++) {
      append(diags[i].text + '\n', diags[i].severity === 'error' ? 'err' : 'warn');
    }
  }

  function showDiagnostics(diags) {
    var box = $('diag');
    var badge = $('diag-count');
    box.innerHTML = '';
    var errs = 0;
    for (var i = 0; i < diags.length; i++) if (diags[i].severity === 'error') errs++;
    setMarkers(diags);

    if (!diags.length) {
      box.innerHTML = '<p class="empty">誤りはありません。</p>';
      badge.className = 'badge';
      badge.textContent = '';
      return;
    }
    badge.className = 'badge on' + (errs === 0 ? ' warn-only' : '');
    badge.textContent = String(diags.length);

    diags.forEach(function (d) {
      var item = document.createElement('div');
      item.className = 'item' + (d.severity === 'error' ? '' : ' warn');
      var top = document.createElement('div');
      top.className = 'top';
      top.innerHTML = '<span class="code">' + esc(d.code) + '</span>' +
        '<span class="msg">' + esc(d.message) + '</span>' +
        '<span class="where">' + esc(d.file) + ':' + d.line + '</span>';
      item.appendChild(top);
      if (d.spans.length && d.spans[0].label) {
        var lab = document.createElement('div');
        lab.className = 'hint';
        lab.textContent = d.spans[0].label;
        item.appendChild(lab);
      }
      if (d.help.length) {
        var ul = document.createElement('ul');
        ul.className = 'hint';
        d.help.forEach(function (h) {
          var li = document.createElement('li');
          li.textContent = h;
          ul.appendChild(li);
        });
        item.appendChild(ul);
      }
      item.addEventListener('click', function () { jumpTo(d.line, d.col); });
      box.appendChild(item);
    });
  }

  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function jumpTo(line, col) {
    if (!editor) return;
    editor.revealLineInCenter(line);
    editor.setPosition({ lineNumber: line, column: toColumn(line, col || 1) });
    editor.focus();
  }

  // ================================================================ 走らせる
  function setState(text, cls) {
    var s = $('st-state');
    s.textContent = text;
    s.className = 'state' + (cls ? ' ' + cls : '');
  }
  function bytes(n) {
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1048576).toFixed(1) + ' MB';
  }
  function stats() {
    $('st-time').textContent = ((performance.now() - startedAt) / 1000).toFixed(2) + ' 秒';
    $('st-memory').textContent = 'メモリ ' + bytes(api.memoryUsed()) + ' / ' + bytes(api.memoryLimit());
  }

  function compile(fl) {
    api.config(fl.memory, fl.lang === 'en' ? 1 : 0, fl.strict ? 1 : 0);
    var errs = api.load(FILE, code());
    var diags = JSON.parse(api.diagnostics());
    showDiagnostics(diags);
    return { errs: errs, diags: diags };
  }

  // 打つ手が止まったら、本物の型検査を走らせて波線を引く（端末には出さない）
  function scheduleCheck() {
    if (checkTimer) clearTimeout(checkTimer);
    checkTimer = setTimeout(function () {
      checkTimer = 0;
      if (!api || running) return;
      compile(flags());
    }, CHECK_DELAY);
  }

  // ボタンから走らせるときは、同じことをする shark コマンドを流れに出す
  function echoCommand(which, fl, file) {
    var s = 'shark ' + which + ' ' + file;
    if (fl.lang === 'en') s += ' --lang en';
    if (fl.memory !== 256) s += ' --memory ' + fl.memory;
    if (fl.strict) s += ' --strict';
    newlineIfNeeded();
    append(PS1, 'ps1');
    append(s + '\n');
  }

  // which は run / check / test。opts.echo で、打っていないコマンドも流れに残す
  function start(which, opts) {
    opts = opts || {};
    if (!api || running) return;
    var fl = opts.flags || flags();
    var file = opts.file || FILE;
    color = fl.color;
    showTab('term');
    if (opts.echo) echoCommand(which, fl, file);
    if (file !== FILE) {   // 開けるのは、左に書いているものだけ
      append('ファイルを開けません: ' + file + '\n', 'err');
      setState('ファイルがありません', 'error');
      return;
    }

    mode = which;
    aborting = false;
    waiting = false;
    if (checkTimer) { clearTimeout(checkTimer); checkTimer = 0; }
    $('st-exit').textContent = '';

    var r = compile(fl);
    printDiags(r.diags);
    if (r.errs > 0) {
      append(r.errs + ' 件の誤りがあります\n', 'err');
      setState(r.errs + ' 件の誤りがあります', 'error');
      return;
    }
    if (which === 'check') {
      if (!r.diags.length) append('問題ありません\n');
      setState(r.diags.length ? '警告 ' + r.diags.length + ' 件' : '問題ありません', 'done');
      return;
    }

    if (which === 'test') {
      var found = api.startTest();
      if (found <= 0) {
        append('test_ で始まる関数がありません\n');
        setState('テストなし', 'done');
        return;
      }
      append(file + '\n');
    } else {
      if (!api.hasEntry()) {
        append('実行するものがありません\n' +
               '  直し方: func main() -> int { } を書くか、文をそのまま並べます\n', 'err');
        setState('実行できません', 'error');
        return;
      }
      api.startRun();
    }

    running = true;
    budget = 200000;
    startedAt = performance.now();
    setState(which === 'test' ? 'テスト中…' : '実行中…', 'running');
    buttons();
    renderLine();
    rafId = requestAnimationFrame(tick);
  }

  // 1回の描画につき1度だけ進める。ここが「少しずつ動かす」の実物
  function tick() {
    var t0 = performance.now();
    var status = api.pump(budget);
    var spent = performance.now() - t0;
    drain();
    if (uiScreen) screenSize();

    var wait = api.waitingInput() === 1;   // input() が打たれるのを待っている
    if (wait !== waiting) {
      waiting = wait;
      setState(wait ? '入力を待っています' : (mode === 'test' ? 'テスト中…' : '実行中…'), 'running');
      if (wait) { showTab('term'); toBottom(); focusTerm(); }
    }
    // 待っている間は進んでいないので、進める量はそのままにする
    if (!wait) {
      // 画面が止まって見えない範囲で、進める量を寄せていく
      if (spent < 6 && budget < 8000000) budget = Math.round(budget * 1.5);
      else if (spent > 14 && budget > 20000) budget = Math.round(budget / 1.5);
    }

    stats();
    if (status === 0) { rafId = requestAnimationFrame(tick); return; }
    finish(status);
  }

  function finish(status) {
    running = false;
    waiting = false;
    rafId = 0;
    drain();
    // 面が開いたまま終わったら閉じる（`shark` コマンドならプロセスごと消えるところ）。
    // 残しておくと、閉じるボタンを押しても受け取る側がもう居ない窓が居座る
    if (uiScreen) api.uiClose();
    stats();
    buttons();

    if (mode === 'test') {
      var passed = api.testPassed(), total = api.testTotal();
      if (status === 2 && !aborting) {
        showPanic();
        setState('止まりました', 'error');
      } else if (aborting) {
        setState('止めました', 'error');
      } else {
        append('\n' + total + ' 件中 ' + passed + ' 件成功\n');
        setState(passed === total ? 'ぜんぶ通りました' : (total - passed) + ' 件が失敗',
                 passed === total ? 'done' : 'error');
      }
      prompt();
      return;
    }

    if (status === 1) {
      var codeNum = api.exitCode();
      $('st-exit').textContent = '終了コード ' + codeNum;
      setState('終わりました', 'done');
    } else if (aborting) {
      setState('止めました', 'error');
    } else {
      showPanic();
      setState('止まりました', 'error');
    }
    prompt();
  }

  // 端末（frontend/main.cpp の print_panic）と同じ形で出す
  function showPanic() {
    var e = JSON.parse(api.error());
    var text = 'panic: ' + e.message + '\n';
    if (e.line > 0) text += '  --> ' + e.file + ':' + e.line + '\n';
    if (e.trace) text += '  呼び出しの経路:\n' + e.trace;
    append(text, 'err');
    $('st-exit').textContent = '';
    if (e.line > 0 && editor && monaco) {
      // 止まった行に印を残す
      monaco.editor.setModelMarkers(editor.getModel(), 'shark-run', [{
        severity: monaco.MarkerSeverity.Error,
        message: 'ここで止まりました: ' + e.message,
        startLineNumber: e.line, startColumn: 1,
        endLineNumber: e.line, endColumn: 1000, source: 'run'
      }]);
    }
  }

  function stop() {
    if (!running) return;
    aborting = true;
    api.abort();
    if (!rafId) rafId = requestAnimationFrame(tick);
  }

  function buttons() {
    var ready = !!api && !!editor;
    $('btn-run').disabled = running || !ready;
    $('btn-check').disabled = running || !ready;
    $('btn-test').disabled = running || !ready;
    $('btn-stop').disabled = !running;
  }

  // ================================================================ 打ったコマンド
  // shark コマンド（frontend/main.cpp）と同じ言い方で受ける。
  // ここで開けるファイルは playground.shk（左に書いているもの）だけ。
  var USAGE =
    'Shark🦈  ゲーム機で動く学習用プログラミング言語\n' +
    '\n' +
    '使い方:\n' +
    '  shark run <file.shk>      実行する\n' +
    '  shark check <file.shk>    型検査だけを行う\n' +
    '  shark test [file.shk]     test_ で始まる関数を走らせる\n' +
    '  shark explain E0102       エラーの詳しい説明を出す\n' +
    '  shark modules             この処理系が持つモジュールを並べる\n' +
    '\n' +
    '選べるもの:\n' +
    '  --lang ja|en   診断の言語（既定は ja）\n' +
    '  --memory <MB>  使ってよいメモリの量。超えたら実行時エラー（既定は 256、0 で上限なし）\n' +
    '  --strict       警告もエラーとして扱う\n' +
    '  --no-color     色を付けない\n' +
    '\n' +
    'ここで開けるファイルは ' + FILE + '（左に書いているもの）だけです。\n' +
    'clear で流れを消します。Ctrl + C で止める、Ctrl + D で入力の終わりです。\n';

  function words(text) {
    var r = text.split(/\s+/);
    var out = [];
    for (var i = 0; i < r.length; i++) if (r[i].length) out.push(r[i]);
    return out;
  }

  // 選びの表示も、打ったとおりに合わせる
  function syncSettings(fl) {
    $('sel-lang').value = fl.lang;
    var sel = $('sel-memory');
    var v = String(fl.memory);
    var has = false;
    for (var i = 0; i < sel.options.length; i++) if (sel.options[i].value === v) has = true;
    if (!has) {
      var o = document.createElement('option');
      o.value = v;
      o.textContent = fl.memory + ' MB';
      sel.appendChild(o);
    }
    sel.value = v;
  }

  function shell(text) {
    var argv = words(text);
    if (!argv.length) return;
    if (argv[0] === 'shark') argv.shift();

    var fl = flags();
    var rest = [];
    for (var i = 0; i < argv.length; i++) {
      var a = argv[i];
      if (a === '--lang' && i + 1 < argv.length) { fl.lang = argv[++i] === 'en' ? 'en' : 'ja'; continue; }
      if (a === '--memory' && i + 1 < argv.length) {
        var mb = argv[++i];
        if (!/^[0-9]+$/.test(mb)) {
          append('--memory には MB の数を渡します（例: --memory 32）\n', 'err');
          return;
        }
        fl.memory = Number(mb);
        continue;
      }
      if (a === '--strict') { fl.strict = true; continue; }
      if (a === '--no-color') { fl.color = false; continue; }
      rest.push(a);
    }
    color = fl.color;
    syncSettings(fl);
    if (!rest.length) { append(USAGE); return; }

    var cmd = rest[0];
    var arg = rest.length > 1 ? rest[1].replace(/^\.\//, '') : '';

    if (cmd === 'run' || cmd === 'check' || cmd === 'test') {
      start(cmd, { flags: fl, file: arg || FILE });
      return;
    }
    if (cmd === 'explain') {
      if (!arg) { append(USAGE); return; }
      var t = api.explain(arg);
      append(t ? arg + '\n' + t + '\n' : arg + ' という番号の説明はありません\n');
      return;
    }
    if (cmd === 'modules') {
      var mods = JSON.parse(api.modules());
      append(mods.join('\n') + '\n');
      return;
    }
    if (cmd === 'version') { append('shark ' + api.version() + '\n'); return; }
    if (cmd === 'help' || cmd === '-h' || cmd === '--help') { append(USAGE); return; }
    if (cmd === 'clear') { clearLog(); return; }
    if (/\.shk$/.test(cmd)) { start('run', { flags: fl, file: cmd }); return; }

    append('知らないコマンドです: ' + cmd + '\n', 'err');
    append(USAGE);
  }

  // ================================================================ 画面の部品
  function showTab(name) {
    ['term', 'diag'].forEach(function (t) {
      $(t).classList.toggle('hidden', t !== name);
    });
    Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (b) {
      b.classList.toggle('active', b.dataset.tab === name);
    });
    if (name === 'term') renderLine();
  }
  Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (b) {
    b.addEventListener('click', function () {
      showTab(b.dataset.tab);
      if (b.dataset.tab === 'term') focusTerm();
    });
  });

  // ================================================================ 画面（std.ui）
  // 窓をこしらえるのは移植層（core/platform/screen_canvas.inc）で、ここは
  // 開いた・閉じたを受けて、下の帯に大きさを出すだけ
  var uiScreen = null;      // いま開いている面（移植層が渡してくる）
  var uiSize = '';

  window.addEventListener('shark:screen-open', function (e) {
    uiScreen = e.detail;
    uiSize = '';
    screenSize();
  });
  window.addEventListener('shark:screen-close', function () {
    uiScreen = null;
    uiSize = '';
    $('st-screen').textContent = '';
  });
  function screenSize() {
    if (!uiScreen) return;
    var s = '画面 ' + uiScreen.width + ' × ' + uiScreen.height;
    if (s === uiSize) return;
    uiSize = s;
    $('st-screen').textContent = s;
  }

  $('btn-run').addEventListener('click', function () { start('run', { echo: true }); });
  $('btn-check').addEventListener('click', function () { start('check', { echo: true }); });
  $('btn-test').addEventListener('click', function () { start('test', { echo: true }); });
  $('btn-stop').addEventListener('click', function () { interrupt(); });
  $('btn-clear').addEventListener('click', function () { clearLog(); focusTerm(); });
  $('btn-help').addEventListener('click', function () { $('help').classList.remove('hidden'); });
  $('help-close').addEventListener('click', function () { $('help').classList.add('hidden'); });
  $('help').addEventListener('click', function (e) {
    if (e.target === $('help')) $('help').classList.add('hidden');
  });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') $('help').classList.add('hidden');
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); start('run', { echo: true }); }
  });
  $('sel-lang').addEventListener('change', scheduleCheck);
  $('sel-memory').addEventListener('change', scheduleCheck);

  // 仕切りを動かす
  (function () {
    var sp = $('splitter'), main = $('main');
    var drag = false;
    sp.addEventListener('pointerdown', function (e) {
      drag = true;
      sp.setPointerCapture(e.pointerId);
    });
    sp.addEventListener('pointermove', function (e) {
      if (!drag) return;
      var narrow = window.matchMedia('(max-width: 860px)').matches;
      var box = main.getBoundingClientRect();
      var f = narrow ? (e.clientY - box.top) / box.height : (e.clientX - box.left) / box.width;
      f = Math.min(0.85, Math.max(0.15, f));
      $('pane-editor').style.flex = f + ' 1 0';
      $('pane-side').style.flex = (1 - f) + ' 1 0';
      renderLine();
    });
    sp.addEventListener('pointerup', function (e) {
      drag = false;
      sp.releasePointerCapture(e.pointerId);
    });
  })();
  window.addEventListener('resize', function () { renderLine(); });

  // ================================================================ 保存と共有
  function save() {
    try {
      localStorage.setItem('shark.code', code());
    } catch (e) { /* 使えない設定のこともある */ }
  }

  function toHash(s) {
    var b = new TextEncoder().encode(s);
    var bin = '';
    for (var i = 0; i < b.length; i++) bin += String.fromCharCode(b[i]);
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  }
  function fromHash(h) {
    var bin = atob(h.replace(/-/g, '+').replace(/_/g, '/'));
    var b = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) b[i] = bin.charCodeAt(i);
    return new TextDecoder().decode(b);
  }
  $('btn-share').addEventListener('click', function () {
    var url = location.origin + location.pathname + '#c=' + toHash(code());
    history.replaceState(null, '', url);
    var done = function () {
      showTab('term');
      newlineIfNeeded();
      append('共有できる URL をコピーしました\n', 'sys');
      prompt();
    };
    if (navigator.clipboard) navigator.clipboard.writeText(url).then(done, done);
    else done();
  });

  // お手本
  (function () {
    var sel = $('sel-example');
    var list = window.SHARK_EXAMPLES || [];
    sel.innerHTML = '<option value="">選ぶ…</option>';
    list.forEach(function (ex, i) {
      var o = document.createElement('option');
      o.value = String(i);
      o.textContent = ex.title;
      sel.appendChild(o);
    });
    sel.addEventListener('change', function () {
      var ex = list[Number(sel.value)];
      if (!ex) return;
      setCode(ex.code);
      save();
      newlineIfNeeded();
      append(ex.path + ' を読み込みました\n', 'sys');
      prompt();
      setState('待機中');
      sel.value = '';
      scheduleCheck();
    });
  })();

  // ================================================================ 起動
  var FIRST = [
    '// Shark🦈 のプレイグラウンド。Ctrl（⌘）+ Enter で実行できます',
    'func main() -> int {',
    '  var fish = ["さめ", "まぐろ", "いわし"];',
    '  for var name in fish {',
    '    print(f"{name} が泳いでいる");',
    '  }',
    '  return 0;',
    '}',
    ''
  ].join('\n');

  function restore() {
    var m = /[#&]c=([^&]+)/.exec(location.hash);
    if (m) {
      try { return fromHash(m[1]); } catch (e) { /* 壊れていたら気にしない */ }
    }
    try {
      var saved = localStorage.getItem('shark.code');
      if (saved) return saved;
    } catch (e) { /* 使えない設定のこともある */ }
    return FIRST;
  }

  pending = restore();
  setState('読み込み中…');
  $('editor').classList.add('loading');
  renderLine();

  function themeName() {
    return window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches
      ? 'shark-dark' : 'shark-light';
  }

  // --- 書くところ（Monaco Editor）---
  require.config({ paths: { vs: window.MONACO.vs } });
  require(['vs/editor/editor.main'], function (m) {
    monaco = m;
    window.SharkLang.install(monaco, window.SHARK_API);

    $('editor').classList.remove('loading');
    var mono = getComputedStyle(document.body).getPropertyValue('--mono').trim();
    editor = monaco.editor.create($('editor'), {
      value: pending,
      language: window.SharkLang.id,
      theme: themeName(),
      automaticLayout: true,
      fontFamily: mono || 'monospace',
      fontSize: 13,
      lineHeight: 22,
      tabSize: 2,
      insertSpaces: true,
      minimap: { enabled: false },
      scrollBeyondLastLine: false,
      wordWrap: 'off',
      padding: { top: 10, bottom: 10 },
      renderLineHighlight: 'all',
      fixedOverflowWidgets: true,
      bracketPairColorization: { enabled: true },
      suggestSelection: 'first',
      tabCompletion: 'on',
      quickSuggestions: { other: true, comments: false, strings: false },
      parameterHints: { enabled: true },
      scrollbar: { useShadows: false }
    });

    editor.onDidChangeModelContent(function () {
      save();
      monaco.editor.setModelMarkers(editor.getModel(), 'shark-run', []);
      scheduleCheck();
    });

    editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter, function () {
      start('run', { echo: true });
    });
    editor.addAction({
      id: 'shark.run', label: '実行する', contextMenuGroupId: 'shark', contextMenuOrder: 1,
      run: function () { start('run', { echo: true }); }
    });
    editor.addAction({
      id: 'shark.check', label: '型検査だけ', contextMenuGroupId: 'shark', contextMenuOrder: 2,
      run: function () { start('check', { echo: true }); }
    });
    editor.addAction({
      id: 'shark.test', label: 'テストを走らせる', contextMenuGroupId: 'shark', contextMenuOrder: 3,
      run: function () { start('test', { echo: true }); }
    });
    editor.addAction({
      id: 'shark.stop', label: '止める', contextMenuGroupId: 'shark', contextMenuOrder: 4,
      keybindings: [monaco.KeyMod.CtrlCmd | monaco.KeyCode.Period],
      run: function () { interrupt(); }
    });

    if (window.matchMedia) {
      window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () {
        monaco.editor.setTheme(themeName());
      });
    }
    buttons();
    if (api) scheduleCheck();
  });

  // 外から様子を見るための取っ手（自動での確かめや、埋め込んだときの操作に使う）
  window.SharkPlayground = {
    get editor() { return editor; },
    get monaco() { return monaco; },
    run: function () { start('run', { echo: true }); },
    check: function () { start('check', { echo: true }); },
    test: function () { start('test', { echo: true }); },
    stop: stop,
    type: function (text) { key.value = text; renderLine(); },
    enter: function () { commit(); },
    get text() { return log.textContent; },
    get running() { return running; },
    get waiting() { return waiting; }
  };

  // --- 処理系（WebAssembly）---
  createShark().then(function (mod) {
    M = mod;
    api = {
      boot: M.cwrap('shk_boot', null, []),
      version: M.cwrap('shk_version', 'string', []),
      config: M.cwrap('shk_config', null, ['number', 'number', 'number']),
      load: M.cwrap('shk_load', 'number', ['string', 'string']),
      diagnostics: M.cwrap('shk_diagnostics', 'string', []),
      hasEntry: M.cwrap('shk_has_entry', 'number', []),
      pushInput: M.cwrap('shk_push_input', null, ['string']),
      pushEof: M.cwrap('shk_push_eof', null, []),
      waitingInput: M.cwrap('shk_waiting_input', 'number', []),
      startRun: M.cwrap('shk_start_run', 'number', []),
      startTest: M.cwrap('shk_start_test', 'number', []),
      pump: M.cwrap('shk_pump', 'number', ['number']),
      abort: M.cwrap('shk_abort', null, []),
      uiClose: M.cwrap('shk_ui_close', null, []),
      exitCode: M.cwrap('shk_exit_code', 'number', []),
      error: M.cwrap('shk_error', 'string', []),
      testPassed: M.cwrap('shk_test_passed', 'number', []),
      testTotal: M.cwrap('shk_test_total', 'number', []),
      memoryUsed: M.cwrap('shk_memory_used', 'number', []),
      memoryLimit: M.cwrap('shk_memory_limit', 'number', []),
      modules: M.cwrap('shk_modules', 'string', []),
      explain: M.cwrap('shk_explain', 'string', ['string']),
    };
    api.boot();

    // 持っているモジュールを見せるために、いちど空のものを読ませる
    api.config(64, 0, 0);
    api.load('modules.shk', '');
    var mods = JSON.parse(api.modules());
    $('help-modules').textContent = mods.join('  ');
    $('st-version').textContent = 'Shark ' + api.version() + ' / WebAssembly';

    setState('待機中');
    buttons();
    append('Shark ' + api.version() + ' / WebAssembly — 左に書いて「実行」、' +
           'または run と打ちます（help でつかいかた）\n', 'sys');
    prompt();
    scheduleCheck();
  }, function (err) {
    setState('処理系を読み込めませんでした', 'error');
    append('shark.wasm を読み込めませんでした。\n' +
           'web/dist/ をそのまま配って開いているか確かめます（file:// では動きません）。\n' +
           String(err) + '\n', 'err');
  });
})();

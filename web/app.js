// app.js — プレイグラウンド UI 実装
//
// 処理系本体は web/shark_web.cpp を WebAssembly にコンパイルしたもの（shark.js / shark.wasm）。
// 本モジュールはホスト環境としての連携処理を担当:
//   ソースコード入力 → ステップ実行 → 標準出力・診断情報の受信と画面描画
// （spec/runtime/embedding.md）
//
// 右側のターミナルペインはネイティブ端末エミュレーション:
//   ・標準出力および入力文字列を同一ストリームに表示
//   ・input() 呼び出し時は入力待ちでブロック。Ctrl + D で EOF（none）
//   ・出力形式（診断・パニック・テスト結果）は shark CLI（frontend/main.cpp）と同一仕様
//
// エディタ領域は Monaco Editor を採用。Shark のシンタックスハイライトと補完候補は web/lang.js で定義。
(function () {
  'use strict';

  var FILE = 'playground.shk';
  var MAX_OUT_NODES = 3000;
  var CHECK_DELAY = 400;      // 入力停止からバックグラウンド型検査を実行するまでの遅延（ms）
  var PS1 = '$ ';             // ターミナルプロンプト
  var HISTORY_MAX = 100;

  var $ = function (id) { return document.getElementById(id); };
  var decoder = new TextDecoder();

  var M = null;         // WebAssembly モジュールインスタンス
  var api = null;       // shk_* C API ラッパー
  var monaco = null;
  var editor = null;
  var pending = '';     // Monaco Editor 初期化前のコードバッファ
  var running = false;
  var mode = 'run';
  var aborting = false;
  var waiting = false;  // input() による標準入力待ちフラグ
  var startedAt = 0;
  var rafId = 0;
  var budget = 200000;  // 1回の tick で実行する命令ステップ数（実行時間に応じて動的に調整）
  var checkTimer = 0;
  var color = true;     // ANSI カラー出力フラグ（--no-color で無効化可能）

  function code() { return editor ? editor.getValue() : pending; }
  function setCode(text) {
    pending = text;
    if (editor) editor.setValue(text);
  }

  // 実行時オプション。ツールバーの設定をデフォルトとし、コマンドライン引数（--lang 等）で上書き可能
  function flags() {
    return {
      lang: $('sel-lang').value,
      memory: Number($('sel-memory').value),
      strict: false,
      color: true
    };
  }

  // ================================================================ ターミナル
  // 端末ストリーム制御。log は出力済み履歴、line は現在入力中の行。
  // キー入力は非表示の textarea（term-key）で受領（IME 入力対応）。
  // 変換候補ウィンドウが正確な位置にポップアップするよう、キャレット位置へ追従配置する。
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

  // ターミナル出力。cls はスタイルクラス（ANSI カラー対応）
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

  // プロンプトを表示
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

  // 非表示の textarea をキャレット位置へ追従配置（IME 変換候補用）
  function placeKey() {
    var a = caret.getBoundingClientRect();
    var b = term.getBoundingClientRect();
    key.style.left = (a.left - b.left + term.scrollLeft) + 'px';
    key.style.top = (a.top - b.top + term.scrollTop) + 'px';
  }

  function focusTerm() {
    key.focus({ preventScroll: true });
  }

  // 1 行分の入力を確定。実行中ならプログラムの input() へ送信し、停止中ならコマンドとして解釈
  function commit() {
    var text = key.value;
    key.value = '';
    append((running ? '' : PS1) + text + '\n');   // ネイティブ端末と同様に入力行をストリームに残す
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

  function interrupt() {                       // Ctrl + C（実行中断）
    append((running ? '' : PS1) + key.value + '^C');   // ネイティブ端末と同様に改行せず ^C を表示
    key.value = '';
    if (running) stop();
    else prompt();
  }

  // Ctrl + D（EOF 送信）。端末と同様に ^D を一瞬表示した後に消去
  function endInput() {
    if (!running || key.value.length) return;  // 未送信の入力テキストがある場合は無効
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

  function recall(delta) {                     // ↑ / ↓ キーによるコマンド履歴参照
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
    if (e.isComposing || e.keyCode === 229) return;   // IME 変換中はスキップ
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
    setTimeout(renderLine, 0);   // 入力文字やキャレット移動の反映後に再描画
  });

  key.addEventListener('input', function () {
    var v = key.value;
    if (v.indexOf('\n') >= 0) {          // 貼り付けられた複数行テキストを 1 行ずつ送信
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
    if (sel && String(sel).length) return;   // テキスト選択中はフォーカス移動を抑止
    focusTerm();
  });
  term.classList.add('off');

  function drain() {
    var len = M._shk_out_len();
    if (!len) return;
    var ptr = M._shk_out_ptr();
    var bytes = M.HEAPU8.slice(ptr, ptr + len);   // メモリ再確保によるポインタ無効化を防ぐためコピー
    M._shk_out_clear();
    append(decoder.decode(bytes));
  }

  // ================================================================ 診断情報
  // カラム番号は「コードポイント（文字単位）」基準（spec/runtime/diagnostics.md）。
  // Monaco Editor は UTF-16 コードユニット基準のため、サロゲートペア対応のオフセット変換を行う。
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
      for (var h = 0; h < d.help.length; h++) msg += '\nヒント: ' + d.help[h];
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

  // ターミナル用フォーマット出力（コア層が生成した整形済みテキストを出力）
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
      box.innerHTML = '<p class="empty">エラーはありません。</p>';
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

  // ================================================================ 実行制御
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

  // 入力停止後にバックグラウンドで型検査を実行し、エディタにマーカーを表示（ターミナルには出力しない）
  function scheduleCheck() {
    if (checkTimer) clearTimeout(checkTimer);
    checkTimer = setTimeout(function () {
      checkTimer = 0;
      if (!api || running) return;
      compile(flags());
    }, CHECK_DELAY);
  }

  // UI ボタンからの実行時は、対応する shark コマンドをターミナルにエコー表示
  function echoCommand(which, fl, file) {
    var s = 'shark ' + which + ' ' + file;
    if (fl.lang === 'en') s += ' --lang en';
    if (fl.memory !== 256) s += ' --memory ' + fl.memory;
    if (fl.strict) s += ' --strict';
    newlineIfNeeded();
    append(PS1, 'ps1');
    append(s + '\n');
  }

  // which: 'run' | 'check' | 'test'。opts.echo: コマンド文字列をターミナルに表示するか
  function start(which, opts) {
    opts = opts || {};
    if (!api || running) return;
    var fl = opts.flags || flags();
    var file = opts.file || FILE;
    color = fl.color;
    showTab('term');
    if (opts.echo) echoCommand(which, fl, file);
    if (file !== FILE) {   // 対象ファイルはエディタ上のファイルのみ
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
      append(r.errs + ' 件のエラーがあります\n', 'err');
      setState(r.errs + ' 件のエラー', 'error');
      return;
    }
    if (which === 'check') {
      if (!r.diags.length) append('エラーはありません\n');
      setState(r.diags.length ? '警告 ' + r.diags.length + ' 件' : '正常終了', 'done');
      return;
    }

    if (which === 'test') {
      var found = api.startTest();
      if (found <= 0) {
        append('test_ で始まる関数が見つかりません\n');
        setState('テストなし', 'done');
        return;
      }
      append(file + '\n');
    } else {
      if (!api.hasEntry()) {
        append('エントリポイントがありません\n' +
               '  ヒント: func main() -> int { } を定義するか、トップレベル文を記述してください\n', 'err');
        setState('実行不能', 'error');
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

  // requestAnimationFrame ごとにステップ実行
  function tick() {
    var t0 = performance.now();
    var status = api.pump(budget);
    var spent = performance.now() - t0;
    drain();
    if (uiScreen) screenSize();

    var wait = api.waitingInput() === 1;   // input() による標準入力待ち
    if (wait !== waiting) {
      waiting = wait;
      setState(wait ? '入力を待っています' : (mode === 'test' ? 'テスト中…' : '実行中…'), 'running');
      if (wait) { showTab('term'); toBottom(); focusTerm(); }
    }
    // 待っている間は進んでいないので、進める量はそのままにする
    if (!wait) {
      // フレームレートが維持されるよう、実行命令バジェットを動的に調整
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
    // UI 画面が開いたまま終了した場合は閉じる（ネイティブ CLI ではプロセス終了時に破棄されるリソース）。
    // 開いたまま残ると、イベント受信ハンドラが不在のウィンドウが残留してしまうため。
    if (uiScreen) api.uiClose();
    stats();
    buttons();

    if (mode === 'test') {
      var passed = api.testPassed(), total = api.testTotal();
      if (status === 2 && !aborting) {
        showPanic();
        setState('エラー終了', 'error');
      } else if (aborting) {
        setState('中断しました', 'error');
      } else {
        append('\n' + total + ' 件中 ' + passed + ' 件成功\n');
        setState(passed === total ? '全テスト合格' : (total - passed) + ' 件失敗',
                 passed === total ? 'done' : 'error');
      }
      prompt();
      return;
    }

    if (status === 1) {
      var codeNum = api.exitCode();
      $('st-exit').textContent = '終了コード ' + codeNum;
      setState('実行完了', 'done');
    } else if (aborting) {
      setState('中断しました', 'error');
    } else {
      showPanic();
      setState('エラー終了', 'error');
    }
    prompt();
  }

  // ターミナル出力（frontend/main.cpp の print_panic と同一形式）
  function showPanic() {
    var e = JSON.parse(api.error());
    var text = 'panic: ' + e.message + '\n';
    if (e.line > 0) text += '  --> ' + e.file + ':' + e.line + '\n';
    if (e.trace) text += '  スタックトレース:\n' + e.trace;
    append(text, 'err');
    $('st-exit').textContent = '';
    if (e.line > 0 && editor && monaco) {
      // パニック発生行にマーカーを設定
      monaco.editor.setModelMarkers(editor.getModel(), 'shark-run', [{
        severity: monaco.MarkerSeverity.Error,
        message: 'パニック発生位置: ' + e.message,
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

  // ソースコード整形（core/fmt_src.cpp。shark fmt と同一）。構文エラーがある場合は適用しない
  function formatCode(echo) {
    if (!api || running) return;
    var src = code();
    var out = api.format(src);
    if (!api.formatted()) {
      if (echo) append('整形できません（構文エラーがあります）\n', 'err');
      setState('整形失敗', 'error');
      return;
    }
    if (out === src) {
      if (echo) append('既に整形済みです\n');
      setState('整形済み', 'done');
      return;
    }
    setCode(out);
    if (echo) append('コードを整形しました\n');
    setState('整形完了', 'done');
    scheduleCheck();
  }

  function buttons() {
    var ready = !!api && !!editor;
    $('btn-run').disabled = running || !ready;
    $('btn-check').disabled = running || !ready;
    $('btn-test').disabled = running || !ready;
    $('btn-fmt').disabled = running || !ready;
    $('btn-stop').disabled = !running;
  }

  // ================================================================ コマンドライン解釈
  // shark CLI（frontend/main.cpp）と互換性のあるコマンド体系。
  // ブラウザ環境で対象にできるファイルは playground.shk（エディタ内のコード）のみ。
  var USAGE =
    'Shark🦈  ゲーム機で動く学習用プログラミング言語\n' +
    '\n' +
    '使い方:\n' +
    '  shark run <file.shk>      プログラムを実行する\n' +
    '  shark check <file.shk>    型検査のみを行う\n' +
    '  shark test [file.shk]     test_ で始まる関数を実行する\n' +
    '  shark fmt <file.shk>      ソースコードを整形する（エディタの内容を更新）\n' +
    '  shark explain E0102       エラーコードの詳細解説を表示する\n' +
    '  shark modules             利用可能なモジュール一覧を表示する\n' +
    '\n' +
    'オプション:\n' +
    '  --lang ja|en   診断メッセージの言語（既定値: ja）\n' +
    '  --memory <MB>  メモリ使用量上限（MB）。超過時は実行時パニック（既定値: 256、0 で無制限）\n' +
    '  --strict       警告をエラーとして扱う\n' +
    '  --no-color     カラー出力を無効化する\n' +
    '\n' +
    'ブラウザ環境で対象にできるファイルは ' + FILE + '（エディタ内のコード）のみです。\n' +
    'clear でターミナルをクリアします。Ctrl + C で中断、Ctrl + D で EOF を送信します。\n';

  function words(text) {
    var r = text.split(/\s+/);
    var out = [];
    for (var i = 0; i < r.length; i++) if (r[i].length) out.push(r[i]);
    return out;
  }

  // ツールバーの選択状態を引数に合わせて同期
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
          append('--memory には MB 単位の数値を指定してください（例: --memory 32）\n', 'err');
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

    if (cmd === 'fmt') {
      formatCode(true);
      return;
    }
    if (cmd === 'run' || cmd === 'check' || cmd === 'test') {
      start(cmd, { flags: fl, file: arg || FILE });
      return;
    }
    if (cmd === 'explain') {
      if (!arg) { append(USAGE); return; }
      var t = api.explain(arg);
      append(t ? arg + '\n' + t + '\n' : arg + ' の解説は見つかりませんでした\n');
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

    append('未定義のコマンドです: ' + cmd + '\n', 'err');
    append(USAGE);
  }

  // ================================================================ UI タブ制御
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

  // ================================================================ UI 画面連携（std.ui）
  // ウィンドウ生成はプラットフォーム層（core/platform/screen_canvas.inc）が担当し、
  // 本モジュールはオープン／クローズイベントを受信してステータスバーに解像度を表示する
  var uiScreen = null;      // アクティブなキャンバス画面情報（プラットフォーム層から通知）
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
  $('btn-fmt').addEventListener('click', function () { formatCode(true); });
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
    // ドキュメントパネルの開閉
    if ((e.key === 'i' || e.key === 'I') && (e.ctrlKey || e.metaKey) && !e.shiftKey) {
      e.preventDefault();
      docsToggle();
    }
    // ソースコード整形（Shift+Alt+F）
    if ((e.key === 'f' || e.key === 'F') && e.shiftKey && e.altKey) {
      e.preventDefault();
      formatCode(false);
    }
  });
  $('sel-lang').addEventListener('change', scheduleCheck);
  $('sel-memory').addEventListener('change', scheduleCheck);

  // スプリッター（ペイン境界）のドラッグ移動
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

  // ================================================================ ドキュメントパネル
  // 別タブへ遷移せず、エディタを見ながら同一画面上で参照可能なモーダルドキュメントビューワ。
  // 表示位置、サイズ、開閉状態は LocalStorage に保存される。
  var DOCS_KEY = 'shark.docs';
  var docsLoaded = false;

  function docsBox() {
    var w = $('docs');
    return { x: w.offsetLeft, y: w.offsetTop, w: w.offsetWidth, h: w.offsetHeight };
  }

  function docsPut(b) {
    var w = $('docs');
    var maxw = window.innerWidth - 40, maxh = window.innerHeight - 40;
    var bw = Math.max(260, Math.min(b.w || 460, maxw));
    var bh = Math.max(160, Math.min(b.h || 520, maxh));
    var bx = Math.max(0, Math.min(b.x == null ? window.innerWidth - bw - 24 : b.x,
                                  window.innerWidth - 80));
    var by = Math.max(0, Math.min(b.y == null ? 64 : b.y, window.innerHeight - 60));
    w.style.left = bx + 'px';
    w.style.top = by + 'px';
    w.style.width = bw + 'px';
    w.style.height = bh + 'px';
  }

  function docsSave(open) {
    try {
      var b = docsBox();
      b.open = open;
      localStorage.setItem(DOCS_KEY, JSON.stringify(b));
    } catch (e) { /* LocalStorage が無効化されている環境への配慮 */ }
  }

  function docsOpen(on) {
    var w = $('docs');
    if (on && !docsLoaded) {          // 初回表示時まで iframe の読み込みを遅延
      $('docs-frame').src = 'docs/index.html';
      docsLoaded = true;
    }
    w.classList.toggle('hidden', !on);
    if (on) docsPut(docsBox());       // 画面外へのはみ出しを防止
    docsSave(on);
  }

  function docsToggle() { docsOpen($('docs').classList.contains('hidden')); }

  (function () {
    var w = $('docs');
    var saved = null;
    try { saved = JSON.parse(localStorage.getItem(DOCS_KEY) || 'null'); } catch (e) { saved = null; }
    docsPut(saved || {});
    if (saved && saved.open) docsOpen(true);

    // タイトルバーのドラッグ移動
    var head = $('docs-head'), drag = null;
    head.addEventListener('pointerdown', function (e) {
      if (e.target.closest('button, a')) return;   // ボタン上のドラッグ操作は除外
      var b = docsBox();
      drag = { dx: e.clientX - b.x, dy: e.clientY - b.y };
      w.classList.add('dragging');
      head.setPointerCapture(e.pointerId);
    });
    head.addEventListener('pointermove', function (e) {
      if (!drag) return;
      docsPut({ x: e.clientX - drag.dx, y: e.clientY - drag.dy,
                w: w.offsetWidth, h: w.offsetHeight });
    });
    head.addEventListener('pointerup', function (e) {
      if (!drag) return;
      drag = null;
      w.classList.remove('dragging');
      head.releasePointerCapture(e.pointerId);
      docsSave(true);
    });

    // 右下リサイズハンドルによるサイズ変更
    var grip = $('docs-grip'), size = null;
    grip.addEventListener('pointerdown', function (e) {
      var b = docsBox();
      size = { x: e.clientX, y: e.clientY, w: b.w, h: b.h };
      w.classList.add('dragging');
      grip.setPointerCapture(e.pointerId);
      e.preventDefault();
    });
    grip.addEventListener('pointermove', function (e) {
      if (!size) return;
      var b = docsBox();
      docsPut({ x: b.x, y: b.y,
                w: size.w + (e.clientX - size.x), h: size.h + (e.clientY - size.y) });
    });
    grip.addEventListener('pointerup', function (e) {
      if (!size) return;
      size = null;
      w.classList.remove('dragging');
      grip.releasePointerCapture(e.pointerId);
      docsSave(true);
    });

    window.addEventListener('resize', function () {
      if (!w.classList.contains('hidden')) docsPut(docsBox());
    });
  })();

  $('btn-docs').addEventListener('click', docsToggle);
  $('docs-close').addEventListener('click', function () { docsOpen(false); });
  $('docs-home').addEventListener('click', function () {
    $('docs-frame').src = 'docs/index.html';
  });
  $('docs-back').addEventListener('click', function () {
    try { $('docs-frame').contentWindow.history.back(); } catch (e) { /* 別オリジン制限への配慮 */ }
  });

  // ================================================================ 保存と共有
  function save() {
    try {
      localStorage.setItem('shark.code', code());
    } catch (e) { /* LocalStorage が無効化されている環境への配慮 */ }
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
      append('共有用 URL をクリップボードにコピーしました\n', 'sys');
      prompt();
    };
    if (navigator.clipboard) navigator.clipboard.writeText(url).then(done, done);
    else done();
  });

  // サンプルコードセレクタ
  (function () {
    var sel = $('sel-example');
    var list = window.SHARK_EXAMPLES || [];
    sel.innerHTML = '<option value="">サンプルを選択…</option>';
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

  // ================================================================ 起動初期化
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
      try { return fromHash(m[1]); } catch (e) { /* ハッシュ破損時のフォールバック */ }
    }
    try {
      var saved = localStorage.getItem('shark.code');
      if (saved) return saved;
    } catch (e) { /* LocalStorage が無効化されている環境への配慮 */ }
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

  // --- エディタ初期化（Monaco Editor）---
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
      id: 'shark.check', label: '型検査のみ', contextMenuGroupId: 'shark', contextMenuOrder: 2,
      run: function () { start('check', { echo: true }); }
    });
    editor.addAction({
      id: 'shark.test', label: 'テストを実行する', contextMenuGroupId: 'shark', contextMenuOrder: 3,
      run: function () { start('test', { echo: true }); }
    });
    editor.addAction({
      id: 'shark.stop', label: '実行を中断する', contextMenuGroupId: 'shark', contextMenuOrder: 4,
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

  // 外部連携用 API インタフェース（テストや外部ホストからの制御用）
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

  // --- WebAssembly ランタイム初期化 ---
  createShark().then(function (mod) {
    M = mod;
    api = {
      boot: M.cwrap('shk_boot', null, []),
      version: M.cwrap('shk_version', 'string', []),
      config: M.cwrap('shk_config', null, ['number', 'number', 'number']),
      load: M.cwrap('shk_load', 'number', ['string', 'string']),
      diagnostics: M.cwrap('shk_diagnostics', 'string', []),
      format: M.cwrap('shk_format', 'string', ['string']),
      formatted: M.cwrap('shk_formatted', 'number', []),
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

    // 利用可能モジュール一覧を取得するため、空のコードを一度ロード
    api.config(64, 0, 0);
    api.load('modules.shk', '');
    var mods = JSON.parse(api.modules());
    $('help-modules').textContent = mods.join('  ');
    $('st-version').textContent = 'Shark ' + api.version() + ' / WebAssembly';

    setState('待機中');
    buttons();
    append('Shark ' + api.version() + ' / WebAssembly — エディタにコードを記述して「実行」、' +
           'または run コマンドを入力します（help で使い方を表示）\n', 'sys');
    prompt();
    scheduleCheck();
  }, function (err) {
    setState('ランタイムの初期化に失敗しました', 'error');
    append('shark.wasm を読み込めませんでした。\n' +
           'HTTP サーバー経由で開いているか確認してください（file:// プロトコルでは動作しません）。\n' +
           String(err) + '\n', 'err');
  });
})();

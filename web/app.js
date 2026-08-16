// app.js — プレイグラウンドの画面まわり
//
// 処理系そのものは web/shark_web.cpp を WebAssembly にしたもの（shark.js / shark.wasm）。
// ここがしているのは、ホストの仕事だけ:
//   ソースを渡す → 少しずつ進める → 出力と診断を受け取って画面に出す
// （spec/runtime/embedding.md）
//
// 書くところは Monaco Editor。Shark の色分けと入力補完は web/lang.js にある。
(function () {
  'use strict';

  var FILE = 'playground.shk';
  var MAX_OUT_NODES = 3000;
  var CHECK_DELAY = 400;      // 打つ手が止まってから、型検査を走らせるまで

  var $ = function (id) { return document.getElementById(id); };
  var out = $('out');
  var decoder = new TextDecoder();

  var M = null;         // WebAssembly の中身
  var api = null;       // shk_* の呼び出し口
  var monaco = null;
  var editor = null;
  var pending = '';     // Monaco ができるまでの、書きかけの中身
  var running = false;
  var mode = 'run';
  var aborting = false;
  var startedAt = 0;
  var rafId = 0;
  var budget = 200000;  // 1回に進める命令の数。時間を見て増減させる（結果は変わらない）
  var checkTimer = 0;

  function code() { return editor ? editor.getValue() : pending; }
  function setCode(text) {
    pending = text;
    if (editor) editor.setValue(text);
  }

  // ================================================================ 出力
  function outNear() {
    return out.scrollHeight - out.scrollTop - out.clientHeight < 40;
  }
  function append(text, cls) {
    if (!text) return;
    var stick = outNear();
    var node = cls ? document.createElement('span') : document.createTextNode(text);
    if (cls) { node.className = cls; node.textContent = text; }
    out.appendChild(node);
    while (out.childNodes.length > MAX_OUT_NODES) out.removeChild(out.firstChild);
    if (stick) out.scrollTop = out.scrollHeight;
  }
  function clearOut() { out.textContent = ''; }

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

  function compile() {
    api.config(Number($('sel-memory').value), $('sel-lang').value === 'en' ? 1 : 0, 0);
    var errs = api.load(FILE, code());
    var diags = JSON.parse(api.diagnostics());
    showDiagnostics(diags);
    return { errs: errs, diags: diags };
  }

  // 打つ手が止まったら、本物の型検査を走らせて波線を引く
  function scheduleCheck() {
    if (checkTimer) clearTimeout(checkTimer);
    checkTimer = setTimeout(function () {
      checkTimer = 0;
      if (!api || running) return;
      compile();
    }, CHECK_DELAY);
  }

  function start(which) {
    if (!api || running) return;
    mode = which;
    aborting = false;
    if (checkTimer) { clearTimeout(checkTimer); checkTimer = 0; }
    clearOut();
    $('st-exit').textContent = '';
    showTab('out');

    var r = compile();
    if (r.errs > 0) {
      setState(r.errs + ' 件の誤りがあります', 'error');
      showTab('diag');
      return;
    }
    if (which === 'check') {
      setState(r.diags.length ? '警告 ' + r.diags.length + ' 件' : '問題ありません', 'done');
      append(r.diags.length ? '警告があります（「診断」を見てください）\n' : '問題ありません\n', 'sys');
      if (r.diags.length) showTab('diag');
      return;
    }

    var stdin = $('stdin').value;
    if (stdin) api.pushInput(stdin);

    if (which === 'test') {
      var found = api.startTest();
      if (found <= 0) {
        append('test_ で始まる関数がありません\n', 'sys');
        setState('テストなし', 'done');
        return;
      }
      append(FILE + '\n', 'sys');
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
    rafId = requestAnimationFrame(tick);
  }

  // 1回の描画につき1度だけ進める。ここが「少しずつ動かす」の実物
  function tick() {
    var t0 = performance.now();
    var status = api.pump(budget);
    var spent = performance.now() - t0;
    drain();

    // 画面が止まって見えない範囲で、進める量を寄せていく
    if (spent < 6 && budget < 8000000) budget = Math.round(budget * 1.5);
    else if (spent > 14 && budget > 20000) budget = Math.round(budget / 1.5);

    stats();
    if (status === 0) { rafId = requestAnimationFrame(tick); return; }
    finish(status);
  }

  function finish(status) {
    running = false;
    rafId = 0;
    drain();
    stats();
    buttons();

    if (mode === 'test') {
      var passed = api.testPassed(), total = api.testTotal();
      if (status === 2 && !aborting) {
        showPanic();
        setState('止まりました', 'error');
      } else if (aborting) {
        append('\n止めました\n', 'sys');
        setState('止めました', 'error');
      } else {
        append('\n' + total + ' 件中 ' + passed + ' 件成功\n', passed === total ? 'ok' : 'err');
        setState(passed === total ? 'ぜんぶ通りました' : (total - passed) + ' 件が失敗',
                 passed === total ? 'done' : 'error');
      }
      return;
    }

    if (status === 1) {
      var codeNum = api.exitCode();
      $('st-exit').textContent = '終了コード ' + codeNum;
      append('\n終わりました（終了コード ' + codeNum + '）\n', 'sys');
      setState('終わりました', 'done');
      return;
    }
    if (aborting) {
      append('\n止めました\n', 'sys');
      setState('止めました', 'error');
      return;
    }
    showPanic();
    setState('止まりました', 'error');
  }

  function showPanic() {
    var e = JSON.parse(api.error());
    var text = '\npanic: ' + e.message + '\n';
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
  }

  function buttons() {
    var ready = !!api && !!editor;
    $('btn-run').disabled = running || !ready;
    $('btn-check').disabled = running || !ready;
    $('btn-test').disabled = running || !ready;
    $('btn-stop').disabled = !running;
    $('btn-send').disabled = !running;
  }

  // ================================================================ 画面の部品
  function showTab(name) {
    ['out', 'diag', 'in'].forEach(function (t) {
      $(t).classList.toggle('hidden', t !== name);
    });
    Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (b) {
      b.classList.toggle('active', b.dataset.tab === name);
    });
  }
  Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (b) {
    b.addEventListener('click', function () { showTab(b.dataset.tab); });
  });

  $('btn-run').addEventListener('click', function () { start('run'); });
  $('btn-check').addEventListener('click', function () { start('check'); });
  $('btn-test').addEventListener('click', function () { start('test'); });
  $('btn-stop').addEventListener('click', stop);
  $('btn-clear').addEventListener('click', clearOut);
  $('btn-send').addEventListener('click', function () {
    if (!running) return;
    api.pushInput($('stdin').value);
    append('（入力を送りました）\n', 'sys');
  });
  $('btn-help').addEventListener('click', function () { $('help').classList.remove('hidden'); });
  $('help-close').addEventListener('click', function () { $('help').classList.add('hidden'); });
  $('help').addEventListener('click', function (e) {
    if (e.target === $('help')) $('help').classList.add('hidden');
  });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') $('help').classList.add('hidden');
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); start('run'); }
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
    });
    sp.addEventListener('pointerup', function (e) {
      drag = false;
      sp.releasePointerCapture(e.pointerId);
    });
  })();

  // ================================================================ 保存と共有
  function save() {
    try {
      localStorage.setItem('shark.code', code());
      localStorage.setItem('shark.stdin', $('stdin').value);
    } catch (e) { /* 使えない設定のこともある */ }
  }
  $('stdin').addEventListener('input', save);

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
    var done = function () { append('共有できる URL をコピーしました\n', 'sys'); showTab('out'); };
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
      clearOut();
      append(ex.path + ' を読み込みました\n', 'sys');
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
      if (saved) {
        var si = localStorage.getItem('shark.stdin');
        if (si) $('stdin').value = si;
        return saved;
      }
    } catch (e) { /* 使えない設定のこともある */ }
    return FIRST;
  }

  pending = restore();
  setState('読み込み中…');
  $('editor').classList.add('loading');

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

    editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter, function () { start('run'); });
    editor.addAction({
      id: 'shark.run', label: '実行する', contextMenuGroupId: 'shark', contextMenuOrder: 1,
      run: function () { start('run'); }
    });
    editor.addAction({
      id: 'shark.check', label: '型検査だけ', contextMenuGroupId: 'shark', contextMenuOrder: 2,
      run: function () { start('check'); }
    });
    editor.addAction({
      id: 'shark.test', label: 'テストを走らせる', contextMenuGroupId: 'shark', contextMenuOrder: 3,
      run: function () { start('test'); }
    });
    editor.addAction({
      id: 'shark.stop', label: '止める', contextMenuGroupId: 'shark', contextMenuOrder: 4,
      keybindings: [monaco.KeyMod.CtrlCmd | monaco.KeyCode.Period],
      run: function () { stop(); }
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
    run: function () { start('run'); },
    check: function () { start('check'); },
    test: function () { start('test'); },
    stop: stop,
    get running() { return running; }
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
      startRun: M.cwrap('shk_start_run', 'number', []),
      startTest: M.cwrap('shk_start_test', 'number', []),
      pump: M.cwrap('shk_pump', 'number', ['number']),
      abort: M.cwrap('shk_abort', null, []),
      exitCode: M.cwrap('shk_exit_code', 'number', []),
      error: M.cwrap('shk_error', 'string', []),
      testPassed: M.cwrap('shk_test_passed', 'number', []),
      testTotal: M.cwrap('shk_test_total', 'number', []),
      memoryUsed: M.cwrap('shk_memory_used', 'number', []),
      memoryLimit: M.cwrap('shk_memory_limit', 'number', []),
      modules: M.cwrap('shk_modules', 'string', []),
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
    append('準備ができました。左に書いて「実行」を押します。\n', 'sys');
    scheduleCheck();
  }, function (err) {
    setState('処理系を読み込めませんでした', 'error');
    append('shark.wasm を読み込めませんでした。\n' +
           'web/dist/ をそのまま配って開いているか確かめます（file:// では動きません）。\n' +
           String(err) + '\n', 'err');
  });
})();

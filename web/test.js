// test.js — ブラウザ用に作ったものが動くかを、node で確かめる
//
//   sh web/build.sh && node web/test.js
//
// 画面を出さずに、コアの入口（web/shark_web.cpp）だけを叩く。
const fs = require('fs');
const path = require('path');
const createShark = require('./dist/shark.js');

const root = path.join(__dirname, '..');
const decoder = new TextDecoder();
let failed = 0;

// api.js（補完のもと）を読む
function loadApi() {
  var text = fs.readFileSync(path.join(__dirname, 'dist/api.js'), 'utf8');
  return JSON.parse(text.slice(text.indexOf('{'), text.lastIndexOf(';')));
}

function find(list, name) {
  for (var i = 0; i < list.length; i++) if (list[i].name === name) return list[i];
  return {};
}

function check(name, ok, detail) {
  console.log((ok ? '  ok    ' : '  fail  ') + name);
  if (!ok) {
    failed++;
    if (detail) console.log('        ' + String(detail).replace(/\n/g, '\n        '));
  }
}


// 要るところだけの偽 DOM。画面（core/platform/screen_canvas.inc）が触るものだけを持つ。
// これで、canvas に出す道と出来事の訳し方を、ブラウザを開かずに確かめられる
function fakeDom() {
  const saved = {};
  // canvas の 2D。面を出すところと、字を描くところ（font_canvas.inc）が使う。
  // 字は「1文字ぶんの箱」を塗るだけの、いちばん簡単な偽物にしてある
  const ctx = {
    last: null,
    font: '10px sans-serif',
    fillStyle: '#000',
    textBaseline: 'alphabetic',
    ink: null,
    createImageData: (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
    putImageData: (img) => { ctx.last = img; },
    _px: () => parseFloat(ctx.font) || 10,
    // ヒラギノがあることにする（無い名前は幅が変わらない ＝ 無い、と見なされる）
    _wide: () => (ctx.font.indexOf('Hiragino') >= 0 ? 0.62 : 0.6),
    measureText(t) {
      const px = ctx._px();
      const n = Array.from(String(t)).length;
      return {
        width: n * px * ctx._wide(),
        actualBoundingBoxLeft: 0,
        actualBoundingBoxRight: n * px * ctx._wide(),
        actualBoundingBoxAscent: Math.round(px * 0.8),
        actualBoundingBoxDescent: Math.round(px * 0.2),
        fontBoundingBoxAscent: Math.round(px * 0.85),
        fontBoundingBoxDescent: Math.round(px * 0.25),
      };
    },
    clearRect() { ctx.ink = null; },
    fillText(t, x, y) { ctx.ink = { t: String(t), x, y }; },
    getImageData(x, y, w, h) {
      const d = new Uint8ClampedArray(w * h * 4);
      if (ctx.ink) for (let i = 0; i < w * h; i++) d[i * 4 + 3] = 255;   // 箱いっぱいに塗る
      return { width: w, height: h, data: d };
    },
  };
  const MOUNT_W = 300;
  const MOUNT_H = 200;

  function px(v) { return v ? parseFloat(String(v)) || 0 : 0; }

  function el(tag) {
    const e = {
      tagName: tag.toUpperCase(),
      style: {},
      children: [],
      parentNode: null,
      offsetLeft: 0,
      offsetTop: 0,
      _on: {},
      addEventListener(k, fn) { (e._on[k] = e._on[k] || []).push(fn); },
      removeEventListener(k, fn) {
        const a = e._on[k] || [];
        const i = a.indexOf(fn);
        if (i >= 0) a.splice(i, 1);
      },
      dispatchEvent(ev) {
        ev.target = ev.target || e;
        (e._on[ev.type] || []).slice().forEach((fn) => fn(ev));
        if (ev.bubbles && e.parentNode) e.parentNode.dispatchEvent(ev);
        return true;
      },
      appendChild(c) { c.parentNode = e; e.children.push(c); return c; },
      removeChild(c) {
        const i = e.children.indexOf(c);
        if (i >= 0) e.children.splice(i, 1);
        c.parentNode = null;
      },
      contains(c) { for (; c; c = c.parentNode) if (c === e) return true; return false; },
      setAttribute() {},
      focus() { doc.activeElement = e; },
      querySelector() { return null; },
      getBoundingClientRect() {
        if (e.tagName === 'CANVAS') return { left: 0, top: 0, width: px(e.style.width), height: px(e.style.height) };
        return { left: 0, top: 0, width: MOUNT_W, height: MOUNT_H };
      },
    };
    Object.defineProperty(e, 'textContent', { get: () => '', set: () => { e.children = []; } });
    if (tag === 'canvas') { e.width = 0; e.height = 0; e.getContext = () => ctx; }
    if (tag === 'textarea') { e.value = ''; e.selectionStart = 0; e.selectionEnd = 0; }
    return e;
  }

  const body = el('body');
  const doc = {
    body,
    activeElement: body,
    createElement: el,
    getElementById: () => null,
    querySelector: () => null,
    addEventListener() {},
    removeEventListener() {},
  };
  const opens = [];
  let closes = 0;
  const win = {
    devicePixelRatio: 2,
    getComputedStyle: () => ({ position: 'static' }),
    addEventListener() {},
    removeEventListener() {},
    dispatchEvent(ev) {
      if (ev.type === 'shark:screen-open') opens.push(ev.detail);
      if (ev.type === 'shark:screen-close') closes++;
      return true;
    },
  };
  class Evt {
    constructor(type, init) { this.type = type; Object.assign(this, init || {}); }
  }

  for (const [k, v] of Object.entries({ document: doc, window: win, CustomEvent: Evt,
                                        PointerEvent: Evt, KeyboardEvent: Evt })) {
    saved[k] = globalThis[k];
    globalThis[k] = v;
  }

  const api = {
    ctx,
    opens,
    get closes() { return closes; },
    get canvas() { return opens.length ? opens[opens.length - 1].canvas : null; },
    textarea() {
      const m = opens.length ? opens[opens.length - 1].mount : null;
      return m ? m.children.filter((c) => c.tagName === 'TEXTAREA')[0] || null : null;
    },
    key(target, type, key) {
      target.dispatchEvent(new Evt(type, { key, bubbles: true, ctrlKey: false, metaKey: false, altKey: false,
                                           preventDefault() {} }));
    },
    // 面の画素の上に、押さずに動かす
    hover(sx, sy) {
      const c = api.canvas;
      const r = c.getBoundingClientRect();
      const k = r.width / c.width;
      c.dispatchEvent(new Evt('pointermove', { clientX: r.left + sx * k + k / 2,
                                               clientY: r.top + sy * k + k / 2,
                                               button: 0, pointerId: 1, bubbles: true,
                                               preventDefault() {} }));
    },
    cursor() { return api.canvas ? api.canvas.style.cursor : ''; },
    // 面の画素で押す（画面の点に直してから渡す）
    tap(sx, sy) {
      const c = api.canvas;
      const r = c.getBoundingClientRect();
      const k = r.width / c.width;
      const at = { clientX: r.left + sx * k + k / 2, clientY: r.top + sy * k + k / 2,
                   button: 0, pointerId: 1, bubbles: true, preventDefault() {} };
      c.dispatchEvent(new Evt('pointerdown', at));
      c.dispatchEvent(new Evt('pointerup', at));
    },
    close() { opens[opens.length - 1].requestClose(); },
    restore() { for (const [k, v] of Object.entries(saved)) globalThis[k] = v; },
  };
  return api;
}

createShark().then((M) => {
  const api = {
    boot: M.cwrap('shk_boot', null, []),
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
    idle: M.cwrap('shk_idle', 'number', []),
    abort: M.cwrap('shk_abort', null, []),
    exitCode: M.cwrap('shk_exit_code', 'number', []),
    error: M.cwrap('shk_error', 'string', []),
    testResults: M.cwrap('shk_test_results', 'string', []),
    testPassed: M.cwrap('shk_test_passed', 'number', []),
    testTotal: M.cwrap('shk_test_total', 'number', []),
    memoryUsed: M.cwrap('shk_memory_used', 'number', []),
    modules: M.cwrap('shk_modules', 'string', []),
    explain: M.cwrap('shk_explain', 'string', ['string']),
  };

  function take() {
    const len = M._shk_out_len();
    if (len === 0) return '';
    const bytes = M.HEAPU8.slice(M._shk_out_ptr(), M._shk_out_ptr() + len);
    M._shk_out_clear();
    return decoder.decode(bytes);
  }

  // 待ちに入ったら少し休む（ブラウザなら次の描画まで待つところ）
  const sleepBuf = new Int32Array(new SharedArrayBuffer(4));
  function nap(ms) { Atomics.wait(sleepBuf, 0, 0, ms); }

  // 1回に 20 万命令ずつ、最大 maxMs ミリ秒まで進める。
  // input() が行を待ち始めたら、そこでいったん返す（ブラウザなら打たれるのを待つところ）
  function drive(maxMs) {
    const start = Date.now();
    let out = '';
    while (Date.now() - start < maxMs) {
      const status = api.pump(200000);
      out += take();
      if (status !== 0) return { status, out };
      if (api.waitingInput()) return { status: 0, out, waiting: true };
      if (api.idle()) nap(2);
    }
    return { status: 0, out };
  }

  function run(source, input) {
    api.config(64, 0, 0);
    const errs = api.load('playground.shk', source);
    if (errs > 0) return { errors: errs, out: '', diagnostics: JSON.parse(api.diagnostics()) };
    if (input) api.pushInput(input);
    api.startRun();
    const r = drive(5000);
    return { errors: 0, out: r.out + take(), status: r.status, code: api.exitCode() };
  }

  api.boot();
  console.log('web/dist を確かめます');

  // --- お手本がそのまま動く ---
  const hello = run(fs.readFileSync(path.join(root, 'examples/hello.shk'), 'utf8'));
  check('hello.shk', hello.out === 'Hello, Shark!\n', JSON.stringify(hello.out));

  const fizz = run(fs.readFileSync(path.join(root, 'examples/fizzbuzz.shk'), 'utf8'));
  check('fizzbuzz.shk', fizz.out.split('\n')[14] === 'FizzBuzz' && fizz.status === 1, fizz.out);

  const fish = run(fs.readFileSync(path.join(root, 'examples/fish.shk'), 'utf8'));
  check('fish.shk（クラスと継承）', fish.status === 1 && fish.out.length > 0, fish.out);

  const tasks = run(fs.readFileSync(path.join(root, 'examples/tasks.shk'), 'utf8'));
  check('tasks.shk（並行処理と待ち）', tasks.status === 1, tasks.out);

  const config = run(fs.readFileSync(path.join(root, 'examples/config.shk'), 'utf8'));
  check('config.shk（file / json / os を使う）', config.errors === 0 && config.status === 1, config.out);

  // --- std.crypto（乱数のもとはブラウザの暗号用乱数を借りる）---
  const crypt = run('import std.crypto;\n' +
                    'func main() -> int {\n' +
                    '  print(crypto.sha256("abc").to_hex());\n' +
                    '  print(crypto.source());\n' +
                    '  print(crypto.random_bytes(32).len());\n' +
                    '  return 0;\n' +
                    '}\n');
  check('std.crypto（ハッシュと、ブラウザから借りる乱数）',
        crypt.out === 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n' +
                      'secure\n32\n',
        crypt.out);

  // --- 入力（端末と同じで、打たれるまで待つ）---
  const askSrc = fs.readFileSync(path.join(root, 'web/examples/ask.shk'), 'utf8');
  const ask = run(askSrc, 'さめ\n');
  check('input() が読める', ask.out.indexOf('こんにちは、さめ さん！') >= 0, ask.out);

  function startAsk() {
    api.config(64, 0, 0);
    api.load('ask.shk', askSrc);
    api.startRun();
    return drive(3000);
  }

  const waited = startAsk();
  check('行が来るまで input() は待つ',
        waited.waiting === true && waited.status === 0 && api.waitingInput() === 1 &&
        waited.out.indexOf('名前を教えてください') >= 0,
        JSON.stringify(waited));
  api.pushInput('まぐろ\n');
  const answered = drive(3000);
  check('打たれた行がそのまま渡る',
        answered.status === 1 && answered.out.indexOf('こんにちは、まぐろ さん！') >= 0,
        JSON.stringify(answered));

  startAsk();
  api.pushEof();                       // 端末の Ctrl + D
  const eof = drive(3000);
  check('入力の終わり（Ctrl + D）では none',
        eof.status === 1 && eof.out.indexOf('入力がありませんでした') >= 0, JSON.stringify(eof));

  // --- 診断 ---
  const bad = run('func main() -> int {\n  var a = 1 + "さめ";\n  return 0;\n}\n');
  check('型の誤りが診断として返る',
        bad.errors === 1 && bad.diagnostics[0].line === 2 && bad.diagnostics[0].code.length > 0,
        JSON.stringify(bad.diagnostics));
  check('診断に直し方が付く', bad.errors === 1 && bad.diagnostics[0].help.length >= 0,
        JSON.stringify(bad.diagnostics[0] && bad.diagnostics[0].help));

  // --- 実行時に止まる ---
  const boom = run('func main() -> int {\n  var xs: list<int> = [];\n  print(xs[3]);\n  return 0;\n}\n');
  const err = JSON.parse(api.error());
  check('panic の理由が取れる', boom.status === 2 && err.message.length > 0 && err.line === 3,
        JSON.stringify(err));

  // --- 止まらない繰り返しでも、こちらから終われる ---
  api.config(64, 0, 0);
  api.load('forever.shk', 'func main() -> int { while true { } return 0; }');
  api.startRun();
  let running = true;
  for (let i = 0; i < 5; i++) running = api.pump(100000) === 0;
  check('無限ループは running のまま返る', running === true);
  api.abort();
  check('abort で止まる', api.pump(100000) === 2);

  // --- メモリの上限 ---
  api.config(8, 0, 0);
  api.load('mem.shk',
           'func main() -> int {\n  var xs: list<int> = [];\n  while true { xs.push(1); }\n  return 0;\n}\n');
  api.startRun();
  let memStatus = 0;
  for (let i = 0; i < 4000 && memStatus === 0; i++) memStatus = api.pump(200000);
  const memErr = JSON.parse(api.error());
  check('メモリの上限を超えたら実行時エラー', memStatus === 2 && memErr.message.indexOf('メモリ') >= 0,
        memStatus + ' ' + JSON.stringify(memErr));

  // --- テスト ---
  api.config(64, 0, 0);
  api.load('unit_test.shk', fs.readFileSync(path.join(root, 'tests/unit_test.shk'), 'utf8'));
  const found = api.startTest();
  const t = drive(10000);
  const results = JSON.parse(api.testResults());
  check('test_ で始まる関数を見つけて走らせる',
        found > 0 && t.status === 1 && api.testTotal() === found && results.length === found,
        found + ' 件中 ' + api.testPassed() + ' 件成功');
  check('全部のテストが通る', api.testPassed() === api.testTotal(),
        JSON.stringify(results.filter((r) => !r.ok)));


  // --- 画面（std.ui）---------------------------------------------------
  // まずは画面の無いところ。node には DOM が無いので、コアは見えない面に描く
  const blind = run('import std.ui;\n' +
                    'func main() -> int {\n' +
                    '  ui.open("t", 20, 10);\n' +
                    '  ui.clear(ui.rgb(240, 80, 60));\n' +
                    '  ui.present();\n' +
                    '  print(f"{ui.visible()} {ui.width()}x{ui.height()} {ui.get(5, 5)}");\n' +
                    '  ui.close();\n' +
                    '  return 0;\n' +
                    '}\n');
  check('画面が無ければ、見えない面に描く',
        blind.status === 1 && blind.out === 'false 20x10 15749180\n', blind.out);

  // ここから先は、要るところだけの偽 DOM を置いて、canvas に出す道を確かめる
  // （本物のブラウザで動かすところは web/README.md の「画面」）
  const dom = fakeDom();
  const uiSrc =
      'import std.ui;\n' +
      'var name = "";\n' +
      'var editing = false;\n' +
      'func main() -> int {\n' +
      '  ui.open("まど", 100, 80);\n' +
      '  print(f"open {ui.visible()} {ui.width()}x{ui.height()} scale {ui.scale()}");\n' +
      '  var n = 0;\n' +
      '  while ui.poll() {\n' +
      '    n += 1;\n' +
      '    if ui.typed() != "" { print(f"typed {ui.typed()}"); }\n' +
      '    if ui.pressed("left") { print("left"); }\n' +
      '    if ui.clicked(0) { print(f"click {ui.mouse_x()},{ui.mouse_y()}"); editing = true; }\n' +
      '    ui.clear(ui.rgb(240, 80, 60));\n' +
      '    _ = ui.show(ui.button("b", "b"), 40, 40);\n' +   // 形のテストで合わせる先
      '    if editing {\n' +
      '      name = ui.input(4, 20, 10, name);\n' +
      '      if name != "" { print(f"name {name}"); }\n' +
      '    }\n' +
      '    ui.present();\n' +
      '    sleep(0.01);\n' +
      '  }\n' +
      '  print(f"quit {n > 0}");\n' +
      '  ui.close();\n' +
      '  return 0;\n' +
      '}\n';
  api.config(64, 0, 0);
  api.load('ui.shk', uiSrc);
  api.startRun();

  // 何回か刻んで、そのつど出たものを返す
  function turn(times) {
    let out = '';
    let status = 0;
    for (let i = 0; i < (times || 8); i++) {
      status = api.pump(200000);
      out += take();
      if (status !== 0) break;
      nap(12);
    }
    return { out, status };
  }

  const opened = turn(6);
  check('canvas を開いて、開いたことを窓に知らせる',
        opened.out.indexOf('open true 100x80 scale 2') >= 0 && dom.opens.length === 1 &&
        dom.opens[0].width === 100 && dom.opens[0].canvas.width === 100,
        opened.out + ' / ' + dom.opens.length);

  // 面の色が、そのまま canvas の画素になっているか（並びは R G B A）
  const img = dom.ctx.last;
  check('面が canvas にそのまま出る',
        !!img && img.width === 100 && img.height === 80 &&
        img.data[0] === 240 && img.data[1] === 80 && img.data[2] === 60 && img.data[3] === 255,
        img ? [img.width, img.height, img.data[0], img.data[1], img.data[2], img.data[3]].join() : 'なし');

  dom.key(dom.canvas, 'keydown', 'a');
  dom.key(dom.canvas, 'keyup', 'a');
  dom.key(dom.canvas, 'keydown', 'ArrowLeft');
  dom.key(dom.canvas, 'keyup', 'ArrowLeft');
  const keyed = turn(4);
  check('打たれた文字と、特別なキーが届く',
        keyed.out.indexOf('typed a') >= 0 && keyed.out.indexOf('left') >= 0, keyed.out);

  dom.tap(30, 24);   // 面の画素で押す
  const tapped = turn(4);
  check('押した場所が、面の画素で届く', tapped.out.indexOf('click 30,24') >= 0, tapped.out);

  // 文字入力（IME）。ブラウザでは見えない textarea に任せる
  check('入力欄の間は、変換の受け皿ができる', !!dom.textarea(), '受け皿なし');
  const ta = dom.textarea();
  if (ta) ta.value = 'さめ';
  const named = turn(4);
  check('受け皿に入った文字が、そのまま渡る', named.out.indexOf('name さめ') >= 0, named.out);

  // --- マウスの形 --------------------------------------------------------
  // 押せるところに合わせたら手に、入力欄なら文字の形に変わる
  dom.hover(4, 4);           // 何も無いところ
  const c0 = turn(3);
  const cur0 = dom.cursor();
  dom.hover(46, 46);         // ボタンの上（上のプログラムが ui.show で置いている）
  const c1 = turn(3);
  check('押せるところに合わせると手になる', dom.cursor() === 'pointer',
        JSON.stringify([cur0, dom.cursor()]) + c0.out + c1.out);

  dom.close();   // 窓の × にあたるもの（requestClose）
  const quit = turn(30);
  check('閉じてくれと伝えると ui.poll() が false になる',
        quit.status === 1 && quit.out.indexOf('quit true') >= 0 && dom.closes === 1,
        quit.status + ' ' + quit.out);

  // --- 字（ブラウザに描いてもらう）---------------------------------------
  // 本物の canvas でどう出るかは web/README.md の「画面」。ここで見るのは、
  // 移植層の字（core/platform/font_canvas.inc）が std.ui につながっているか
  const font = run('import std.ui;\n' +
                   'func main() -> int {\n' +
                   '  ui.open("字", 80, 40);\n' +
                   '  var t = "あいう";\n' +
                   '  var w0 = ui.text_width(t);\n' +
                   '  print(f"builtin {ui.font_name()} {w0}");\n' +
                   '  print(f"open {ui.font(16)} {ui.font_name()}");\n' +
                   '  var w1 = ui.text_width(t);\n' +
                   '  print(f"wide {w1 > w0}");\n' +
                   '  ui.clear(ui.rgb(0, 0, 0));\n' +
                   '  ui.text(2, 2, "あ", ui.rgb(255, 255, 255));\n' +
                   '  var ink = false;\n' +
                   '  for var y in range(40) {\n' +
                   '    for var x in range(80) {\n' +
                   '      if ui.get(x, y) != 0 { ink = true; }\n' +
                   '    }\n' +
                   '  }\n' +
                   '  print(f"ink {ink}");\n' +
                   '  var miss = ui.font("ないフォント名", 16);\n' +
                   '  print(f"missing {miss}");\n' +
                   '  ui.font_builtin();\n' +
                   '  var w2 = ui.text_width(t);\n' +
                   '  print(f"back {ui.font_name()} {w2 == w0}");\n' +
                   '  ui.close();\n' +
                   '  return 0;\n' +
                   '}\n');
  const fl = font.out.split('\n');
  check('ui.font() でブラウザの字を使う', fl[1] === 'open true Hiragino Sans', font.out);
  check('その字の幅で数える', fl[0].indexOf('builtin  ') === 0 && fl[2] === 'wide true', font.out);
  check('面に字が描かれる', fl[3] === 'ink true', font.out);
  check('無い名前のフォントは false', fl[4] === 'missing false', font.out);
  check('ui.font_builtin() で内蔵に戻る', fl[5] === 'back  true', font.out);

  dom.restore();

  // --- 補完のもと（api.js）が、この処理系の中身と合っているか ---
  var api2 = loadApi();
  check('api.js ができている',
        api2 && Object.keys(api2.modules).length > 0 && api2.builtins.length > 0 && !!api2.methods.list);
  check('署名と説明が仕様書から入っている',
        find(api2.modules.math.members, 'sqrt').sig === 'math.sqrt(x: float) -> float' &&
        find(api2.modules.math.members, 'sqrt').doc.length > 0,
        JSON.stringify(find(api2.modules.math.members, 'sqrt')));
  check('型のメソッドが型検査の表から入っている',
        find(api2.methods.list, 'push').sig === 'push(v: T) -> void' &&
        find(api2.methods.map, 'get').sig === 'get(k: K) -> V?' &&
        !!find(api2.methods.list, 'sort'),
        JSON.stringify(find(api2.methods.list, 'push')));
  check('組み込みのオーバーロードも拾えている',
        (find(api2.builtins, 'len').overloads || []).length >= 3,
        JSON.stringify(find(api2.builtins, 'len')));
  check('print はどんな型でも、いくつでも受け取る',
        find(api2.builtins, 'print').sig === 'print(values: Any...) -> void',
        JSON.stringify(find(api2.builtins, 'print').sig));

  // --- 補完（lang.js）が、継承元の関数の雛形を出すか ---
  // Monaco の代わりに、lang.js が使うところだけを持った偽物を渡す
  function install() {
    global.window = {};
    delete require.cache[require.resolve('./lang.js')];
    require('./lang.js');
    var kinds = {};
    ('Text Method Function Constructor Field Variable Class Module Property Keyword Snippet Constant Event')
      .split(' ').forEach(function (k, i) { kinds[k] = i; });
    var providers = {};
    function keep(name) { return function (id, p) { providers[name] = p; }; }
    global.window.SharkLang.install({
      Range: function (a, b, c, d) {
        this.startLineNumber = a; this.startColumn = b; this.endLineNumber = c; this.endColumn = d;
      },
      languages: {
        CompletionItemKind: kinds, SymbolKind: kinds,
        CompletionItemInsertTextRule: { InsertAsSnippet: 4 },
        register: function () {}, setLanguageConfiguration: function () {},
        setMonarchTokensProvider: function () {},
        registerCompletionItemProvider: keep('completion'),
        registerHoverProvider: keep('hover'),
        registerSignatureHelpProvider: keep('signature'),
        registerDefinitionProvider: keep('definition'),
        registerDocumentSymbolProvider: keep('symbol')
      },
      editor: { defineTheme: function () {} }
    }, loadApi());
    return providers;
  }

  var modelCount = 0;
  function model(text) {
    var lines = text.split('\n');
    var id = 'test' + (++modelCount);          // lang.js は id と版で覚えるので、毎回変える
    return {
      id: id, uri: id,
      getValue: function () { return text; },
      getVersionId: function () { return 1; },
      getLineContent: function (n) { return lines[n - 1]; },
      getValueInRange: function (r) {
        if (r.startLineNumber === r.endLineNumber) {
          return lines[r.startLineNumber - 1].slice(r.startColumn - 1, r.endColumn - 1);
        }
        var out = [lines[r.startLineNumber - 1].slice(r.startColumn - 1)];
        for (var i = r.startLineNumber; i < r.endLineNumber - 1; i++) out.push(lines[i]);
        out.push(lines[r.endLineNumber - 1].slice(0, r.endColumn - 1));
        return out.join('\n');
      },
      getWordUntilPosition: function (p) {
        var w = (/[A-Za-z_]\w*$/.exec(lines[p.lineNumber - 1].slice(0, p.column - 1)) || [''])[0];
        return { word: w, startColumn: p.column - w.length, endColumn: p.column };
      },
      getWordAtPosition: function (p) {
        var w = this.getWordUntilPosition(p);
        return w.word ? w : null;
      }
    };
  }

  const lang = install();

  function complete(src, line, column) {
    return lang.completion.provideCompletionItems(
      model(src), { lineNumber: line, column: column }).suggestions;
  }
  function stubs(list) {       // 雛形は、いちばん上に出るように sortText が 0 で始まる
    return list.filter(function (s) { return (s.sortText || '')[0] === '0'; });
  }
  function stub(list, name) {
    return stubs(list).filter(function (s) { return s.label === name; })[0];
  }

  //  1 class Shape {
  //  2   virtual func area() -> float;
  //  3   virtual func label() -> string;
  //  4   virtual func draw() -> void { print("○"); }
  //  5   func id() -> int { return 1; }
  //  6 }
  //  8 class Circle : Shape, Comparable {
  // 10   override func label() -> string {
  // 13   ←ここで補完する
  const shapes = [
    'class Shape {',
    '  virtual func area() -> float;',
    '  virtual func label() -> string;',
    '  virtual func draw() -> void { print("○"); }',
    '  func id() -> int { return 1; }',
    '}',
    '',
    'class Circle : Shape, Comparable {',
    '  var r: float;',
    '  override func label() -> string {',
    '    return "円";',
    '  }',
    '  ',
    '}'
  ].join('\n');

  const inClass = complete(shapes, 13, 3);
  const names = stubs(inClass).map(function (s) { return s.label; }).sort();
  check('継承元の virtual が雛形として出る（書いたものと virtual でないものは出ない）',
        names.join(' ') === 'area compare draw', names.join(' '));
  check('純粋仮想は空の本体と、戻り値に合う値',
        (stub(inClass, 'area') || {}).insertText === 'override func area() -> float {\n\t$0\n\treturn 0.0;\n}',
        JSON.stringify((stub(inClass, 'area') || {}).insertText));
  check('実装のある親は super を呼ぶところまで書く',
        (stub(inClass, 'draw') || {}).insertText === 'override func draw() -> void {\n\t$0\n\tsuper.draw();\n}',
        JSON.stringify((stub(inClass, 'draw') || {}).insertText));
  check('インタフェースの This は自分のクラスになり、親の public も引き継ぐ',
        (stub(inClass, 'compare') || {}).insertText ===
        'public override func compare(other: Circle) -> int {\n\t$0\n\treturn 0;\n}',
        JSON.stringify((stub(inClass, 'compare') || {}).insertText));

  const typed = complete(shapes.replace('\n  \n}', '\n  override \n}'), 13, 12);
  check('override と打った後は、雛形だけを出して override を重ねない',
        typed.length > 0 && typed.length === stubs(typed).length &&
        (stub(typed, 'area') || {}).insertText === 'func area() -> float {\n\t$0\n\treturn 0.0;\n}',
        typed.length + ' 件 ' + JSON.stringify((stub(typed, 'area') || {}).insertText));

  const withPublic = complete(shapes.replace('\n  \n}', '\n  public \n}'), 13, 10);
  check('public と打った後は、public を重ねない',
        (stub(withPublic, 'compare') || {}).insertText ===
        'override func compare(other: Circle) -> int {\n\t$0\n\treturn 0;\n}',
        JSON.stringify((stub(withPublic, 'compare') || {}).insertText));

  check('メソッドの中では雛形を出さない', stubs(complete(shapes, 11, 5)).length === 0,
        stubs(complete(shapes, 11, 5)).map(function (s) { return s.label; }).join(' '));

  //  1 class Dog {
  //  2   public var name: string;
  //  3   var secret: int;
  //  6   public func walk() -> void { }
  //  7   func hidden() -> void { }
  //  9 class Pup : Dog {
  // 13   p. ←ここで補完する
  const dogs = [
    'class Dog {',
    '  public var name: string;',
    '  var secret: int;',
    '  func init() { }',
    '  public virtual func bark() -> string { return "wan"; }',
    '  public func walk() -> void { }',
    '  func hidden() -> void { }',
    '}',
    'class Pup : Dog {',
    '  public func nap() -> void { }',
    '  public override func bark() -> string { return "kyan"; }',
    '}',
    'func main() -> int {',
    '  var p = Pup();',
    '  p.',
    '  return 0;',
    '}'
  ].join('\n');

  const dot = complete(dogs, 15, 5).map(function (x) { return x.label; }).sort();
  check('「.」の後ろに、親から受け継いだ public なメンバも出る',
        dot.join(' ') === 'bark name nap walk', dot.join(' '));

  const fishSrc = fs.readFileSync(path.join(root, 'examples/fish.shk'), 'utf8');
  const fishLines = fishSrc.split('\n');
  const sharkLine = fishLines.indexOf('class Shark : Fish {') + 2;
  const inShark = complete(fishSrc.replace('class Shark : Fish {', 'class Shark : Fish {\n  '), sharkLine, 3);
  // Shark は describe をもう上書きしている。親をたどると Fish の compare が残る
  check('お手本（fish.shk）でも、親をたどった compare が雛形として出る',
        (stub(inShark, 'compare') || {}).insertText ===
        'override func compare(other: Fish) -> int {\n\t$0\n\treturn super.compare(other);\n}' &&
        !stub(inShark, 'describe') && !stub(inShark, 'to_string'),
        stubs(inShark).map(function (x) { return x.label; }).join(' ') + ' / ' +
        JSON.stringify((stub(inShark, 'compare') || {}).insertText));

  // --- 補完の一覧と、実際に持っているモジュールがずれていないか ---
  api.config(64, 0, 0);
  api.load('m.shk', 'print("x");');
  var runtimeMods = JSON.parse(api.modules())
    .map(function (m) { return m.replace(/^std\./, ''); })
    .filter(function (m) { return m !== 'builtin'; });
  var onlyInApi = Object.keys(api2.modules).filter(function (m) { return runtimeMods.indexOf(m) < 0; });
  var onlyInRuntime = runtimeMods.filter(function (m) { return !api2.modules[m]; });
  check('補完のモジュールと、処理系が持つモジュールが一致する',
        onlyInApi.length === 0 && onlyInRuntime.length === 0,
        'api.js だけ: ' + onlyInApi.join(' ') + ' / 処理系だけ: ' + onlyInRuntime.join(' '));

  // --- モジュールと説明 ---
  api.config(64, 0, 0);
  api.load('m.shk', 'print("x");');
  const mods = JSON.parse(api.modules());
  check('モジュールの一覧が取れる', mods.length > 0 && mods.indexOf('std.math') >= 0, mods.join(' '));
  check('エラー番号の説明が取れる', api.explain('E0102').length > 0);
  check('無い番号は空', api.explain('E9999') === '');

  // --- 英語の診断 ---
  api.config(64, 1, 0);
  api.load('en.shk', 'func main() -> int { var a = 1 + "x"; return 0; }');
  const en = JSON.parse(api.diagnostics());
  check('--lang en にあたる切り替えが効く', en.length > 0 && /[a-z]/.test(en[0].message),
        en.length ? en[0].message : '');

  console.log(failed === 0 ? '\nぜんぶ通りました' : '\n' + failed + ' 件が失敗しました');
  process.exit(failed === 0 ? 0 : 1);
});

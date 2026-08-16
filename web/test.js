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

createShark().then((M) => {
  const api = {
    boot: M.cwrap('shk_boot', null, []),
    config: M.cwrap('shk_config', null, ['number', 'number', 'number']),
    load: M.cwrap('shk_load', 'number', ['string', 'string']),
    diagnostics: M.cwrap('shk_diagnostics', 'string', []),
    hasEntry: M.cwrap('shk_has_entry', 'number', []),
    pushInput: M.cwrap('shk_push_input', null, ['string']),
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

  // 1回に 20 万命令ずつ、最大 maxMs ミリ秒まで進める
  function drive(maxMs) {
    const start = Date.now();
    let out = '';
    while (Date.now() - start < maxMs) {
      const status = api.pump(200000);
      out += take();
      if (status !== 0) return { status, out };
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

  // --- 入力 ---
  const ask = run(fs.readFileSync(path.join(root, 'web/examples/ask.shk'), 'utf8'), 'さめ\n');
  check('input() が読める', ask.out.indexOf('こんにちは、さめ さん！') >= 0, ask.out);

  const noInput = run(fs.readFileSync(path.join(root, 'web/examples/ask.shk'), 'utf8'));
  check('入力が無いときは none', noInput.out.indexOf('入力がありませんでした') >= 0, noInput.out);

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

  // --- 補完のもと（api.js）が、この処理系の中身と合っているか ---
  var api2 = loadApi();
  check('api.js ができている',
        api2 && Object.keys(api2.modules).length > 0 && api2.builtins.length > 0 && !!api2.methods.list);
  check('署名と説明が仕様書から入っている',
        find(api2.modules.math.members, 'sqrt').sig === 'math.sqrt(x: float) -> float' &&
        find(api2.modules.math.members, 'sqrt').doc.length > 0,
        JSON.stringify(find(api2.modules.math.members, 'sqrt')));
  check('型のメソッドが型検査の表から入っている',
        find(api2.methods.list, 'push').sig === 'push(T) -> void' &&
        find(api2.methods.map, 'get').sig === 'get(K) -> V?' &&
        !!find(api2.methods.list, 'sort'),
        JSON.stringify(find(api2.methods.list, 'push')));
  check('組み込みのオーバーロードも拾えている',
        (find(api2.builtins, 'print').overloads || []).length >= 3,
        JSON.stringify(find(api2.builtins, 'print')));

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

// screen_canvas.inc — ブラウザの canvas に出す画面（web.cpp から読む）
//
// 移植層に求められているのは3つだけ（platform.h）。ここでは canvas を1枚こしらえて、
//   1. 面をそのまま流し込む     present
//   2. DOM の出来事を渡す       poll（待たない）
//   3. 開ける・閉じる           open / close
// を埋める。描くのはコアの側なので、ここが持つのは画素を運ぶ道と、出来事の訳し方だけ。
//
//   ・ホストが場所を決めていなければ、**窓をこしらえる**。名札の帯・× ・持ち手が付いていて、
//     引けば動かせるし、隅を引けば大きさも変えられる（机の上の窓と同じ）。
//     出す先を決めたいときは Module.sharkMount か #shark-screen を置く（その中に面だけ出る）。
//     開いた・閉じたは window の shark:screen-open / shark:screen-close で知らせる
//   ・変換つきの文字入力（かな漢字）は、見えない textarea をブラウザに任せて、
//     確定した文字と変換中の文字だけを受け取る（spec/library/ui.md）
//   ・出来事は JavaScript 側の列にたまり、poll() が1つずつ取る。待たない
//
// 画面の無いところ（node で動かすときや SHARK_UI=off）では open が false を返し、
// コアは見えない面に描く。
namespace canvas {

// ------------------------------------------------------------ ブラウザとの境目
// EM_JS で書いたものは、C からは普通の関数に見える。持ち物は Module.__shark_screen。

// 出来事の番号とキーの番号は、C の側（platform.h）が正。開く前に一度だけ渡す
EM_JS(void, cv_kinds, (int close, int key, int text, int mouse, int resize), {
  Module.__shark_ev = ({ close: close, key: key, text: text, mouse: mouse, resize: resize });
});
EM_JS(void, cv_key, (const char* name, int code), {
  if (!Module.__shark_keys) Module.__shark_keys = ({});
  Module.__shark_keys[UTF8ToString(name)] = code;
});

// 画面の細かさ（HiDPI なら 2）。**開く前にも呼べる**
EM_JS(int, cv_scale, (), {
  if (typeof window === 'undefined') return 1;
  var k = Math.round(window.devicePixelRatio || 1);
  if (!(k >= 1)) k = 1;
  return k > 4 ? 4 : k;
});

// 貼り付け（Ctrl+V）で届いたものを覚えておく置き場。窓が無くても使えるので、
// 持ち物とは別にしてある。ブラウザからは、こちらから読みに行けない
EM_JS(void, cv_clip_init, (), {
  if (Module.__shark_clip_on || typeof document === 'undefined') return;
  Module.__shark_clip_on = true;
  if (Module.__shark_clip === undefined) Module.__shark_clip = "";
  document.addEventListener('paste', function (e) {
    if (e.clipboardData) Module.__shark_clip = e.clipboardData.getData('text') || "";
  }, true);
});


// 窓をこしらえる。見た目は Dear ImGui に寄せてある ― 角は立てたまま、細い縁、
// 帯は濃い青（焦点が外れると黒に近くなる）、右下に三角の持ち手。
// ここが作るのは**窓枠だけ**で、中身（面）は canvas が受け持つ
EM_JS(void, cv_window_init, (), {
  // 窓を作る手だてを覚えさせるだけ。DOM を触るのは、実際に呼ばれたとき
  if (Module.__shark_make_window) return;
  Module.__shark_make_window = function (title, cw, ch) {
    // Dear ImGui の暗い配色（imgui.cpp の StyleColorsDark から）
    var C = ({
      bg: '#0f0f0f',                       // WindowBg
      border: 'rgba(110,110,128,0.50)',    // Border
      title: '#0a0a0a',                    // TitleBg（焦点が無いとき）
      titleOn: '#294a7a',                  // TitleBgActive
      text: '#ffffff',
      dim: 'rgba(255,255,255,0.60)',
      grip: 'rgba(66,150,250,0.35)',       // ResizeGrip
      gripOn: 'rgba(66,150,250,0.90)',     // ResizeGripHovered
      hot: 'rgba(66,150,250,1.00)'         // ButtonHovered
    });
    var BAR = 22;                          // 名札の帯の高さ（CSS の点）
    var win = ({ el: null, box: null, onClose: null, onResize: null });

    var el = document.createElement('div');
    el.style.cssText = 'position:fixed;z-index:2147483000;display:flex;flex-direction:column;' +
        'border:1px solid ' + C.border + ';background:' + C.bg + ';color:' + C.text + ';' +
        'box-shadow:0 10px 40px rgba(0,0,0,.5);' +
        'font:12px/1.5 system-ui,-apple-system,"Hiragino Sans","Noto Sans JP",sans-serif';

    var bar = document.createElement('div');
    bar.style.cssText = 'display:flex;align-items:center;gap:6px;flex:none;height:' + BAR + 'px;' +
        'padding:0 4px 0 6px;background:' + C.titleOn + ';cursor:move;user-select:none';

    // たたむ印。ImGui と同じで、押すと中身が隠れて帯だけになる
    var arrow = document.createElement('span');
    arrow.textContent = '▼';
    arrow.style.cssText = 'flex:none;width:13px;text-align:center;font-size:9px;cursor:pointer;' +
        'color:' + C.dim;
    arrow.title = 'たたむ';

    var label = document.createElement('span');
    label.textContent = title;
    label.style.cssText = 'flex:1;min-width:0;overflow:hidden;white-space:nowrap;' +
        'text-overflow:ellipsis';

    var shut = document.createElement('span');
    shut.textContent = '✕';
    shut.title = '閉じる';
    shut.setAttribute('role', 'button');
    shut.setAttribute('aria-label', '閉じる');
    shut.style.cssText = 'flex:none;width:15px;height:15px;line-height:15px;text-align:center;' +
        'font-size:10px;border-radius:2px;cursor:pointer;color:' + C.dim;
    shut.addEventListener('pointerdown', function (e) { e.stopPropagation(); });
    shut.addEventListener('pointerenter', function () {
      shut.style.background = C.hot;
      shut.style.color = '#fff';
    });
    shut.addEventListener('pointerleave', function () {
      shut.style.background = 'transparent';
      shut.style.color = C.dim;
    });
    shut.addEventListener('click', function () { if (win.onClose) win.onClose(); });

    bar.appendChild(arrow);
    bar.appendChild(label);
    bar.appendChild(shut);

    var box = document.createElement('div');
    box.style.cssText = 'flex:1;min-height:0;display:flex;align-items:center;' +
        'justify-content:center;overflow:hidden;position:relative;background:#000';

    // 右下の持ち手。ImGui と同じ三角。面の入れ物ではなく窓に付ける
    // （box の中身は開くたびに空にされるので）
    var grip = document.createElement('div');
    grip.style.cssText = 'position:absolute;right:0;bottom:0;width:14px;height:14px;z-index:4;' +
        'cursor:nwse-resize;background:linear-gradient(315deg,' + C.grip + ' 42%,transparent 42%)';
    grip.addEventListener('pointerenter', function () {
      grip.style.background = 'linear-gradient(315deg,' + C.gripOn + ' 42%,transparent 42%)';
    });
    grip.addEventListener('pointerleave', function () {
      if (!size) grip.style.background = 'linear-gradient(315deg,' + C.grip + ' 42%,transparent 42%)';
    });

    el.appendChild(bar);
    el.appendChild(box);
    el.appendChild(grip);

    // 出す大きさ。画面からはみ出さないところまで
    var vw = window.innerWidth || 1024;
    var vh = window.innerHeight || 768;
    var ww = Math.max(120, Math.min(cw, vw - 40));
    var wh = Math.max(80, Math.min(ch, vh - 40 - BAR));
    el.style.width = ww + 'px';
    el.style.height = (wh + BAR) + 'px';
    el.style.left = Math.max(10, Math.round((vw - ww) / 2)) + 'px';
    el.style.top = Math.max(10, Math.round((vh - wh - BAR) / 2)) + 'px';
    document.body.appendChild(el);

    // いま触っている窓かどうかで帯の色が変わる（ImGui の TitleBgActive）。
    // 焦点の出来事だけでは足りない（窓に焦点の来ない出し先もある）ので、
    // 押されたところも見る
    function focused(on) { bar.style.background = on ? C.titleOn : C.title; }
    el.addEventListener('focusin', function () { focused(true); });
    el.addEventListener('focusout', function (e) {
      if (!e.relatedTarget || !el.contains(e.relatedTarget)) focused(false);
    });
    el.addEventListener('pointerdown', function () { focused(true); }, true);
    var away = function (e) { if (!el.contains(e.target)) focused(false); };
    document.addEventListener('pointerdown', away, true);

    // たたむ・ひらく
    var folded = false;
    var openH = wh + BAR;
    function fold(on) {
      folded = on;
      arrow.textContent = on ? '▶' : '▼';
      arrow.title = on ? 'ひらく' : 'たたむ';
      if (on) {
        openH = el.offsetHeight;
        box.style.display = 'none';
        grip.style.display = 'none';
        el.style.height = BAR + 'px';
      } else {
        box.style.display = 'flex';
        grip.style.display = win.resizable === false ? 'none' : 'block';
        el.style.height = openH + 'px';
      }
    }
    arrow.addEventListener('pointerdown', function (e) { e.stopPropagation(); });
    arrow.addEventListener('click', function () { fold(!folded); });
    bar.addEventListener('dblclick', function () { fold(!folded); });

    // 帯を引くと動く
    var move = null;
    bar.addEventListener('pointerdown', function (e) {
      if (e.button !== 0) return;
      move = ({ x: e.clientX - el.offsetLeft, y: e.clientY - el.offsetTop });
      if (bar.setPointerCapture) { try { bar.setPointerCapture(e.pointerId); } catch (err) {} }
      e.preventDefault();
    });
    bar.addEventListener('pointermove', function (e) {
      if (!move) return;
      var vw2 = window.innerWidth || 1024;
      var vh2 = window.innerHeight || 768;
      var nx = Math.min(vw2 - 80, Math.max(80 - el.offsetWidth, e.clientX - move.x));
      var ny = Math.min(vh2 - BAR, Math.max(0, e.clientY - move.y));
      el.style.left = nx + 'px';
      el.style.top = ny + 'px';
    });
    bar.addEventListener('pointerup', function () { move = null; });
    bar.addEventListener('pointercancel', function () { move = null; });

    // 隅を引くと大きさが変わる。面はこのあと SEV_Resize で追いつく
    var size = null;
    grip.addEventListener('pointerdown', function (e) {
      if (e.button !== 0) return;
      size = ({ x: e.clientX, y: e.clientY, w: el.offsetWidth, h: el.offsetHeight });
      if (grip.setPointerCapture) { try { grip.setPointerCapture(e.pointerId); } catch (err) {} }
      e.preventDefault();
      e.stopPropagation();
    });
    grip.addEventListener('pointermove', function (e) {
      if (!size) return;
      el.style.width = Math.max(120, size.w + (e.clientX - size.x)) + 'px';
      el.style.height = Math.max(60 + BAR, size.h + (e.clientY - size.y)) + 'px';
      if (win.onResize) win.onResize();
      e.stopPropagation();
    });
    grip.addEventListener('pointerup', function () { size = null; });
    grip.addEventListener('pointercancel', function () { size = null; });

    win.el = el;
    win.box = box;
    win.resizable = true;
    win.setTitle = function (s) { label.textContent = s; };
    win.setResizable = function (on) {
      win.resizable = on;
      grip.style.display = (on && !folded) ? 'block' : 'none';
    };
    win.dispose = function () {
      move = null;
      size = null;
      document.removeEventListener('pointerdown', away, true);
    };
    return win;
  };
});

EM_JS(int, cv_open, (const char* title, int w, int h, int zoom, int dpi), {
  if (typeof document === 'undefined' || !document.body) return 0;   // node には画面が無い
  var EV = Module.__shark_ev;
  var KEYS = Module.__shark_keys;
  var name = UTF8ToString(title);

  // --- 出す先を決める ---
  // ホストが場所を決めていればそこへ（面だけ出す）。決めていなければ窓をこしらえる
  var mount = Module.sharkMount;
  if (typeof mount === 'string') mount = document.querySelector(mount);
  if (!mount) mount = document.getElementById('shark-screen');
  var win = null;
  if (!mount) {
    win = Module.__shark_make_window(name, w * zoom / dpi, h * zoom / dpi);
    mount = win.box;
  }
  // 変換の受け皿を面の上に置くので、位置の基準になってもらう
  if (window.getComputedStyle(mount).position === 'static') mount.style.position = 'relative';
  mount.textContent = "";   // 前に出した絵は捨てる

  var c = document.createElement('canvas');
  c.width = w;
  c.height = h;
  c.tabIndex = 0;                       // キーを受け取れるようにする
  c.setAttribute('aria-label', name);
  // 面の1画素は画面の1画素。小さい面だけ zoom 倍に引き伸ばす（ぼかさない）
  c.style.cssText = 'display:block;outline:none;touch-action:none;image-rendering:pixelated;' +
      'width:' + (w * zoom / dpi) + 'px;height:' + (h * zoom / dpi) + 'px';
  mount.appendChild(c);

  // --- 持ち物 ---
  var S = ({});
  S.mount = mount;
  S.win = win;      // こしらえた窓（ホストが場所を決めていれば null）
  S.canvas = c;
  S.ctx = c.getContext('2d');
  S.img = null;      // 面を流し込む入れ物（present で作る）
  S.u32 = null;
  S.q = [];          // 出来事の列。poll が1つずつ取る
  S.sw = w;
  S.sh = h;
  S.rw = w;          // 最後に知らせた面の大きさ
  S.rh = h;
  S.boxw = 0;        // 出す先の大きさ（CSS の点）。0 はまだ測っていない
  S.boxh = 0;
  S.zoom = zoom;
  S.dpi = dpi;
  S.resizable = true;
  S.textOn = false;
  S.ta = null;       // 文字入力の受け皿（text_input で作る）
  S.comp = null;     // 変換中のようす
  S.held = ({});     // いま押されているキー（焦点が外れたら離したことにする）
  S.listeners = [];
  Module.__shark_screen = S;

  // 面の大きさ（画素）÷ 画面の細かさ × 引き伸ばし = 出したい大きさ。
  // 出す先がそれより狭ければ、縦横の比を保ったまま縮めて収める（窓と違って、板の
  // 大きさはこちらでは決められない）
  function fit() {
    var dw = S.sw * S.zoom / S.dpi;   // 出したい大きさ（CSS の点）
    var dh = S.sh * S.zoom / S.dpi;
    var r = S.mount.getBoundingClientRect();
    var k = 1;
    if (r.width >= 1 && r.height >= 1) k = Math.min(1, r.width / dw, r.height / dh);
    S.canvas.style.width = (dw * k) + "px";
    S.canvas.style.height = (dh * k) + "px";
  }
  S.fit = fit;

  function push(e) {
    if (S.q.length > 512) S.q.shift();   // 取りに来られないほど来たら、古いものから捨てる
    S.q.push(e);
  }
  function ev(kind) { return ({ kind: kind, code: 0, down: false, x: 0, y: 0, text: "" }); }
  function on(target, kind, fn) {
    target.addEventListener(kind, fn);
    S.listeners.push([target, kind, fn]);
  }
  // --- マウス（指も同じ道を通る） ---
  function at(e) {   // 画面の点 → 面の画素
    var r = c.getBoundingClientRect();
    var x = Math.floor((e.clientX - r.left) * S.sw / (r.width || 1));
    var y = Math.floor((e.clientY - r.top) * S.sh / (r.height || 1));
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= S.sw) x = S.sw - 1;
    if (y >= S.sh) y = S.sh - 1;
    return ([x, y]);
  }
  function mouse(e, code, down) {
    var p = at(e);
    var m = ev(EV.mouse);
    m.code = code;
    m.down = down;
    m.x = p[0];
    m.y = p[1];
    push(m);
  }
  function button(e) { return e.button === 1 ? 1 : (e.button === 2 ? 2 : 0); }

  on(c, 'pointerdown', function (e) {
    if (c.setPointerCapture) { try { c.setPointerCapture(e.pointerId); } catch (err) {} }
    if (!S.textOn) c.focus();   // 入力欄に入れている間は、受け皿に持たせたまま
    mouse(e, button(e), true);
    e.preventDefault();
  });
  on(c, 'pointerup', function (e) { mouse(e, button(e), false); });
  on(c, 'pointermove', function (e) { mouse(e, -1, false); });
  on(c, 'pointercancel', function (e) { mouse(e, button(e), false); });
  on(c, 'contextmenu', function (e) { e.preventDefault(); });   // 右で押すのは ui.menu のもの

  // --- キー ---
  // 面と受け皿はどちらも入れ物の中にあるので、まとめて入れ物で受ける
  var PREVENT = ({ ArrowLeft: 1, ArrowRight: 1, ArrowUp: 1, ArrowDown: 1, PageUp: 1,
                   PageDown: 1, Home: 1, End: 1, Tab: 1, Enter: 1, Backspace: 1, ' ': 1 });
  function keyOf(e) {
    var k = KEYS[e.key];
    if (k !== undefined) return k;
    var t = e.key;
    if (!t || Array.from(t).length !== 1) return -1;
    var cp = t.codePointAt(0);
    if (cp >= 65 && cp <= 90) cp += 32;      // 英字は小文字で数える
    return (cp >= 32 && cp < 127) ? cp : -1;
  }
  function onKey(e) {
    // 変換の最中のキーは変換のもの。ここで渡すと、確定した拍子に入力欄から出てしまう
    if (e.isComposing || e.keyCode === 229) return;
    var down = (e.type === 'keydown');
    var k = keyOf(e);
    if (k >= 0) {
      var q = ev(EV.key);
      q.code = k;
      q.down = down;
      push(q);
      if (down) S.held[k] = 1;
      else delete S.held[k];
    }
    // 打たれた文字。入力欄に入れている間は、受け皿（textarea）が受け取る
    if (down && !S.textOn && !e.ctrlKey && !e.metaKey && !e.altKey) {
      var t = e.key;
      if (t && Array.from(t).length === 1) {
        var cp = t.codePointAt(0);
        if (cp >= 32 && cp !== 127) {
          var q2 = ev(EV.text);
          q2.text = t;
          push(q2);
        }
      }
    }
    // 面が受け取ったキーは、ブラウザの既定の動き（巻物・焦点の移動）に渡さない
    if (!S.textOn && !e.ctrlKey && !e.metaKey && !e.altKey && PREVENT[e.key]) e.preventDefault();
  }
  function release() {   // 焦点が外れた。押しっぱなしのままにしない
    for (var k in S.held) {
      var q = ev(EV.key);
      q.code = Number(k);
      q.down = false;
      push(q);
    }
    S.held = ({});
  }
  on(mount, 'keydown', onKey);
  on(mount, 'keyup', onKey);
  on(mount, 'focusout', function (e) {
    if (!e.relatedTarget || !mount.contains(e.relatedTarget)) release();
  });
  on(window, 'blur', release);

  // --- 出す先の大きさが変わったら、窓の縁を引かれたのと同じに扱う ---
  function onBox() {
    fit();
    if (!S.resizable) return;
    var r = mount.getBoundingClientRect();
    if (r.width < 1 || r.height < 1) return;
    if (!(S.boxw > 0)) {   // 初めて測れた。ここを元の大きさとして覚える
      S.boxw = r.width;
      S.boxh = r.height;
      return;
    }
    if (Math.abs(r.width - S.boxw) < 1 && Math.abs(r.height - S.boxh) < 1) return;
    S.boxw = r.width;
    S.boxh = r.height;
    var nw = Math.round(r.width * S.dpi / S.zoom);
    var nh = Math.round(r.height * S.dpi / S.zoom);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw === S.rw && nh === S.rh) return;
    S.rw = nw;
    S.rh = nh;
    // まだ取られていない前の知らせは差し替える（引いている間にたまらないように）
    if (S.q.length && S.q[S.q.length - 1].kind === EV.resize) S.q.pop();
    var q = ev(EV.resize);
    q.x = nw;
    q.y = nh;
    push(q);
  }
  if (typeof ResizeObserver !== 'undefined') {
    S.ro = new ResizeObserver(onBox);
    S.ro.observe(mount);
  } else {
    on(window, 'resize', onBox);
  }
  var r0 = mount.getBoundingClientRect();
  if (r0.width >= 1 && r0.height >= 1) {
    S.boxw = r0.width;
    S.boxh = r0.height;
    fit();
  }

  // --- ホストへの知らせ ---
  // 閉じてくれと頼む口。窓の × と、ホストが出すボタンの両方から呼ばれる
  S.requestClose = function () { push(ev(EV.close)); };
  if (win) {
    win.onClose = S.requestClose;
    win.onResize = onBox;          // 隅を引いている間も、面を追いつかせる
  }
  Module.sharkScreen = ({ canvas: c, mount: mount, title: name, width: w, height: h,
                          requestClose: S.requestClose });
  if (typeof CustomEvent === 'function')
    window.dispatchEvent(new CustomEvent('shark:screen-open', ({ detail: Module.sharkScreen })));
  c.focus();
  return 1;
});

// 閉じる。出した絵は、こちらで入れ物を作ったときだけ片づける。
// ホストが場所を用意しているときは、最後の絵をそのまま残す（見えたままのほうがよい）
EM_JS(void, cv_close, (), {
  var S = Module.__shark_screen;
  if (!S) return;
  for (var i = 0; i < S.listeners.length; i++)
    S.listeners[i][0].removeEventListener(S.listeners[i][1], S.listeners[i][2]);
  if (S.ro) S.ro.disconnect();
  if (S.ta && S.ta.parentNode) S.ta.parentNode.removeChild(S.ta);
  if (S.win) {
    S.win.dispose();
    if (S.win.el.parentNode) S.win.el.parentNode.removeChild(S.win.el);
  }
  Module.__shark_screen = null;
  Module.sharkScreen = null;
  if (typeof CustomEvent === 'function')
    window.dispatchEvent(new CustomEvent('shark:screen-close', ({ detail: ({ canvas: S.canvas }) })));
});

// 面を canvas に流し込む。面は 0x00RRGGBB の並び、canvas は R G B A の並び
EM_JS(void, cv_present, (const uint32_t* px, int w, int h), {
  var S = Module.__shark_screen;
  if (!S || !S.ctx) return;
  if (S.sw !== w || S.sh !== h || !S.img) {   // 面の大きさが変わった
    S.canvas.width = w;
    S.canvas.height = h;
    S.img = S.ctx.createImageData(w, h);
    S.u32 = new Uint32Array(S.img.data.buffer);
    S.sw = w;
    S.sh = h;
    S.rw = w;
    S.rh = h;
    if (S.fit) S.fit();   // 出す先に入る大きさにする
    if (Module.sharkScreen) {
      Module.sharkScreen.width = w;
      Module.sharkScreen.height = h;
    }
  }
  var n = w * h;
  var src = new Uint32Array(HEAPU8.buffer, px, n);
  var dst = S.u32;
  for (var i = 0; i < n; i++) {
    var v = src[i];
    // 0x00RRGGBB → 0xAABBGGRR（並びが逆になる）
    dst[i] = 0xff000000 | ((v & 0xff) << 16) | (v & 0xff00) | ((v >>> 16) & 0xff);
  }
  S.ctx.putImageData(S.img, 0, 0);
});

// 出来事を1つ取る。無ければ 0。**待たない**
EM_JS(int, cv_poll, (int* f, char* text), {
  var S = Module.__shark_screen;
  if (!S || !S.q.length) return 0;
  var e = S.q.shift();
  HEAP32[(f >> 2) + 0] = e.kind;
  HEAP32[(f >> 2) + 1] = e.code;
  HEAP32[(f >> 2) + 2] = e.down ? 1 : 0;
  HEAP32[(f >> 2) + 3] = e.x;
  HEAP32[(f >> 2) + 4] = e.y;
  stringToUTF8(e.text || "", text, 8);
  return 1;
});

// --- 文字入力（IME）------------------------------------------------------
// 変換は OS とブラウザが持っている。見えない textarea に任せて、結果だけ受け取る
EM_JS(void, cv_text_input, (int on, const char* initial, int x, int y, int h), {
  var S = Module.__shark_screen;
  if (!S) return;
  if (!on) {
    if (S.ta) {
      // 焦点は面に返す。どこにも無いままだと、キーがどこにも届かなくなる
      if (document.activeElement === S.ta) S.canvas.focus();
      S.ta.style.display = 'none';
    }
    S.textOn = false;
    S.comp = null;
    return;
  }
  if (!S.ta) {
    var ta = document.createElement('textarea');
    ta.rows = 1;
    ta.spellcheck = false;
    ta.setAttribute('autocomplete', 'off');
    ta.setAttribute('autocapitalize', 'off');
    ta.setAttribute('autocorrect', 'off');
    ta.setAttribute('aria-label', 'ui.field');
    // 見えない受け皿。透かすのは見た目だけで、キーも変換も今までどおり受け取れる。
    // 変換中の候補は、ここに置いた場所のあたりにブラウザが出す
    ta.style.cssText = 'position:absolute;z-index:2;padding:0;margin:0;border:0;outline:none;' +
        'resize:none;overflow:hidden;white-space:nowrap;background:transparent;' +
        'color:transparent;caret-color:transparent';
    ta.addEventListener('compositionstart', function () {
      S.comp = ({ start: ta.selectionStart, len: 0 });
    });
    ta.addEventListener('compositionupdate', function (e) {
      if (S.comp) S.comp.len = (e.data || "").length;
    });
    ta.addEventListener('compositionend', function () { S.comp = null; });
    ta.addEventListener('paste', function (e) {
      if (e.clipboardData) Module.__shark_clip = e.clipboardData.getData('text') || "";
    });
    S.mount.appendChild(ta);
    S.ta = ta;
  }
  var k = S.zoom / S.dpi;   // 面の画素 → CSS の点
  S.ta.style.display = 'block';
  S.ta.style.left = (S.canvas.offsetLeft + x * k) + 'px';
  S.ta.style.top = (S.canvas.offsetTop + y * k) + 'px';
  S.ta.style.width = '1px';
  S.ta.style.height = (h * k) + 'px';
  S.ta.style.fontSize = (h * k * 0.8) + 'px';   // 変換の窓の大きさを、入力欄に合わせる
  S.ta.style.lineHeight = (h * k) + 'px';
  if (initial) {
    var v = UTF8ToString(initial);
    if (S.ta.value !== v) {
      S.ta.value = v;
      S.ta.selectionStart = S.ta.selectionEnd = v.length;
    }
  }
  S.textOn = true;
  // 焦点を取り戻すのは、面か、どこにも無いときだけ。ページの他のところからは奪わない
  var ae = document.activeElement;
  if (ae !== S.ta && (ae === S.canvas || ae === document.body || !ae)) S.ta.focus();
});

// いまの中身。confirmed に確定した文字列、marked に変換中の文字列
EM_JS(int, cv_text_state, (char* conf, int conf_cap, char* mark, int mark_cap), {
  var S = Module.__shark_screen;
  if (!S || !S.textOn || !S.ta) return 0;
  var v = S.ta.value;
  var m = "";
  if (S.comp && S.comp.len > 0) {   // 変換中のところだけ切り分ける
    m = v.substr(S.comp.start, S.comp.len);
    v = v.slice(0, S.comp.start) + v.slice(S.comp.start + S.comp.len);
  }
  var clean = function (s) { return s.split('\n').join("").split('\r').join(""); };
  var put = function (s, ptr, cap) {
    s = clean(s);
    while (s.length && lengthBytesUTF8(s) >= cap) s = s.slice(0, -1);   // 入る分だけ
    stringToUTF8(s, ptr, cap);
  };
  put(v, conf, conf_cap);
  put(m, mark, mark_cap);
  return 1;
});

// 選んでいるところ。単位は文字の数（UTF-16 の数え方とは違う）
EM_JS(int, cv_text_selection, (int* out), {
  var S = Module.__shark_screen;
  if (!S || !S.textOn || !S.ta) return 0;
  var v = S.ta.value;
  var count = function (s) { return Array.from(s).length; };
  var a = count(v.slice(0, S.ta.selectionStart));
  var b = count(v.slice(0, S.ta.selectionEnd));
  HEAP32[(out >> 2) + 0] = a;
  HEAP32[(out >> 2) + 1] = b - a;
  return 1;
});

EM_JS(void, cv_text_select, (int start, int len), {
  var S = Module.__shark_screen;
  if (!S || !S.textOn || !S.ta) return;
  var a = Array.from(S.ta.value);
  var at = function (n) {
    var u = 0;
    for (var i = 0; i < n && i < a.length; i++) u += a[i].length;
    return u;
  };
  if (start < 0) start = 0;
  if (len < 0) len = 0;
  S.ta.selectionStart = at(start);
  S.ta.selectionEnd = at(start + len);
});

// 選んでいるところを置き換える（貼り付けと切り取り）。変換中は触らない
EM_JS(void, cv_text_replace, (const char* s), {
  var S = Module.__shark_screen;
  if (!S || !S.textOn || !S.ta || S.comp) return;
  var ins = UTF8ToString(s);
  var v = S.ta.value;
  var a = S.ta.selectionStart;
  var b = S.ta.selectionEnd;
  S.ta.value = v.slice(0, a) + ins + v.slice(b);
  S.ta.selectionStart = S.ta.selectionEnd = a + ins.length;
});

// --- 切り貼りの置き場 -----------------------------------------------------
// ブラウザからは、置き場を勝手に読めない。貼り付け（Ctrl+V）で届いたものと、
// 自分で入れたものを覚えておく
EM_JS(int, cv_clip_get, (char* buf, int cap), {
  var s = Module.__shark_clip;
  if (typeof s !== 'string') return 0;
  while (s.length && lengthBytesUTF8(s) >= cap) s = s.slice(0, -1);
  stringToUTF8(s, buf, cap);
  return 1;
});
EM_JS(void, cv_clip_set, (const char* s), {
  var v = UTF8ToString(s);
  Module.__shark_clip = v;
  if (typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText) {
    try { navigator.clipboard.writeText(v).catch(function () {}); } catch (e) {}
  }
});

// マウスの形。CSS の名前に直して canvas に付ける
EM_JS(void, cv_set_cursor, (const char* css), {
  var S = Module.__shark_screen;
  if (S && S.canvas) S.canvas.style.cursor = UTF8ToString(css);
});

EM_JS(void, cv_set_resizable, (int on), {
  var S = Module.__shark_screen;
  if (!S) return;
  S.resizable = !!on;
  if (S.win) S.win.setResizable(!!on);   // 隅の持ち手を出す・引っ込める
});

// ------------------------------------------------------------ 移植層の口
struct G {
  bool open;
  int sw, sh;      // 面の大きさ
  int zoom;        // 面の1画素を画面の何画素にするか（小さい面だけ引き伸ばす）
  int dpi;         // 画面の細かさ
  bool resizable;
  G() : open(false), sw(0), sh(0), zoom(1), dpi(1), resizable(true) {}
};
G g;

bool same(const char* a, const char* b) {
  int i = 0;
  for (; a[i] && b[i]; i++)
    if (a[i] != b[i]) return false;
  return a[i] == b[i];
}

// SHARK_UI で画面を切れるようにしておく（desktop.cpp と同じ見方）
bool ui_off() {
  const char* p = getenv("SHARK_UI");
  if (!p || !p[0]) return false;
  return same(p, "off") || same(p, "none");
}

int screen_scale() {
  if (ui_off()) return 1;
  int k = cv_scale();
  return k < 1 ? 1 : k;
}

// 面が小さいときだけ、整数倍に広げる（大きい面は画面の画素と1対1）
int pick_zoom(int w, int h) {
  int s = 1;
  while (s < 6 && w * (s + 1) <= 640 && h * (s + 1) <= 480) s++;
  return s;
}

void init_js() {
  static bool done = false;
  if (done) return;
  done = true;
  cv_kinds(SEV_Close, SEV_Key, SEV_Text, SEV_Mouse, SEV_Resize);
  // DOM の KeyboardEvent.key から、こちらのキーの番号にする表。
  // 印字できる文字は、その ASCII をそのまま使う（JavaScript の側で数える）
  struct { const char* name; int code; } m[] = {
      {"ArrowLeft", SKEY_Left}, {"ArrowRight", SKEY_Right}, {"ArrowUp", SKEY_Up},
      {"ArrowDown", SKEY_Down}, {"Enter", SKEY_Enter}, {"Escape", SKEY_Escape},
      {"Tab", SKEY_Tab}, {"Backspace", SKEY_Back}, {"Delete", SKEY_Delete},
      {"Home", SKEY_Home}, {"End", SKEY_End}, {"PageUp", SKEY_PageUp},
      {"PageDown", SKEY_PageDown}, {"Shift", SKEY_Shift}, {"Control", SKEY_Ctrl},
      {"Alt", SKEY_Alt}};
  for (int i = 0; i < (int)(sizeof m / sizeof m[0]); i++) cv_key(m[i].name, m[i].code);
  for (int i = 0; i < 12; i++) {   // F1〜F12
    char nm[4];
    nm[0] = 'F';
    if (i < 9) { nm[1] = (char)('1' + i); nm[2] = 0; }
    else { nm[1] = '1'; nm[2] = (char)('0' + (i - 9)); nm[3] = 0; }
    cv_key(nm, SKEY_F1 + i);
  }
  cv_clip_init();
  cv_window_init();
}

bool open(const char* title, int w, int h) {
  if (g.open) return false;
  if (ui_off()) return false;
  init_js();
  int zoom = pick_zoom(w, h);
  int dpi = cv_scale();
  if (dpi < 1) dpi = 1;
  if (!cv_open((title && title[0]) ? title : "Shark", w, h, zoom, dpi)) return false;
  g.open = true;
  g.sw = w;
  g.sh = h;
  g.zoom = zoom;
  g.dpi = dpi;
  g.resizable = true;
  return true;
}

void close() {
  if (!g.open) return;
  g.open = false;
  cv_close();
}

void present(const uint32_t* px, int w, int h) {
  if (!g.open) return;
  g.sw = w;
  g.sh = h;
  cv_present(px, w, h);
}

bool poll(ScreenEvent* out) {
  if (!g.open) return false;
  int f[5];
  char text[8];
  f[0] = f[1] = f[2] = f[3] = f[4] = 0;
  text[0] = 0;
  if (!cv_poll(f, text)) return false;
  ScreenEvent e;
  e.kind = f[0];
  e.code = f[1];
  e.down = f[2] != 0;
  e.x = f[3];
  e.y = f[4];
  for (int i = 0; i < 7; i++) e.text[i] = text[i];
  e.text[7] = 0;
  *out = e;
  return true;
}

void text_input(bool on, const char* initial, int x, int y, int h) {
  if (!g.open) return;
  cv_text_input(on ? 1 : 0, initial, x, y, h);
}

bool text_state(Str* confirmed, Str* marked) {
  if (!g.open) return false;
  char conf[4096], mark[512];
  conf[0] = 0;
  mark[0] = 0;
  if (!cv_text_state(conf, (int)sizeof conf, mark, (int)sizeof mark)) return false;
  *confirmed = Str(conf);
  *marked = Str(mark);
  return true;
}

bool text_selection(int* start, int* len) {
  if (!g.open) return false;
  int r[2];
  r[0] = r[1] = 0;
  if (!cv_text_selection(r)) return false;
  *start = r[0];
  *len = r[1];
  return true;
}

void text_select(int start, int len) {
  if (g.open) cv_text_select(start, len);
}

void text_replace(const char* s) {
  if (g.open && s) cv_text_replace(s);
}

bool clipboard_get(Str* out) {
  init_js();
  char buf[8192];
  buf[0] = 0;
  if (!cv_clip_get(buf, (int)sizeof buf)) return false;
  *out = Str(buf);
  return true;
}

void clipboard_set(const char* s) {
  if (!s) return;
  init_js();
  cv_clip_set(s);
}

void set_resizable(bool on) {
  if (!g.open) return;
  g.resizable = on;
  cv_set_resizable(on ? 1 : 0);
}

// CSS の名前（developer.mozilla.org の cursor）に直して渡す
void set_cursor(int kind) {
  if (!g.open) return;
  const char* css = "default";
  switch (kind) {
    case SCUR_Hand: css = "pointer"; break;
    case SCUR_Text: css = "text"; break;
    case SCUR_Cross: css = "crosshair"; break;
    case SCUR_Wait: css = "wait"; break;
    case SCUR_ResizeX: css = "ew-resize"; break;
    case SCUR_ResizeY: css = "ns-resize"; break;
    case SCUR_Move: css = "grab"; break;
    case SCUR_None: css = "none"; break;
    default: break;
  }
  cv_set_cursor(css);
}

}  // namespace canvas

// canvas は「離した」も出せる。変換つきの文字入力は、見えない textarea に任せる。
// 窓の縁を引いている間の描き直し（set_redraw）は要らない ― ブラウザは止まらない
const PlatformScreen kCanvasScreen = {
    canvas::screen_scale,   canvas::open,           canvas::close,
    canvas::present,        canvas::poll,           true,
    canvas::text_input,     canvas::text_state,     canvas::text_selection,
    canvas::text_select,    canvas::text_replace,   canvas::clipboard_get,
    canvas::clipboard_set,
    0,                      // 窓の縁を引いている間の描き直しは要らない（ブラウザは止まらない）
    canvas::set_cursor,     canvas::set_resizable,
    true};                  // 刻みは requestAnimationFrame が握っている（app.js）

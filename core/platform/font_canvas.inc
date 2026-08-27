// font_canvas.inc — ブラウザに字を描いてもらう（web.cpp から読む）
//
// ブラウザの中からは、フォントのファイルを読めない。かわりに
// **canvas に1文字ずつ描いてもらい、その濃さ（alpha）を写し取る**。
// 返す形は FreeType と同じ（platform.h の PlatformGlyph）なので、
// コアから見れば「機種のフォントが読めた」のと変わらない（spec/library/ui.md）。
//
//   ・使う字は下の kFamilies。Windows・macOS・Linux のどれでも日本語が出るものを、
//     上から順に並べてある。canvas は**字ごとに**、持っているものへ落ちていく
//   ・一度描いた字形は覚えておく（JavaScript 側。cp と大きさが鍵）
//   ・描くのは1文字ずつだが、覚え書きが効くので、動かしている間に描き直すのは初回だけ
namespace webfont {

// 機種にありそうなフォント。上から順に見て、最初に見つかったものを名前として答える。
// どれも「その機種にはじめから入っている、日本語の出るゴシック」
static const char* const kFamilies[] = {
    "Hiragino Sans",              // macOS / iOS（ヒラギノ角ゴシック）
    "Hiragino Kaku Gothic ProN",
    "Yu Gothic UI",               // Windows 10 以降（游ゴシック UI）
    "Yu Gothic",
    "Meiryo",                     // Windows Vista 以降（メイリオ）
    "MS Gothic",                  // ずっとある（MS ゴシック）
    "Noto Sans CJK JP",           // Linux / Android / ChromeOS
    "Noto Sans JP",
    "IPAexGothic",
    "IPAGothic",
    "VL Gothic",
    "Droid Sans Japanese",
    0};

// ------------------------------------------------------------ ブラウザとの境目

// 上の一覧を JavaScript 側に渡す（開く前に一度だけ）
EM_JS(void, cf_family, (const char* name), {
  if (!Module.__shark_families) Module.__shark_families = [];
  Module.__shark_families.push(UTF8ToString(name));
});

// 字を描く支度をする。want が 0 なら、機種にあるものに任せる。
// 名指しされたものが無ければ 0（ui.font() が false を返す）
EM_JS(int, cf_open, (const char* want, int px), {
  if (typeof document === 'undefined' || !document.createElement) return 0;
  var fams = Module.__shark_families || [];
  var S = Module.__shark_font;
  if (!S) {
    S = ({});
    var c = document.createElement('canvas');
    c.width = 64;
    c.height = 64;
    S.canvas = c;
    S.ctx = c.getContext('2d', ({ willReadFrequently: true }));
    if (!S.ctx) return 0;
    S.cache = new Map();

    // その名前のフォントがあるか。ブラウザは document.fonts.check では
    // 何でも true と答えるので、**幅を比べて**確かめる
    S.has = function (fam) {
      var probe = 'あいうえおMWil漢字';
      var base = ['monospace', 'serif'];
      for (var i = 0; i < base.length; i++) {
        S.ctx.font = '72px ' + base[i];
        var w0 = S.ctx.measureText(probe).width;
        S.ctx.font = '72px "' + fam + '",' + base[i];
        var w1 = S.ctx.measureText(probe).width;
        if (Math.abs(w1 - w0) > 0.5) return true;
      }
      return false;
    };

    // 1文字ぶんの字形。濃さの並びと、置き場所（数え方は FreeType と同じ）
    S.raster = function (cp, px2) {
      var ch = String.fromCodePoint(cp);
      var ctx = S.ctx;
      var font = px2 + 'px ' + S.stack;
      ctx.font = font;
      var m = ctx.measureText(ch);
      var adv = Math.round(m.width);
      var l = Math.ceil(m.actualBoundingBoxLeft);     // 起点から左へ（右なら負）
      var r = Math.ceil(m.actualBoundingBoxRight);
      var a = Math.ceil(m.actualBoundingBoxAscent);   // 基準線から上へ
      var d = Math.ceil(m.actualBoundingBoxDescent);
      var w = l + r + 2;
      var h = a + d + 2;
      var empty = ({ w: 0, h: 0, left: 0, top: 0, adv: adv, data: null });
      if (!(w > 0) || !(h > 0) || w > 256 || h > 256) return empty;
      if (S.canvas.width < w || S.canvas.height < h) {
        S.canvas.width = Math.max(S.canvas.width, w);
        S.canvas.height = Math.max(S.canvas.height, h);
      }
      ctx.font = font;               // 大きさを変えると canvas の設定は消える
      ctx.textBaseline = 'alphabetic';
      ctx.fillStyle = '#fff';
      ctx.clearRect(0, 0, w, h);
      ctx.fillText(ch, l + 1, a + 1);   // 起点をここに置いて描く
      var src = ctx.getImageData(0, 0, w, h).data;
      var out = new Uint8Array(w * h);
      var ink = 0;
      for (var i = 0; i < w * h; i++) {
        out[i] = src[i * 4 + 3];       // 濃さは alpha
        ink |= out[i];
      }
      if (!ink) return empty;          // 空白など
      return ({ w: w, h: h, left: -(l + 1), top: a + 1, adv: adv, data: out });
    };
    Module.__shark_font = S;
  }

  var name = want ? UTF8ToString(want) : "";
  if (name) {
    if (!S.has(name)) return 0;        // 名指しされたものが無い
    S.stack = '"' + name + '",sans-serif';
    S.name = name;
  } else {
    var quoted = [];
    var found = "";
    for (var i = 0; i < fams.length; i++) {
      quoted.push('"' + fams[i] + '"');
      if (!found && S.has(fams[i])) found = fams[i];
    }
    quoted.push('system-ui');
    quoted.push('sans-serif');
    S.stack = quoted.join(',');
    S.name = found || 'sans-serif';
  }
  S.cache.clear();                     // 字の出どころが変わった
  S.px = px;
  return 1;
});

EM_JS(void, cf_close, (), {
  var S = Module.__shark_font;
  if (!S) return;
  S.cache.clear();
  S.stack = "";
  S.name = "";
});

EM_JS(int, cf_name, (char* buf, int cap), {
  var S = Module.__shark_font;
  var s = (S && S.name) ? S.name : "";
  while (s.length && lengthBytesUTF8(s) >= cap) s = s.slice(0, -1);
  stringToUTF8(s, buf, cap);
  return 1;
});

// 1行ぶんの高さ。out[0] に基準線から上、out[1] に下
EM_JS(int, cf_metrics, (int px, int* out), {
  var S = Module.__shark_font;
  if (!S || !S.stack) return 0;
  S.ctx.font = px + 'px ' + S.stack;
  var m = S.ctx.measureText('Mあgj');
  var a = Math.ceil(m.fontBoundingBoxAscent || m.actualBoundingBoxAscent || px * 0.88);
  var d = Math.ceil(m.fontBoundingBoxDescent || m.actualBoundingBoxDescent || px * 0.22);
  HEAP32[(out >> 2) + 0] = a;
  HEAP32[(out >> 2) + 1] = d;
  return 1;
});

// 字形を1つ。meta に [w, h, left, top, adv]、bits に濃さの並び
EM_JS(int, cf_glyph, (int cp, int px, int* meta, unsigned char* bits, int cap), {
  var S = Module.__shark_font;
  if (!S || !S.stack) return 0;
  var key = cp * 512 + (px & 511);
  var g = S.cache.get(key);
  if (g === undefined) {
    g = S.raster(cp, px);
    if (S.cache.size > 4000) S.cache.clear();   // ためすぎない
    S.cache.set(key, g);
  }
  var n = g.w * g.h;
  if (n > cap) return 0;
  if (n > 0 && g.data) HEAPU8.set(g.data, bits);
  HEAP32[(meta >> 2) + 0] = g.w;
  HEAP32[(meta >> 2) + 1] = g.h;
  HEAP32[(meta >> 2) + 2] = g.left;
  HEAP32[(meta >> 2) + 3] = g.top;
  HEAP32[(meta >> 2) + 4] = g.adv;
  return 1;
});

// ------------------------------------------------------------ 移植層の口

// 字形の置き場。移植層の持ちもので、次に glyph を呼ぶまで有効（platform.h）
static unsigned char g_bits[256 * 256];
static Str g_name;
static bool g_on = false;
static int g_asc = 0, g_desc = 0, g_metrics_px = 0;   // 高さは覚えておく（毎回測らない）

static bool measure(int px) {
  if (px == g_metrics_px) return true;
  int r[2];
  r[0] = r[1] = 0;
  if (!cf_metrics(px, r)) return false;
  g_asc = r[0];
  g_desc = r[1];
  g_metrics_px = px;
  return true;
}

bool f_open(const char* name, int px) {
  static bool listed = false;
  if (!listed) {   // 選んだフォントの一覧を、JavaScript 側に渡しておく
    listed = true;
    for (int i = 0; kFamilies[i]; i++) cf_family(kFamilies[i]);
  }
  if (!cf_open(name, px)) return false;
  char buf[128];
  buf[0] = 0;
  cf_name(buf, (int)sizeof buf);
  g_name = Str(buf);
  g_metrics_px = 0;
  g_on = true;
  return true;
}

void f_close() {
  if (!g_on) return;
  cf_close();
  g_on = false;
  g_name.clear();
  g_metrics_px = 0;
}

const char* f_name() { return g_name.c_str(); }

bool f_glyph(int cp, int px, PlatformGlyph* out) {
  if (!g_on) return false;
  int meta[5];
  meta[0] = meta[1] = meta[2] = meta[3] = meta[4] = 0;
  if (!cf_glyph(cp, px, meta, g_bits, (int)sizeof g_bits)) return false;
  out->w = meta[0];
  out->h = meta[1];
  out->left = meta[2];
  out->top = meta[3];
  out->adv = meta[4];
  out->bits = (out->w > 0 && out->h > 0) ? g_bits : 0;
  return true;
}

int f_line_height(int px) {
  if (!g_on || !measure(px)) return px;
  int h = g_asc + g_desc;
  return h > 0 ? h : px;
}

int f_ascender(int px) {
  if (!g_on || !measure(px)) return px;
  return g_asc;
}

}  // namespace webfont

const PlatformFont kCanvasFont = {webfont::f_open,   webfont::f_close,
                                  webfont::f_name,   webfont::f_glyph,
                                  webfont::f_line_height, webfont::f_ascender};

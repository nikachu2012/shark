// ui.cpp — std.ui（spec/library/ui.md）
//
// 画面に出すための、いちばん下の層。
//   ・描くのは全部ここ（点・線・四角・円・文字）。外の描画の道具には頼らない
//   ・移植層に求めるのは「面を画面に出す」と「起きた出来事を渡す」の2つだけ
//     （core/platform/platform.h の PlatformScreen）
//   ・画面を持たない機種でも、**見えない面**に同じように描ける。
//     描いた結果は ui.get() と ui.to_png() で取り出せるので、テストにも使える
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

#include "font5x7.inc"

// FreeType のヘッダは、**名前空間の外**で読む。
// 字を描く中身（font_ft.inc）は namespace shark { の中で取り込むので、そこで読むと
// 連れてくる <stdlib.h> などまで shark:: の中に入ってしまい、std:: が壊れる。
#if defined(SHARK_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace shark {

// 字形を FreeType で出す口（入っていなければ中身は空）
#include "font_ft.inc"
// PNG を読む（展開も自前。書き出しは下の encode_png）
#include "png.inc"

static Value* A(Value* args, int i) { return val_deref(&args[i]); }

// ------------------------------------------------------------------ 持ちもの
// 面は1つだけ。ゲームに組み込むときも、画面は1つしか無いため
static bool g_open = false;
static bool g_visible = false;     // 画面に出せているか（出せなければ見えない面）
static int g_w = 0, g_h = 0;
static uint32_t* g_px = 0;
static int g_cx0 = 0, g_cy0 = 0, g_cx1 = 0, g_cy1 = 0;   // 切り抜き（両端を含む）
static bool g_quit = false;

// 見た目の既定値（spec/library/ui.md）。ui.theme() で変えられる
static uint32_t g_bg = 0x14141c, g_fg = 0xe6e6e6, g_accent = 0x4c9be8;

static bool g_key[SKEY_Max], g_press[SKEY_Max], g_rel[SKEY_Max], g_hit[SKEY_Max];
static Str g_typed;
static int g_mx = 0, g_my = 0;
static bool g_mb[3], g_mpress[3], g_mrel[3];
// この回に車輪（ホイール）が送られたぶん。**1/100 行**で、下と右が正
// （移植層がここまで細かく渡す。platform.h）。
// 巻物を持つ部品（一覧・複数行の入力欄・メニュー）が使うと、そこで 0 に戻る
static int g_wheel_x = 0, g_wheel_y = 0;
// ui.wheel() が返す**行の数**。1行に満たないぶんは g_wheel_sub に持ち越す
static int g_wheel_lines_x = 0, g_wheel_lines_y = 0;
static int g_wheel_sub_x = 0, g_wheel_sub_y = 0;
// 部品が画素に直すときの、1画素に満たないぶんのため置き（1/100 画素）。
// これが無いと、ゆっくり動かしたぶんが毎回切り捨てられて、いつまでも動かない
static int g_wheel_px_sub = 0;
// マウスの形。毎回 ui.poll() で「ふつう」に戻り、その回に頼まれた形が
// ui.present() で機種に伝わる（頼まれるのは ui.cursor() と、宣言的な層の部品）
static int g_cursor_want = SCUR_Arrow;
static int g_cursor_set = SCUR_Arrow;

static void reset_input() {
  for (int i = 0; i < SKEY_Max; i++) { g_key[i] = false; g_press[i] = false; g_rel[i] = false; g_hit[i] = false; }
  for (int i = 0; i < 3; i++) { g_mb[i] = false; g_mpress[i] = false; g_mrel[i] = false; }
  g_typed.clear();
  g_mx = g_my = 0;
  g_wheel_x = g_wheel_y = 0;
  g_wheel_lines_x = g_wheel_lines_y = 0;
  g_wheel_sub_x = g_wheel_sub_y = 0;
  g_wheel_px_sub = 0;
}

static void ui_reset_widgets();   // 宣言的な層の覚えごとを消す（下で定義）

static void input_stop();   // 下で定義
static void font_close();   // 下で定義（字の出どころを内蔵に戻す）
static bool ui_live_redraw(int w, int h);   // 下で定義（窓の縁を引いている間の置き直し）

static void depth_free();   // 下で定義（奥行きの面）

// 次のこまの刻限（単調時計のナノ秒）。0 はまだ数え始めていない（ui.frame）
static int64_t g_frame_at = 0;

static void drop_surface() {
  g_frame_at = 0;
  input_stop();
  depth_free();
  // 形を変えたまま終わらない（窓が閉じても、あとに残る機種がある）
  if (g_visible && platform().screen && platform().screen->set_cursor && g_cursor_set != SCUR_Arrow)
    platform().screen->set_cursor(SCUR_Arrow);
  g_cursor_want = SCUR_Arrow;
  g_cursor_set = SCUR_Arrow;
  if (g_visible && platform().screen) platform().screen->close();
  g_visible = false;
  if (g_px) { sk_free(g_px); g_px = 0; }
  g_open = false;
  g_w = g_h = 0;
  g_quit = false;
  reset_input();
  ui_reset_widgets();
}

// 処理系を捨てるときに呼ばれる（registry.h）。
// 画面を開いたまま終わっても、面と字形は返しておく
void ui_shutdown() {
  drop_surface();
  font_close();   // 読んだフォントと、字形の覚え書きも返す
}

static bool need_open(VM& vm) {
  if (g_open) return true;
  vm.panic(vm.L("先に ui.open(横, 縦) で面を用意します",
                "call ui.open(width, height) first"));
  return false;
}

// 窓の大きさが変わった（SEV_Resize）。面を同じ大きさに作り直す。
// 前の中身は左上に残し、広がったところは下地の色にする。切り抜きは面いっぱいに戻る
static void resize_surface(int w, int h) {
  if (!g_open || !g_px) return;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (w > 16384) w = 16384;
  if (h > 16384) h = 16384;
  if (w == g_w && h == g_h) return;
  size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
  // 使ってよいメモリに入らないなら、面は今の大きさのまま（窓には引き伸ばして出る）
  if (sk_mem_limit() != 0 && bytes > sk_mem_limit()) return;
  uint32_t* np = (uint32_t*)sk_alloc(bytes);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      np[(size_t)y * (size_t)w + (size_t)x] =
          (x < g_w && y < g_h) ? g_px[(size_t)y * (size_t)g_w + (size_t)x] : g_bg;
  sk_free(g_px);
  g_px = np;
  g_w = w;
  g_h = h;
  g_cx0 = 0; g_cy0 = 0; g_cx1 = g_w - 1; g_cy1 = g_h - 1;
}

// ------------------------------------------------------------------ 描く土台
// 色は int 1つ。下から青・緑・赤の 8 ビットずつ（0xRRGGBB）で、
// **いちばん上のバイトは「透け具合」**。0 なら不透明、255 でまるごと透明。
//
// 上を「不透明のぶん（アルファ）」ではなく「透け具合」にしてあるのは、
// そうすると 0 が不透明になり、ui.rgb() の返す数が今までと1つも変わらないため。
// 書く人が見るのは ui.rgba() / ui.alpha() の**不透明のぶん**（255 が不透明）で、
// この裏返しは処理系の中だけの話（docs/implementation.md）
static inline uint32_t to_color(int64_t v) { return (uint32_t)(v & 0xffffffffu); }
static inline int clearness(uint32_t c) { return (int)(c >> 24); }

// 下地 dst の上に src を重ねた色。どちらも「透け具合」を上のバイトに持つ。
// 透ける絵の上に透ける絵を重ねても正しくなるよう、まじめに解いてある
static uint32_t over(uint32_t dst, uint32_t src) {
  int st = (int)(src >> 24);
  if (st == 0) return src;      // src が不透明なら、そのまま置き換わる
  if (st == 255) return dst;    // まるごと透明なら、何も起きない
  int sa = 255 - st;                    // src の不透明のぶん
  int da = 255 - (int)(dst >> 24);      // 下地の不透明のぶん
  int keep = da * (255 - sa) / 255;     // 下地のうち、透けて見えるぶん
  int oa = sa + keep;                   // 出来上がりの不透明のぶん
  if (oa <= 0) return 0xff000000u;      // どちらも透明なら、透明のまま
  uint32_t o = (uint32_t)(255 - oa) << 24;
  for (int sh = 16; sh >= 0; sh -= 8) {
    int s = (int)((src >> sh) & 0xff), d = (int)((dst >> sh) & 0xff);
    o |= (uint32_t)((s * sa + d * keep) / oa) << sh;
  }
  return o;
}

// 半透明なら下地と混ぜて置く。線・四角・円・文字・三角・貼るはこちらを通る
static inline void put(int x, int y, uint32_t c) {
  if (x < g_cx0 || x > g_cx1 || y < g_cy0 || y > g_cy1) return;
  uint32_t* p = &g_px[y * g_w + x];
  *p = (c >> 24) ? over(*p, c) : c;   // 不透明はそのまま書く（今までと同じ速さ）
}

// 混ぜずに、その色をそのまま置く。ui.set / ui.clear / ui.blit はこちら。
// **これが無いと「透明にする」が書けない**（透明を重ねても、下地が残るだけなので）。
// 絵を直す道具の消しゴムは 絵.set(x, y, ui.rgba(0,0,0,0))
static inline void put_raw(int x, int y, uint32_t c) {
  if (x < g_cx0 || x > g_cx1 || y < g_cy0 || y > g_cy1) return;
  g_px[y * g_w + x] = c;
}

// 切り抜きの中に収めてから、横一列を塗る
static void span(int x, int y, int w, uint32_t c) {
  if (y < g_cy0 || y > g_cy1) return;
  int x0 = x, x1 = x + w - 1;
  if (x0 < g_cx0) x0 = g_cx0;
  if (x1 > g_cx1) x1 = g_cx1;
  uint32_t* row = g_px + (size_t)y * (size_t)g_w;
  if (c >> 24) {
    if ((c >> 24) == 255) return;   // まるごと透明
    for (int i = x0; i <= x1; i++) row[i] = over(row[i], c);
    return;
  }
  for (int i = x0; i <= x1; i++) row[i] = c;
}

// 混ぜずに、その色をそのまま並べる（span の置き換え版）
static void span_raw(int x, int y, int w, uint32_t c) {
  if (y < g_cy0 || y > g_cy1) return;
  int x0 = x, x1 = x + w - 1;
  if (x0 < g_cx0) x0 = g_cx0;
  if (x1 > g_cx1) x1 = g_cx1;
  uint32_t* row = g_px + (size_t)y * (size_t)g_w;
  for (int i = x0; i <= x1; i++) row[i] = c;
}

// ------------------------------------------------------------------ 面
static NativeStatus u_open(VM& vm, Value* a, int n, Value& out) {
  Str title;
  int64_t w, h;
  bool resizable = true;   // 省いたときは、縁を引いて大きさを変えられる窓
  if (n >= 3) {
    title = as_str(*A(a, 0))->s; w = A(a, 1)->i; h = A(a, 2)->i;
    if (n >= 4) resizable = A(a, 3)->b;
  } else {
    title = Str("Shark"); w = A(a, 0)->i; h = A(a, 1)->i;
  }
  if (w <= 0 || h <= 0) {
    vm.panic(vm.L("面の大きさは 1 以上にします", "surface size must be 1 or more"));
    return N_Panic;
  }
  if (w > 16384 || h > 16384) {
    vm.panic(vm.L("面が大きすぎます（縦横とも 16384 まで）",
                  "surface too large (up to 16384 on each side)"));
    return N_Panic;
  }
  size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
  if (sk_mem_limit() != 0 && bytes > sk_mem_limit()) {
    vm.panic(vm.L("面が大きすぎて、使ってよいメモリに入りません",
                  "the surface does not fit in the memory limit"));
    return N_Panic;
  }
  drop_surface();

  g_px = (uint32_t*)sk_alloc(bytes);
  for (size_t i = 0; i < (size_t)w * (size_t)h; i++) g_px[i] = 0;
  g_w = (int)w;
  g_h = (int)h;
  g_cx0 = 0; g_cy0 = 0; g_cx1 = g_w - 1; g_cy1 = g_h - 1;
  g_open = true;

  const PlatformScreen* s = platform().screen;
  // 画面が無い機種や、窓を開けないところでは false が返る。
  // そのときは見えない面に描くだけになる
  g_visible = s && s->open(title.c_str(), g_w, g_h);
  // 窓の縁を引いている間もこちらで描き直せるように、口があれば渡しておく
  if (g_visible && s->set_redraw) s->set_redraw(ui_live_redraw);
  // 大きさを変えられない窓を頼まれたときは、移植層に伝える（持たない機種では素通し）
  if (g_visible && s->set_resizable) s->set_resizable(resizable);
  out = mk_void();
  return N_Ok;
}

static NativeStatus u_close(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  drop_surface();
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_width(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n; out = mk_int(g_w); return N_Ok;
}
static NativeStatus u_height(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n; out = mk_int(g_h); return N_Ok;
}
// 画面の細かさ。HiDPI（Retina）なら 2。**開く前でも呼べる**
static NativeStatus u_scale(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  const PlatformScreen* s = platform().screen;
  int k = (s && s->scale) ? s->scale() : 1;
  out = mk_int(k < 1 ? 1 : k);
  return N_Ok;
}
// 画面の細かさ（丸めない）。1.5 のような半端な数もそのまま返す。
// 持っていない機種では、丸めた数をそのまま渡す
static NativeStatus u_pixel_ratio(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  const PlatformScreen* s = platform().screen;
  double r = 1.0;
  if (s && s->pixel_ratio) r = s->pixel_ratio();
  else if (s && s->scale) r = (double)s->scale();
  if (!(r >= 1.0)) r = 1.0;
  if (r > 4.0) r = 4.0;
  out = mk_float(r);
  return N_Ok;
}
static NativeStatus u_visible(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n; out = mk_bool(g_visible); return N_Ok;
}
// この回に頼まれたマウスの形を、変わったときだけ機種に伝える
static void cursor_flush() {
  const PlatformScreen* s = platform().screen;
  if (!g_visible || !s || !s->set_cursor) return;
  if (g_cursor_want == g_cursor_set) return;
  g_cursor_set = g_cursor_want;
  s->set_cursor(g_cursor_set);
}

static NativeStatus u_present(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  cursor_flush();
  if (g_visible && platform().screen) platform().screen->present(g_px, g_w, g_h);
  out = mk_void();
  return N_Ok;
}

// 次のこまの刻限まで待つ。
//
// 「1こま描いてから 16ms 眠る」と書くと、**描くのにかかった分だけ足が出る**。
// ここは眠る長さではなく**刻限**を決めて、そこまで待つので、描くのが速い機種でも
// 遅い機種でも同じ速さで進む。
//
// ホストが刻みを握っている機種（ブラウザの requestAnimationFrame）では、
// 刻限の少し手前で起きる。刻限のすぐ後ろに起きると、その回のきっかけに間に合わず
// 次のきっかけまで丸ごと待つことになり、速さが半分になるため。
// 半こま手前にしてあるのは、描くのにその半分より長くかかっていれば、
// どのみち間に合っていないから（spec/library/ui.md「こまの速さ」）
static NativeStatus u_frame(VM& vm, Value* a, int n, Value& out) {
  if (!need_open(vm)) return N_Panic;
  TaskState* t = vm.task();
  if (t->cancel_req) { t->wake_at = 0; return N_Cancel; }
  if (t->wake_at == 0) {
    const int64_t fps = n > 0 ? A(a, 0)->i : 60;
    if (fps < 1 || fps > 1000) {
      vm.panic(vm.L("こまの速さは 1〜1000 にします", "frame rate must be between 1 and 1000"));
      return N_Panic;
    }
    const int64_t period = 1000000000LL / fps;
    const int64_t now = platform().monotonic_nanos();
    if (g_frame_at == 0) g_frame_at = now;
    g_frame_at += period;
    if (g_frame_at < now) g_frame_at = now;   // 追いつけていない。刻限を取り直す
    const PlatformScreen* sc = g_visible ? platform().screen : 0;
    const int64_t lead = (sc && sc->host_paced) ? period / 2 : 0;
    const int64_t target = g_frame_at - lead;
    if (target <= now) { out = mk_void(); return N_Ok; }
    t->wake_at = target;
    return N_Wait;
  }
  if (platform().monotonic_nanos() >= t->wake_at) {
    t->wake_at = 0;
    out = mk_void();
    return N_Ok;
  }
  return N_Wait;
}

// ------------------------------------------------------------------ 色
static NativeStatus u_rgb(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t r = A(a, 0)->i, g = A(a, 1)->i, b = A(a, 2)->i;
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  out = mk_int((r << 16) | (g << 8) | b);
  return N_Ok;
}
// 赤緑青に「不透明のぶん」を足して色を作る。a は 255 で不透明、0 でまるごと透明。
// 中では裏返して「透け具合」で持つので、a=255 のときは ui.rgb() と同じ数になる
static NativeStatus u_rgba(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t r = A(a, 0)->i, g = A(a, 1)->i, b = A(a, 2)->i, al = A(a, 3)->i;
  if (r < 0) r = 0; if (r > 255) r = 255;
  if (g < 0) g = 0; if (g > 255) g = 255;
  if (b < 0) b = 0; if (b > 255) b = 255;
  if (al < 0) al = 0; if (al > 255) al = 255;
  out = mk_int(((255 - al) << 24) | (r << 16) | (g << 8) | b);
  return N_Ok;
}
// 不透明のぶん（0〜255）。255 が不透明。ui.rgb() で作った色は 255 を返す
static NativeStatus u_alpha(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(255 - (int64_t)clearness(to_color(A(a, 0)->i)));
  return N_Ok;
}
static NativeStatus u_red(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n; out = mk_int((A(a, 0)->i >> 16) & 0xff); return N_Ok;
}
static NativeStatus u_green(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n; out = mk_int((A(a, 0)->i >> 8) & 0xff); return N_Ok;
}
static NativeStatus u_blue(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n; out = mk_int(A(a, 0)->i & 0xff); return N_Ok;
}
// 2つの色を混ぜる。t が 0.0 なら a、1.0 なら b
static NativeStatus u_mix(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  uint32_t c0 = to_color(A(a, 0)->i), c1 = to_color(A(a, 1)->i);
  double t = A(a, 2)->f;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  int64_t r = 0;
  for (int sh = 24; sh >= 0; sh -= 8) {   // 透け具合（上のバイト）も一緒に混ぜる
    double v = (double)((c0 >> sh) & 0xff) * (1 - t) + (double)((c1 >> sh) & 0xff) * t;
    int64_t k = (int64_t)(v + 0.5);
    if (k < 0) k = 0; if (k > 255) k = 255;
    r |= k << sh;
  }
  out = mk_int(r);
  return N_Ok;
}

// ------------------------------------------------------------------ 描く
static NativeStatus u_clear(VM& vm, Value* a, int n, Value& out) {
  if (!need_open(vm)) return N_Panic;
  // 色を渡さなければ、いまの下地の色（ui.theme）で塗る
  uint32_t c = n >= 1 ? to_color(A(a, 0)->i) : g_bg;
  // 塗りつぶすので、混ぜずに置く。ui.rgba(0,0,0,0) を渡せば「まるごと透明にする」
  for (int y = g_cy0; y <= g_cy1; y++) span_raw(g_cx0, y, g_cx1 - g_cx0 + 1, c);
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_set(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  put_raw((int)A(a, 0)->i, (int)A(a, 1)->i, to_color(A(a, 2)->i));
  out = mk_void();
  return N_Ok;
}
// 面の外を尋ねたら黒（0）を返す。読むだけなので止めない
static NativeStatus u_get(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x = (int)A(a, 0)->i, y = (int)A(a, 1)->i;
  if (x < 0 || y < 0 || x >= g_w || y >= g_h) { out = mk_int(0); return N_Ok; }
  out = mk_int((int64_t)g_px[y * g_w + x]);
  return N_Ok;
}
static NativeStatus u_hline(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int64_t w = A(a, 2)->i;
  if (w > 0) span((int)A(a, 0)->i, (int)A(a, 1)->i, (int)w, to_color(A(a, 3)->i));
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_vline(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x = (int)A(a, 0)->i, y = (int)A(a, 1)->i;
  int64_t h = A(a, 2)->i;
  uint32_t c = to_color(A(a, 3)->i);
  for (int64_t i = 0; i < h; i++) put(x, y + (int)i, c);
  out = mk_void();
  return N_Ok;
}
// 線（ブレゼンハム）。整数の足し引きだけで引く
static NativeStatus u_line(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x0 = (int)A(a, 0)->i, y0 = (int)A(a, 1)->i;
  int x1 = (int)A(a, 2)->i, y1 = (int)A(a, 3)->i;
  uint32_t c = to_color(A(a, 4)->i);
  int dx = x1 - x0 < 0 ? x0 - x1 : x1 - x0;
  int dy = y1 - y0 < 0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  for (;;) {
    put(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err * 2;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx) { err += dx; y0 += sy; }
  }
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_rect(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x = (int)A(a, 0)->i, y = (int)A(a, 1)->i;
  int w = (int)A(a, 2)->i, h = (int)A(a, 3)->i;
  uint32_t c = to_color(A(a, 4)->i);
  if (w <= 0 || h <= 0) { out = mk_void(); return N_Ok; }
  span(x, y, w, c);
  span(x, y + h - 1, w, c);
  for (int i = 1; i < h - 1; i++) { put(x, y + i, c); put(x + w - 1, y + i, c); }
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_fill_rect(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x = (int)A(a, 0)->i, y = (int)A(a, 1)->i;
  int w = (int)A(a, 2)->i, h = (int)A(a, 3)->i;
  uint32_t c = to_color(A(a, 4)->i);
  for (int i = 0; i < h; i++) span(x, y + i, w, c);
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_circle(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int cx = (int)A(a, 0)->i, cy = (int)A(a, 1)->i, r = (int)A(a, 2)->i;
  uint32_t c = to_color(A(a, 3)->i);
  if (r < 0) { out = mk_void(); return N_Ok; }
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    put(cx + x, cy + y, c); put(cx + y, cy + x, c);
    put(cx - y, cy + x, c); put(cx - x, cy + y, c);
    put(cx - x, cy - y, c); put(cx - y, cy - x, c);
    put(cx + y, cy - x, c); put(cx + x, cy - y, c);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_fill_circle(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int cx = (int)A(a, 0)->i, cy = (int)A(a, 1)->i, r = (int)A(a, 2)->i;
  uint32_t c = to_color(A(a, 3)->i);
  for (int dy = -r; dy <= r; dy++) {
    // 横一列ぶんの幅を、その段の高さから出す（掛け算だけで済ませる）
    int w = 0;
    while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
    span(cx - w, cy + dy, w * 2 + 1, c);
  }
  out = mk_void();
  return N_Ok;
}
// 切り抜き。ここから外には描かなくなる
static NativeStatus u_clip(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x = (int)A(a, 0)->i, y = (int)A(a, 1)->i;
  int w = (int)A(a, 2)->i, h = (int)A(a, 3)->i;
  g_cx0 = x < 0 ? 0 : x;
  g_cy0 = y < 0 ? 0 : y;
  g_cx1 = x + w - 1; if (g_cx1 > g_w - 1) g_cx1 = g_w - 1;
  g_cy1 = y + h - 1; if (g_cy1 > g_h - 1) g_cy1 = g_h - 1;
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_clip_off(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  g_cx0 = 0; g_cy0 = 0; g_cx1 = g_w - 1; g_cy1 = g_h - 1;
  out = mk_void();
  return N_Ok;
}
// 画素の並びをそのまま置く。横 w で折り返す（縦は個数から決まる）
static NativeStatus u_blit(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x = (int)A(a, 0)->i, y = (int)A(a, 1)->i, w = (int)A(a, 2)->i;
  ListObj* src = as_list(*A(a, 3));
  if (w <= 0) {
    vm.panic(vm.L("blit の横幅は 1 以上にします", "blit width must be 1 or more"));
    return N_Panic;
  }
  // 「そのまま置く」ものなので、混ぜない（透明もそのまま入る）
  for (int i = 0; i < src->v.size(); i++) {
    put_raw(x + i % w, y + i / w, to_color(src->v[i].i));
  }
  out = mk_void();
  return N_Ok;
}

// ------------------------------------------------------------------ 文字
// 内蔵の字形（font5x7.inc）で描く。表に無い文字は □ で場所だけ取る
static void draw_glyph(int cp, int x, int y, int scale, uint32_t c) {
  int idx = (cp >= kFontFirst && cp < kFontFirst + kFontCount) ? cp - kFontFirst : kFontCount;
  const char* art = kFont5x7[idx];
  for (int gy = 0; gy < kGlyphH; gy++) {
    for (int gx = 0; gx < kGlyphW; gx++) {
      if (art[gy * kGlyphW + gx] != '#') continue;
      if (scale == 1) { put(x + gx, y + gy, c); continue; }
      for (int sy = 0; sy < scale; sy++)
        span(x + gx * scale, y + gy * scale + sy, scale, c);
    }
  }
}

// --- 字の出どころ ---------------------------------------------------------
// 3つある。何もしなければ内蔵のままで、内蔵は ASCII しか持たないかわりに
// **どの機種でも同じに出る**（spec/library/ui.md）。
//
//   1. 内蔵の 5×7（font5x7.inc）           いつでもある
//   2. FreeType で読んだフォント            SHARK_FREETYPE を付けて作ったとき
//   3. 移植層が描いてくれる字               platform().font があるとき（ブラウザなど）
//
// 選ぶのは ui.font()。ここから下は、どれを使っているかを知らずに済むように、
// この5つの関数だけを通す。2 と 3 は同じ形（濃さの並び）で字形を返す
struct FontGlyph {
  const unsigned char* bits;   // w×h の濃さ（0 なら空）。次に取るまでの間だけ有効
  int w, h, left, top, adv;
};

static bool g_pfont = false;   // 移植層の字を使っているか
static int  g_pfont_px = 12;   // そのときの大きさ

static bool font_active() {
#if defined(SHARK_FREETYPE)
  if (ft::active()) return true;
#endif
  return g_pfont;
}
// いまの字の大きさ（行送りではなく、頼んだ大きさ）
static int font_size() {
#if defined(SHARK_FREETYPE)
  if (ft::active()) return ft::size();
#endif
  return g_pfont_px;
}
static bool font_glyph(int cp, int px, FontGlyph* out) {
#if defined(SHARK_FREETYPE)
  if (ft::active()) {
    const ft::Glyph* g = ft::glyph(cp, px);
    if (!g) return false;
    out->bits = g->bits;
    out->w = g->w; out->h = g->h;
    out->left = g->left; out->top = g->top;
    out->adv = g->adv;
    return true;
  }
#endif
  if (!g_pfont || !platform().font) return false;
  PlatformGlyph g;
  g.bits = 0; g.w = 0; g.h = 0; g.left = 0; g.top = 0; g.adv = 0;
  if (!platform().font->glyph(cp, px, &g)) return false;
  out->bits = g.bits;
  out->w = g.w; out->h = g.h;
  out->left = g.left; out->top = g.top;
  out->adv = g.adv;
  return true;
}
static int font_line_height(int px) {
#if defined(SHARK_FREETYPE)
  if (ft::active()) return ft::line_height(px);
#endif
  if (g_pfont && platform().font) return platform().font->line_height(px);
  return px;
}
static int font_ascender(int px) {
#if defined(SHARK_FREETYPE)
  if (ft::active()) return ft::ascender(px);
#endif
  if (g_pfont && platform().font) return platform().font->ascender(px);
  return px;
}
// 内蔵に戻す
static void font_close() {
#if defined(SHARK_FREETYPE)
  ft::unload();
#endif
  if (g_pfont && platform().font) platform().font->close();
  g_pfont = false;
}

// いま描いている字の大きさ（画素）。
//
// 下の層（ui.text）は「フォントの大きさの何倍か」で頼むが、上の層（部品）は
// **画素で持つ**。部品ごとに大きさを変えられるようにするためで（Widget の .font）、
// 見出しと本文を同じ画面に置ける。0 のあいだはフォントの大きさそのまま。
//
// 部品の寸法（間・余白・印の大きさ…）は、ぜんぶ ui_unit() ごしにここから決まるので、
// .font を書いた部品は、字だけでなく**まわりの寸法ごと**釣り合ったまま大きくなる。
static int g_text_px = 0;

static int base_text_px() { return font_active() ? font_size() : kCellH; }
static int cur_text_px() { return g_text_px > 0 ? g_text_px : base_text_px(); }

// 内蔵の 5×7 は整数倍にしか伸ばせないので、頼まれた大きさに近い倍率へ落とす
static int builtin_scale(int px) {
  int s = px / kCellH;
  return s < 1 ? 1 : s;
}

// 1行の高さ（字の上端から下端まで）。部品の高さもこれで決まる
static int line_h(int px) {
  if (font_active()) return font_line_height(px);
  return kCellH * builtin_scale(px);
}
// 改行したときに下げる幅。字の高さそのままだと詰まって見えるので少し空ける。
// 内蔵の 5×7 は枠（6×8）に空きを含んでいるので、そのまま
static int line_pitch(int px) {
  if (font_active()) return line_h(px) * 5 / 4;
  return line_h(px);
}
// その文字のぶん、次の字までどれだけ進むか
static int advance_of(int cp, int px) {
  if (font_active()) {
    FontGlyph g;
    return font_glyph(cp, px, &g) ? g.adv : 0;
  }
  (void)cp;
  return kCellW * builtin_scale(px);
}

// 濃さ a（0〜255）で、下地と混ぜて1点打つ。内蔵の 5×7 以外は縁がなめらかなので混ぜる
static void blend_at(int x, int y, uint32_t c, int a) {
  if (a <= 0) return;
  if (x < g_cx0 || x > g_cx1 || y < g_cy0 || y > g_cy1) return;
  uint32_t* p = &g_px[y * g_w + x];
  if (a >= 255) { *p = c; return; }
  uint32_t d = *p, r = 0;
  for (int sh = 16; sh >= 0; sh -= 8) {
    int dv = (int)((d >> sh) & 0xff), cv = (int)((c >> sh) & 0xff);
    r |= (uint32_t)((dv * (255 - a) + cv * a + 127) / 255) << sh;
  }
  *p = r;
}

// 1文字描く。(x, y) はその文字の枠の左上
static void draw_cp(int x, int y, int cp, int px, uint32_t c) {
  if (font_active()) {
    FontGlyph g;
    if (!font_glyph(cp, px, &g)) return;
    int base = y + font_ascender(px);   // 基準線。字形はここから上下に置かれる
    for (int gy = 0; gy < g.h; gy++)
      for (int gx = 0; gx < g.w; gx++)
        blend_at(x + g.left + gx, base - g.top + gy, c,
                 g.bits ? g.bits[gy * g.w + gx] : 0);
    return;
  }
  draw_glyph(cp, x, y, builtin_scale(px), c);
}

static void put_text(int x, int y, const Str& s, int px, uint32_t c) {
  if (px < 1) px = 1;
  int lh = line_pitch(px);
  int cx = x, cy = y;
  int at = 0;
  while (at < s.size()) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    at += adv;
    if (cp == '\n') { cx = x; cy += lh; continue; }
    draw_cp(cx, cy, cp, px, c);
    cx += advance_of(cp, px);
  }
}

// 何行あるか
static int text_lines(const Str& s) {
  int at = 0, n = 1;
  while (at < s.size()) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    at += adv;
    if (cp == '\n') n++;
  }
  return n;
}

// その文字を描いたときの幅（画素）。いちばん長い行で数える
static int text_px_width(const Str& s, int px) {
  if (px < 1) px = 1;
  int at = 0, cur = 0, best = 0;
  while (at < s.size()) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    at += adv;
    if (cp == '\n') { cur = 0; continue; }
    cur += advance_of(cp, px);
    if (cur > best) best = cur;
  }
  return best;
}

// 描ける幅（画素）に折り返す。改行を差し込んだ文字列を返す。
// 折り目は、その行に空白があればそこ（英語の単語を切らない）、
// 無ければ（日本語など）入り切らなくなった字の手前。
// 行頭の1字は、幅より広くてもそのまま出す（無限に折らないため）
static Str wrap_text(const Str& s, int max_w, int px) {
  if (max_w <= 0) return s;
  const char* sd = s.data();
  Str out, line;
  int line_w = 0;    // いま組んでいる行の幅
  int sp_at = -1;    // 行の中のいちばん後ろの空白（line の中のバイト位置）
  int sp_w = 0;      // その空白までの幅（空白を含む）
  int at = 0;
  while (at < s.size()) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    if (cp == '\n') {
      out += line;
      out.push('\n');
      line.clear();
      line_w = 0; sp_at = -1; sp_w = 0;
      at += adv;
      continue;
    }
    int cw = advance_of(cp, px);
    // 行末からあふれた空白は、折り目そのものにする（次の行の頭に空白を残さない）
    if (cp == ' ' && line_w > 0 && line_w + cw > max_w) {
      out += line;
      out.push('\n');
      line.clear();
      line_w = 0; sp_at = -1; sp_w = 0;
      at += adv;
      continue;
    }
    while (line_w > 0 && line_w + cw > max_w) {
      if (sp_at >= 0) {   // 空白まで戻って折る
        out += line.sub(0, sp_at);
        out.push('\n');
        Str rest = line.sub(sp_at + 1, line.size() - sp_at - 1);
        line = rest;
        line_w -= sp_w;
        sp_at = -1; sp_w = 0;
      } else {            // 空白が無ければ、この字の手前で折る
        out += line;
        out.push('\n');
        line.clear();
        line_w = 0;
      }
    }
    if (cp == ' ') { sp_at = line.size(); sp_w = line_w + cw; }
    line.append(sd + at, adv);
    line_w += cw;
    at += adv;
  }
  out += line;
  return out;
}

// 倍率で頼まれたぶんを、字の大きさ（画素）へ直してから渡す
static NativeStatus draw_text(VM& vm, Value* a, int scale, Value& out) {
  if (!need_open(vm)) return N_Panic;
  if (scale < 1) scale = 1;
  put_text((int)A(a, 0)->i, (int)A(a, 1)->i, as_str(*A(a, 2))->s, base_text_px() * scale,
           to_color(A(a, 3)->i));
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_text(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return draw_text(vm, a, 1, out);
}
static NativeStatus u_text_scaled(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return draw_text(vm, a, (int)A(a, 4)->i, out);
}
static NativeStatus u_text_width(VM& vm, Value* a, int n, Value& out) {
  (void)vm;
  int scale = n >= 2 ? (int)A(a, 1)->i : 1;
  if (scale < 1) scale = 1;
  out = mk_int(text_px_width(as_str(*A(a, 0))->s, base_text_px() * scale));
  return N_Ok;
}
static NativeStatus u_text_height(VM& vm, Value* a, int n, Value& out) {
  (void)vm;
  out = mk_int(line_h(base_text_px() * (n >= 1 && A(a, 0)->i > 0 ? (int)A(a, 0)->i : 1)));
  return N_Ok;
}

// ------------------------------------------------------------------ 入力
struct KeyName { const char* name; int code; };
static const KeyName kKeyNames[] = {
    {"left", SKEY_Left}, {"right", SKEY_Right}, {"up", SKEY_Up}, {"down", SKEY_Down},
    {"enter", SKEY_Enter}, {"esc", SKEY_Escape}, {"tab", SKEY_Tab}, {"back", SKEY_Back},
    {"delete", SKEY_Delete}, {"home", SKEY_Home}, {"end", SKEY_End},
    {"pageup", SKEY_PageUp}, {"pagedown", SKEY_PageDown},
    {"shift", SKEY_Shift}, {"ctrl", SKEY_Ctrl}, {"alt", SKEY_Alt}, {"meta", SKEY_Meta},
    {"space", 32},
    {"f1", SKEY_F1}, {"f2", SKEY_F2}, {"f3", SKEY_F3}, {"f4", SKEY_F4},
    {"f5", SKEY_F5}, {"f6", SKEY_F6}, {"f7", SKEY_F7}, {"f8", SKEY_F8},
    {"f9", SKEY_F9}, {"f10", SKEY_F10}, {"f11", SKEY_F11}, {"f12", SKEY_F12},
    {0, 0}};

// 名前をキーの番号にする。印字できる1文字は、その文字がそのまま名前になる
static int key_code(const Str& name) {
  if (name.size() == 1) {
    int c = (unsigned char)name[0];
    if (c >= 'A' && c <= 'Z') c += 32;
    return (c >= 32 && c < 127) ? c : -1;
  }
  for (int i = 0; kKeyNames[i].name; i++)
    if (name == kKeyNames[i].name) return kKeyNames[i].code;
  return -1;
}
static bool key_index(VM& vm, Value* a, int* idx) {
  const Str& name = as_str(*A(a, 0))->s;
  int c = key_code(name);
  if (c < 0) {
    vm.panic(vm.L(Str("そんな名前のキーはありません: ") + name,
                  Str("no such key: ") + name));
    return false;
  }
  *idx = c;
  return true;
}

// 文字入力（IME）。移植層が変換つきの入力を持っているときは、そちらに受けさせる
static bool g_input_want = false;   // この回、誰かが文字入力を求めたか
static bool g_input_on = false;
static Str g_input_seed;            // いま移植層に渡してある確定文字列
static Str g_marked;                // 変換中の文字列

// 入力欄の編集。移植層が選択を持たないときは、ここで数えて動かす
static int g_caret = 0, g_anchor = 0;   // 文字の数
static int g_scroll = 0;                // 左に隠している文字の数
// 1行の入力欄で、変換を始める前のカーソル。変換中は移植層の数え方に
// 変換中の字が入ってしまうので、こちらで覚えておいたところを使う（place_field）
static int g_fcaret = 0;
// 巻物の位置は**画素**で持つ。行の数で持つと、送るたびに1行まるごと飛んで
// 途中が出ない。いま（cur）と目当て（to）の2つを持ち、毎こし少しずつ寄せる
static int g_area_px = 0, g_area_to = 0;   // 複数行の入力欄で、上に隠しているぶん
static Str g_area_id;                   // その巻物が、どの入力欄のものか
static int g_vcaret = -1;               // 前の回に「見えるように」したカーソル
// 複数行の入力欄のカーソルと錨。移植層が選択を持つ機種でも、**どちら端に
// カーソルがあるか**までは取れないので、こちらで覚えておく
static int g_acaret = 0, g_aanchor = 0;
static int g_agoal = -1;                // 上下に動くときの、目当ての横の位置（画素）
// 一覧（ui.listbox）で、上に隠している行の数。最後に触った一覧のぶんだけ覚える
static Str g_list_id;
static int g_list_px = 0, g_list_to = 0;   // 上に隠しているぶん（画素）と、その目当て
static int g_list_sel = -1;      // 前の回に「見えるように」した番号
static bool g_list_drag = false;   // 右の帯をつかんでいる最中か
// 数の入力欄（ui.number）を打っている途中の字。値にできない形（空や "-"）も通るので、
// 焦点のあるあいだだけここに置く。中身そのものは、いつもどおり呼んだ側が持つ
static Str g_num_id, g_num_text;
// つまみ（ui.slider）をつかんでいる最中か。**つかんだら、外へ出ても付いてくる**。
// 押しっぱなしで動かすものは、部品の外に出たとたん止まると使いにくい
static Str g_slide_id;
static bool g_sliding = false;

// 引いて変える欄（ui.drag）。押したところと、そのときの値を覚えておく
static Str g_dnum_id;
static int g_dnum_x = 0;
static bool g_dnum_moved = false;
static double g_dnum_base = 0;
// 取り消し帳。入力欄の中身を、変わる**前**の姿で控えておく
static Str g_undo_id;                      // どの入力欄のものか
static Vec<Str> g_undo, g_redo;            // 変わる前の中身
static Vec<int> g_undo_at, g_redo_at;      // そのときのカーソル
static int64_t g_undo_last = 0;            // 最後に控えた刻限
// 続けて押したときの数え（2回で語、3回で行、4回でぜんぶ）
// 色を選ぶ板（ui.color）。出しているのはどの部品か、と、いま選んでいる色み
static Str g_color_id;
static int g_color_x = 0, g_color_y = 0;
static double g_color_h = 0, g_color_s = 0, g_color_v = 0;
static uint32_t g_color_keep = 0;   // 開いたときの色（透けぶんを持ち越す）
static int g_color_drag = 0;        // 1 = 四角をつかんでいる、2 = 色みの帯
static bool g_color_moved = false;  // この回、板の中で色が動いたか

// 巻物（ui.scroll）の、いま隠しているぶん。部品ごとに名札で覚える
static Vec<Str> g_sc_id;
static Vec<int> g_sc_px, g_sc_to;
static Str g_sc_drag;      // 右の帯をつかんでいる巻物

static Str g_multi_id;
static int g_multi_n = 0;
static int64_t g_multi_at = 0;
static int g_multi_x = 0, g_multi_y = 0;
// カーソルを合わせたときに出す説明（.tooltip）。この回、どれに乗っているか
static Str g_tip_text, g_tip_prev;
static int g_tip_x = 0, g_tip_y = 0;
static int64_t g_tip_since = 0;
static bool g_dragging = false;         // なぞって選んでいる最中
static int g_drag_anchor = 0;           // なぞり始めた文字
// 押されたところ。焦点が移るのは次の回で、変換の受け皿（IME）もそのときできる。
// 受け皿に位置を入れられるのはそれからなので、どこを押されたかを持ち越す
static int g_want_caret = -1;
static int g_want_len = 0;              // 語や行を選んだときは、その長さも
static Str g_want_id;                   // その位置を入れたい入力欄の名札
static bool g_lang_ja = true;           // 内蔵メニューの言い方

// 右で押したときのメニュー
static bool g_menu_on = false;
static int g_menu_x = 0, g_menu_y = 0;
static Vec<Str> g_menu_items;
static Vec<Str> g_menu_keys;            // 項目ごとの、キーの書き方（無ければ空）
static int g_menu_pick = -1;            // この回に選ばれた番号
static Str g_menu_owner;                // 入力欄が出したメニューなら、その名札
static int g_menu_min_w = 0;            // 最低この幅で出す（ui.combo が押した部品に揃える）
static int g_menu_px = 0, g_menu_to = 0;   // 上に隠しているぶん（画素）と、その目当て
static int64_t g_menu_step_at = 0;      // 次に1つ送る刻限。0 は「送るしるしに合わせていない」
static int g_menu_open_mx = 0, g_menu_open_my = 0;   // 出したときの、押した場所（引きずったか判じるのに使う）
static const int kMenuDragPx = 4;       // これより動いたら「引きずった」とみなす画素数





// --- 文字を受け取る -------------------------------------------------------
// 移植層が変換つきの入力（IME）を持っていればそれを使い、無ければ
// 打たれた文字をそのまま入れる。ui.field も ui.input もここを通る。
//
//   x, y, h  入力欄の位置（変換中の候補をこのあたりに出してもらう）
//   value    呼んだ側が持っている今の中身
//   conf     確定した中身（これを呼んだ側が持ち直す）
//   marked   変換中の文字列（下線を引いて出す）
static bool has_ime() {
  const PlatformScreen* s = platform().screen;
  return g_visible && s && s->text_input && s->text_state;
}

static void input_stop() {
  if (!g_input_on) return;
  if (has_ime()) platform().screen->text_input(false, 0, 0, 0, 0, false);
  g_input_on = false;
  g_input_seed.clear();
  g_marked.clear();
}

// --- 選んでいるところ -----------------------------------------------------
// 移植層が持っていればそちらが正（矢印や shift での選択も OS がやってくれる）。
// 無ければ、こちらで文字の数を覚えて動かす
static bool sel_from_platform() {
  const PlatformScreen* s = platform().screen;
  return has_ime() && s->text_selection && s->text_select;
}
static void sel_get(const Str& text, int* start, int* len) {
  if (sel_from_platform() && platform().screen->text_selection(start, len)) return;
  int n = utf8_len(text);
  int a = g_anchor > n ? n : g_anchor;
  int c = g_caret > n ? n : g_caret;
  *start = a < c ? a : c;
  *len = a < c ? c - a : a - c;
}
static void sel_set(const Str& text, int start, int len) {
  int n = utf8_len(text);
  if (start < 0) start = 0;
  if (start > n) start = n;
  if (len < 0) len = 0;
  if (start + len > n) len = n - start;
  if (sel_from_platform()) { platform().screen->text_select(start, len); return; }
  g_anchor = start;
  g_caret = start + len;
}
static Str sub_chars(const Str& s, int start, int len) {
  int b0 = utf8_offset(s, start), b1 = utf8_offset(s, start + len);
  return s.sub(b0, b1 - b0);
}
// 選んでいるところを ins で置き換えた文字列を返す
static Str sel_replace(const Str& text, const Str& ins) {
  int st = 0, ln = 0;
  sel_get(text, &st, &ln);
  if (sel_from_platform() && platform().screen->text_replace) {
    platform().screen->text_replace(ins.c_str());
    return text;   // 次の回に、受け皿から読み直す
  }
  int b0 = utf8_offset(text, st), b1 = utf8_offset(text, st + ln);
  Str out = text.sub(0, b0);
  out += ins;
  out += text.sub(b1, text.size() - b1);
  g_anchor = g_caret = st + utf8_len(ins);
  return out;
}

// --- 切り貼りの置き場 -----------------------------------------------------
static bool clip_get(Str* out) {
  const PlatformScreen* s = platform().screen;
  return s && s->clipboard_get && s->clipboard_get(out);
}
static void clip_set(const Str& s) {
  const PlatformScreen* p = platform().screen;
  if (p && p->clipboard_set) p->clipboard_set(s.c_str());
}

// 改行を落として1行にする。1行の入力欄（ui.field・ui.input）は、
// 貼り付けなどで改行が混ざってもこれで平らにする
static Str flatten(const Str& s) {
  Str r;
  for (int i = 0; i < s.size(); i++)
    if (s[i] != '\n' && s[i] != '\r') r.push(s[i]);
  return r;
}

static void input_frame(int x, int y, int h, const Str& value, Str* conf, Str* marked,
                        bool multiline = false) {
  g_input_want = true;
  marked->clear();
  if (has_ime()) {
    const PlatformScreen* sc = platform().screen;
    // 呼んだ側が中身を変えていたら、渡し直す
    bool reseed = !g_input_on || !(value == g_input_seed);
    sc->text_input(true, reseed ? value.c_str() : 0, x, y, h, multiline);
    g_input_on = true;
    Str c;
    if (sc->text_state(&c, marked)) {
      // 受け皿は改行をそのまま持つ。1行の入力欄なら、ここで落とす。
      // 落としたものは覚えている中身と食い違うので、次の回に受け皿へ入れ直される
      *conf = multiline ? c : flatten(c);
      if (!multiline) *marked = flatten(*marked);
      g_input_seed = c;
      g_marked = *marked;
      return;
    }
    *conf = value;
    g_input_seed = value;
    return;
  }
  // 変換を持たない出し先。確定した文字だけが ui.typed() に届くので、
  // 入力の位置（キャレット）はここで数えて動かす
  Str next = value;
  int n = utf8_len(next);
  if (g_caret > n) g_caret = n;
  if (g_anchor > n) g_anchor = n;
  bool shift = g_key[SKEY_Shift];
  if (g_press[SKEY_Left] && g_caret > 0) { g_caret--; if (!shift) g_anchor = g_caret; }
  if (g_press[SKEY_Right] && g_caret < n) { g_caret++; if (!shift) g_anchor = g_caret; }
  // 行の頭と終わりは、折り返しを知っている側で数える。複数行の入力欄では
  // place_area が受け持つので、ここでは触らない
  if (!multiline) {
    if (g_press[SKEY_Home]) { g_caret = 0; if (!shift) g_anchor = 0; }
    if (g_press[SKEY_End]) { g_caret = n; if (!shift) g_anchor = n; }
  }
  int st = 0, ln = 0;
  sel_get(next, &st, &ln);
  if (g_press[SKEY_Back]) {
    if (ln > 0) next = sel_replace(next, Str());
    else if (st > 0) { sel_set(next, st - 1, 1); next = sel_replace(next, Str()); }
  } else if (g_press[SKEY_Delete]) {
    if (ln > 0) next = sel_replace(next, Str());
    else if (st < n) { sel_set(next, st, 1); next = sel_replace(next, Str()); }
  }
  if (g_typed.size() > 0) next = sel_replace(next, multiline ? g_typed : flatten(g_typed));
  *conf = next;
  g_input_seed = next;
  g_marked.clear();
}

// 入力欄が受け取った打鍵を、その回のぶんとして使い切る。
// 同じ回にもう一度描くとき、同じ字がもう一度入らないようにする
static void take_input(bool area = false) {
  g_typed.clear();
  g_press[SKEY_Back] = false;
  g_press[SKEY_Delete] = false;
  g_press[SKEY_Left] = false;
  g_press[SKEY_Right] = false;
  g_press[SKEY_Home] = false;
  g_press[SKEY_End] = false;
  if (!area) return;
  // 複数行の入力欄は、行を動かすキーも使い切る（enter は改行を入れる）
  g_press[SKEY_Enter] = false;
  g_press[SKEY_Up] = false;
  g_press[SKEY_Down] = false;
  g_press[SKEY_PageUp] = false;
  g_press[SKEY_PageDown] = false;
}

static NativeStatus u_poll(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  // 前の刻みの「今このとき」を消す
  g_cursor_want = SCUR_Arrow;   // 形を頼むのは、この回に描く人
  for (int i = 0; i < SKEY_Max; i++) { g_press[i] = false; g_rel[i] = false; g_hit[i] = false; }
  for (int i = 0; i < 3; i++) { g_mpress[i] = false; g_mrel[i] = false; }
  g_typed.clear();
  g_wheel_x = g_wheel_y = 0;
  g_wheel_lines_x = g_wheel_lines_y = 0;

  const PlatformScreen* s = platform().screen;
  if (g_visible && s) {
    ScreenEvent e;
    // 一度に取る数に上限を置く。出来事が絶え間なく来ても、呼んだ側に戻れるように
    for (int guard = 0; guard < 4096 && s->poll(&e); guard++) {
      if (e.kind == SEV_Close) { g_quit = true; }
      else if (e.kind == SEV_Key) {
        if (e.code < 0 || e.code >= SKEY_Max) continue;
        if (e.down) { g_key[e.code] = true; g_press[e.code] = true; g_hit[e.code] = true; }
        else { g_key[e.code] = false; g_rel[e.code] = true; }
      } else if (e.kind == SEV_Text) {
        g_typed += e.text;
      } else if (e.kind == SEV_Mouse) {
        g_mx = e.x; g_my = e.y;
        if (e.code >= 0 && e.code < 3) {
          if (e.down) { g_mb[e.code] = true; g_mpress[e.code] = true; }
          else { g_mb[e.code] = false; g_mrel[e.code] = true; }
        }
      } else if (e.kind == SEV_Wheel) {
        g_wheel_x += e.x;   // 送りぶんは、この回のうちに足し合わせる
        g_wheel_y += e.y;
      } else if (e.kind == SEV_Resize) {
        resize_surface(e.x, e.y);   // 窓に合わせて、面を同じ大きさに作り直す
      }
    }
    // 離した合図が来ない機種では、押された刻みだけ押されているとみなす
    if (!s->has_key_up) {
      for (int i = 0; i < SKEY_Max; i++)
        if (g_key[i] && !g_hit[i]) { g_key[i] = false; g_rel[i] = true; }
    }
  }
  // ui.wheel() は**行の数**で返す。1行に満たないぶんは次の回へ持ち越すので、
  // ゆっくり動かしても、たまったところでちゃんと 1 が返る
  g_wheel_sub_y += g_wheel_y;
  g_wheel_lines_y = g_wheel_sub_y / 100;
  g_wheel_sub_y -= g_wheel_lines_y * 100;
  g_wheel_sub_x += g_wheel_x;
  g_wheel_lines_x = g_wheel_sub_x / 100;
  g_wheel_sub_x -= g_wheel_lines_x * 100;

  // 前の回に誰も文字入力を求めなかったら、受け付けを止める
  if (!g_input_want) input_stop();
  g_input_want = false;

  out = mk_bool(!g_quit);
  return N_Ok;
}
// マウスの形を頼む。**その回かぎり**で、次の ui.poll() で「ふつう」に戻る
static NativeStatus u_cursor(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  const Str& name = as_str(*A(a, 0))->s;
  static const struct { const char* name; int kind; } kNames[] = {
      {"arrow", SCUR_Arrow}, {"hand", SCUR_Hand}, {"text", SCUR_Text}, {"cross", SCUR_Cross},
      {"wait", SCUR_Wait}, {"resize_x", SCUR_ResizeX}, {"resize_y", SCUR_ResizeY},
      {"move", SCUR_Move}, {"none", SCUR_None}};
  for (int i = 0; i < (int)(sizeof kNames / sizeof kNames[0]); i++) {
    if (name == kNames[i].name) {
      g_cursor_want = kNames[i].kind;
      out = mk_void();
      return N_Ok;
    }
  }
  vm.panic(vm.L(Str("知らないマウスの形です: ") + name +
                    "（arrow hand text cross wait resize_x resize_y move none）",
                Str("unknown cursor: ") + name +
                    " (arrow hand text cross wait resize_x resize_y move none)"));
  return N_Panic;
}

static NativeStatus u_quit(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  g_quit = true;
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_key(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int i = 0;
  if (!key_index(vm, a, &i)) return N_Panic;
  out = mk_bool(g_key[i]);
  return N_Ok;
}
static NativeStatus u_pressed(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int i = 0;
  if (!key_index(vm, a, &i)) return N_Panic;
  out = mk_bool(g_press[i]);
  return N_Ok;
}
static NativeStatus u_released(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int i = 0;
  if (!key_index(vm, a, &i)) return N_Panic;
  out = mk_bool(g_rel[i]);
  return N_Ok;
}
static NativeStatus u_typed(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_str(g_typed);
  return N_Ok;
}
static NativeStatus u_mouse_x(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n; out = mk_int(g_mx); return N_Ok;
}
static NativeStatus u_mouse_y(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n; out = mk_int(g_my); return N_Ok;
}
static bool button_index(VM& vm, Value* a, int* idx) {
  int64_t b = A(a, 0)->i;
  if (b < 0 || b > 2) {
    vm.panic(vm.L("ボタンは 0（左）1（中）2（右）のどれかです",
                  "button must be 0 (left), 1 (middle) or 2 (right)"));
    return false;
  }
  *idx = (int)b;
  return true;
}
static NativeStatus u_mouse(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int i = 0;
  if (!button_index(vm, a, &i)) return N_Panic;
  out = mk_bool(g_mb[i]);
  return N_Ok;
}
static NativeStatus u_clicked(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int i = 0;
  if (!button_index(vm, a, &i)) return N_Panic;
  out = mk_bool(g_mpress[i]);
  return N_Ok;
}


// --- 文字入力（低い層）----------------------------------------------------
// 自分で入力欄を描くとき用。毎回呼んでいるあいだだけ受け付ける
// 車輪（ホイール）が、この回に送られたぶん。行の数で、下が正
static NativeStatus u_wheel(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_int(g_wheel_lines_y);
  return N_Ok;
}
// 横に送られたぶん。右が正
static NativeStatus u_wheel_x(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_int(g_wheel_lines_x);
  return N_Ok;
}
static NativeStatus u_input(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  Str conf, marked;
  input_frame((int)A(a, 0)->i, (int)A(a, 1)->i, (int)A(a, 2)->i, as_str(*A(a, 3))->s, &conf,
              &marked);
  out = mk_str(conf);
  return N_Ok;
}
static NativeStatus u_input_off(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  input_stop();
  g_input_want = false;
  out = mk_void();
  return N_Ok;
}
// 変換中の文字列。確定していないので、下線を引いて出す
static NativeStatus u_marked(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_str(g_marked);
  return N_Ok;
}
static NativeStatus u_has_ime(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_bool(has_ime());
  return N_Ok;
}


// --------------------------------------------------------- 機種のファイル選び
// OS の選び窓を出して、選ばれた道（パス）を返す。出しているあいだ、
// プログラムは止まる（OS が窓を持っているため）。
// 選び窓を持たない機種と、取りやめたときは「値なし」
static NativeStatus pick(VM& vm, bool save, const Str& title, const Str& name, Value& out) {
  (void)vm;
  const PlatformScreen* s = platform().screen;
  Str got;
  if (!s || !s->pick_file || !s->pick_file(save, title.c_str(), name.c_str(), &got)) {
    out = mk_none();
    return N_Ok;
  }
  out = mk_str(got);
  return N_Ok;
}
static NativeStatus u_pick_file(VM& vm, Value* a, int n, Value& out) {
  return pick(vm, false, n >= 1 ? as_str(*A(a, 0))->s : Str(), Str(), out);
}
static NativeStatus u_pick_save(VM& vm, Value* a, int n, Value& out) {
  if (n >= 2) return pick(vm, true, as_str(*A(a, 0))->s, as_str(*A(a, 1))->s, out);
  return pick(vm, true, Str(), n >= 1 ? as_str(*A(a, 0))->s : Str(), out);
}

// ------------------------------------------------------------------ 取り出す
// PNG にして返す。外の圧縮の道具に頼らないよう、zlib の「そのまま入れる」
// 形（stored）だけを使う。縮まないが、どの読み手でも開ける
static uint32_t crc32_of(const unsigned char* d, int n, uint32_t crc) {
  crc = ~crc;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return ~crc;
}
static void be32(Str& s, uint32_t v) {
  s.push((char)(v >> 24)); s.push((char)(v >> 16)); s.push((char)(v >> 8)); s.push((char)v);
}
static void chunk(Str& out, const char* type, const Str& data) {
  be32(out, (uint32_t)data.size());
  Str body(type, 4);
  body.append(data);
  out.append(body);
  be32(out, crc32_of((const unsigned char*)body.data(), body.size(), 0));
}
// 面を PNG にする。透けているところが1つでもあれば透明つき（色の型 6）で、
// なければ今までどおり赤緑青だけ（色の型 2）で書く。
// 読む側はどちらも開けるので、透明を使わない絵は今までと同じ大きさのまま
static Str encode_png(const uint32_t* px, int w, int h) {
  bool has_clear = false;
  for (size_t i = 0; i < (size_t)w * (size_t)h && !has_clear; i++)
    if (px[i] >> 24) has_clear = true;
  int ch = has_clear ? 4 : 3;

  Str png;
  const char sig[8] = {(char)0x89, 'P', 'N', 'G', '\r', '\n', (char)0x1a, '\n'};
  png.append(sig, 8);

  Str ihdr;
  be32(ihdr, (uint32_t)w);
  be32(ihdr, (uint32_t)h);
  ihdr.push(8);                        // 1色 8 ビット
  ihdr.push(has_clear ? 6 : 2);        // 6 = 赤緑青＋透明、2 = 赤緑青
  ihdr.push(0); ihdr.push(0); ihdr.push(0);
  chunk(png, "IHDR", ihdr);

  // 各行の頭に「前の行との差の取り方」を置く。0 はそのまま
  Str raw;
  raw.reserve((w * ch + 1) * h);
  for (int y = 0; y < h; y++) {
    raw.push(0);
    const uint32_t* row = px + (size_t)y * (size_t)w;
    for (int x = 0; x < w; x++) {
      raw.push((char)((row[x] >> 16) & 0xff));
      raw.push((char)((row[x] >> 8) & 0xff));
      raw.push((char)(row[x] & 0xff));
      // こちらは「透け具合」、PNG は「不透明のぶん」。裏返して書く
      if (ch == 4) raw.push((char)(255 - ((row[x] >> 24) & 0xff)));
    }
  }

  Str z;
  z.push(0x78); z.push(0x01);   // zlib の覚え書き（縮めていない）
  int at = 0;
  do {
    int len = raw.size() - at;
    if (len > 65535) len = 65535;
    bool last = (at + len >= raw.size());
    z.push(last ? 1 : 0);
    z.push((char)(len & 0xff)); z.push((char)((len >> 8) & 0xff));
    z.push((char)(~len & 0xff)); z.push((char)((~len >> 8) & 0xff));
    z.append(raw.data() + at, len);
    at += len;
  } while (at < raw.size());
  uint32_t s1 = 1, s2 = 0;   // adler32
  for (int i = 0; i < raw.size(); i++) {
    s1 = (s1 + (unsigned char)raw[i]) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  be32(z, (s2 << 16) | s1);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", Str());
  return png;
}

static NativeStatus u_to_png(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  out = mk_bytes(encode_png(g_px, g_w, g_h));
  return N_Ok;
}

// ------------------------------------------------------------------ 絵（Canvas）
// 画面のほかにも描ける面を持つ。使い道は3つ。
//   ・絵を読んで出す（ui.load_png）
//   ・下ごしらえした絵を何度も貼る（スプライト・地形）
//   ・3D の描き先（ui.tri と奥行きの面）
//
// **ふつうの値**にしてある（ハンドルではない）。代入すればコピーになるので、
// 絵を直す前に `var 前 = 絵;` と控えを取れる。中身は参照数を数えていて、
// 書き換えるまで写さない（copy on write）ので、控えを取るだけなら安い。
//
// 描き先を大域で切り替えると、その絵が消えたときに行き場を失う。そこで
// **絵に向けたメソッド**（絵.line(...)）にし、その1回の呼び出しの間だけ
// 面の持ちものを差し替える形にした。画面に描く ui.line(...) と名前はそろえてある
enum CanvasField { CF_W = 0, CF_H, CF_Px, CF_Cx0, CF_Cy0, CF_Cx1, CF_Cy1, CF_Count };

// 型検査に使う仮の型。本物のクラスは型検査のときに作られる（Widget と同じ仕組み）
static Type* canvas_stub(TypeTable& t) { return t.generic(Str("Canvas")); }

static bool is_canvas_stub(Type* t) {
  if (!t) return false;
  if (t->kind == T_Generic && t->name == "Canvas") return true;
  if (t->kind == T_Optional && t->a && t->a->kind == T_Generic && t->a->name == "Canvas") return true;
  return false;
}

void ui_bind_canvas_class(Registry& r, ClassInfo* real) {
  if (!real) return;
  TypeTable& t = r.types();
  Type* c = t.class_type(real);
  Type* oc = t.optional_of(c);
  for (int i = 0; i < r.size(); i++) {
    NativeEntry& e = r.at(i);
    if (is_canvas_stub(e.ret)) e.ret = (e.ret->kind == T_Optional) ? oc : c;
    for (int j = 0; j < e.params.size(); j++)
      if (is_canvas_stub(e.params[j])) e.params[j] = (e.params[j]->kind == T_Optional) ? oc : c;
  }
}

// 本物の Canvas クラス。プログラムごとに変わるので、その都度引き直す
static ClassInfo* canvas_class(VM& vm) {
  static Program* seen = 0;
  static ClassInfo* found = 0;
  if (vm.prog == seen) return found;
  seen = vm.prog;
  found = 0;
  if (!vm.prog) return 0;
  for (int i = 0; i < vm.prog->classes.size(); i++) {
    ClassInfo* c = vm.prog->classes[i];
    if (c->name == "Canvas" && c->module == "std") { found = c; break; }
  }
  return found;
}

// いま描いている面の持ちもの。絵に描く間だけ差し替える
struct Target {
  uint32_t* px; int w, h, cx0, cy0, cx1, cy1; bool open;
};
static Target target_save() {
  Target t;
  t.px = g_px; t.w = g_w; t.h = g_h;
  t.cx0 = g_cx0; t.cy0 = g_cy0; t.cx1 = g_cx1; t.cy1 = g_cy1;
  t.open = g_open;
  return t;
}
static void target_load(const Target& t) {
  g_px = t.px; g_w = t.w; g_h = t.h;
  g_cx0 = t.cx0; g_cy0 = t.cy0; g_cx1 = t.cx1; g_cy1 = t.cy1;
  g_open = t.open;
}

static bool is_canvas(VM& vm, const Value& v) {
  if (v.k != V_Obj || v.o->kind != O_Inst) return false;
  InstObj* o = (InstObj*)v.o;
  return o->cls && o->cls == canvas_class(vm) && o->fields.size() == CF_Count;
}

// 絵を作る。色を渡さなければ、まるごと透明で埋める
static bool make_canvas(VM& vm, int64_t w, int64_t h, uint32_t fill, Value& out) {
  ClassInfo* cls = canvas_class(vm);
  if (!cls) {
    vm.panic(vm.L("この処理系は ui の絵を持っていません", "this build has no ui canvas"));
    return false;
  }
  if (w <= 0 || h <= 0) {
    vm.panic(vm.L("絵の大きさは 1 以上にします", "canvas size must be 1 or more"));
    return false;
  }
  if (w > 16384 || h > 16384) {
    vm.panic(vm.L("絵が大きすぎます（縦横とも 16384 まで）",
                  "canvas too large (up to 16384 on each side)"));
    return false;
  }
  size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
  if (sk_mem_limit() != 0 && bytes > sk_mem_limit()) {
    vm.panic(vm.L("絵が大きすぎて、使ってよいメモリに入りません",
                  "the canvas does not fit in the memory limit"));
    return false;
  }
  Str px;
  px.reserve((int)bytes);
  for (size_t i = 0; i < bytes; i++) px.push('\0');
  uint32_t* p = (uint32_t*)&px[0];
  for (size_t i = 0; i < (size_t)w * (size_t)h; i++) p[i] = fill;

  out = mk_inst(cls);
  InstObj* o = as_inst(out);
  o->fields.push(mk_int(w));
  o->fields.push(mk_int(h));
  o->fields.push(mk_bytes(px));
  o->fields.push(mk_int(0));
  o->fields.push(mk_int(0));
  o->fields.push(mk_int(w - 1));
  o->fields.push(mk_int(h - 1));
  return true;
}

// 受け手の絵を描き先にする。**中身を書き換えるので、先に自分だけのものにする**
static InstObj* canvas_begin(VM& vm, Value* a, Target& saved) {
  Value* recv = val_deref(&a[0]);
  if (!is_canvas(vm, *recv)) {
    vm.panic(vm.L("絵ではありません", "not a canvas"));
    return 0;
  }
  InstObj* o = (InstObj*)obj_unique(*recv);          // 絵そのもの
  StrObj* px = (StrObj*)obj_unique(o->fields[CF_Px]); // 画素の並びも
  int w = (int)o->fields[CF_W].i, h = (int)o->fields[CF_H].i;
  if (px->s.size() < (int)((size_t)w * (size_t)h * sizeof(uint32_t))) {
    vm.panic(vm.L("絵が壊れています", "corrupt canvas"));
    return 0;
  }
  saved = target_save();
  g_px = (uint32_t*)&px->s[0];
  g_w = w; g_h = h;
  g_cx0 = (int)o->fields[CF_Cx0].i; g_cy0 = (int)o->fields[CF_Cy0].i;
  g_cx1 = (int)o->fields[CF_Cx1].i; g_cy1 = (int)o->fields[CF_Cy1].i;
  g_open = true;   // 絵の上では「面がある」。画面を開いていなくても描ける
  return o;
}

// 読むだけのとき。**写さない**（obj_unique を通さない）。
// 控えを持っている絵（参照が2つ以上）でも、読むだけなら写さずに済ませる
static bool canvas_begin_read(VM& vm, Value* a, Target& saved) {
  Value* recv = val_deref(&a[0]);
  if (!is_canvas(vm, *recv)) {
    vm.panic(vm.L("絵ではありません", "not a canvas"));
    return false;
  }
  InstObj* o = as_inst(*recv);
  StrObj* px = as_str(o->fields[CF_Px]);
  int w = (int)o->fields[CF_W].i, h = (int)o->fields[CF_H].i;
  if (px->s.size() < (int)((size_t)w * (size_t)h * sizeof(uint32_t))) {
    vm.panic(vm.L("絵が壊れています", "corrupt canvas"));
    return false;
  }
  saved = target_save();
  g_px = (uint32_t*)px->s.data();   // 読むだけなので、書き込み可能にしなくてよい
  g_w = w; g_h = h;
  g_cx0 = 0; g_cy0 = 0; g_cx1 = w - 1; g_cy1 = h - 1;
  g_open = true;
  return true;
}

// 描き終わり。切り抜きは絵の側に覚えておく（絵ごとに持つ）
static void canvas_end(InstObj* o, const Target& saved) {
  if (o) {
    o->fields[CF_Cx0] = mk_int(g_cx0); o->fields[CF_Cy0] = mk_int(g_cy0);
    o->fields[CF_Cx1] = mk_int(g_cx1); o->fields[CF_Cy1] = mk_int(g_cy1);
  }
  target_load(saved);
}

// 画面に描く u_* を、そのまま絵に向けて呼ぶ。受け手のぶん（a[0]）をずらすだけで、
// 描くしくみは1つで済む
typedef NativeStatus (*DrawFn)(VM&, Value*, int, Value&);
static NativeStatus canvas_forward(DrawFn fn, VM& vm, Value* a, int n, Value& out) {
  Target saved;
  InstObj* o = canvas_begin(vm, a, saved);
  if (!o) return N_Panic;
  NativeStatus st = fn(vm, a + 1, n - 1, out);
  canvas_end(o, saved);
  return st;
}

// 前もって宣言（下の register_ui より前に要る）
static NativeStatus u_clear(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_set(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_get(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_hline(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_vline(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_line(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_rect(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_fill_rect(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_circle(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_fill_circle(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_blit(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_clip(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_clip_off(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_text(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_text_scaled(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_draw(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_tri(VM& vm, Value* a, int n, Value& out);
static NativeStatus u_clear_depth(VM& vm, Value* a, int n, Value& out);

NativeStatus n_canvas_clear(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_clear, vm, a, n, out); }
NativeStatus n_canvas_set(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_set, vm, a, n, out); }
NativeStatus n_canvas_hline(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_hline, vm, a, n, out); }
NativeStatus n_canvas_vline(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_vline, vm, a, n, out); }
NativeStatus n_canvas_line(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_line, vm, a, n, out); }
NativeStatus n_canvas_rect(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_rect, vm, a, n, out); }
NativeStatus n_canvas_fill_rect(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_fill_rect, vm, a, n, out); }
NativeStatus n_canvas_circle(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_circle, vm, a, n, out); }
NativeStatus n_canvas_fill_circle(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_fill_circle, vm, a, n, out); }
NativeStatus n_canvas_blit(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_blit, vm, a, n, out); }
NativeStatus n_canvas_clip(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_clip, vm, a, n, out); }
NativeStatus n_canvas_clip_off(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_clip_off, vm, a, n, out); }
NativeStatus n_canvas_draw(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_draw, vm, a, n, out); }
NativeStatus n_canvas_tri(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_tri, vm, a, n, out); }
NativeStatus n_canvas_clear_depth(VM& vm, Value* a, int n, Value& out) { return canvas_forward(u_clear_depth, vm, a, n, out); }
NativeStatus n_canvas_text(VM& vm, Value* a, int n, Value& out) {
  return canvas_forward(n >= 6 ? u_text_scaled : u_text, vm, a, n, out);
}
// 読むだけ。写さない道を通す（控えを持っていても、読むのは安いまま）
NativeStatus n_canvas_get(VM& vm, Value* a, int n, Value& out) {
  Target saved;
  if (!canvas_begin_read(vm, a, saved)) return N_Panic;
  NativeStatus st = u_get(vm, a + 1, n - 1, out);
  target_load(saved);
  return st;
}

NativeStatus n_canvas_width(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value* recv = val_deref(&a[0]);
  if (!is_canvas(vm, *recv)) { vm.panic(vm.L("絵ではありません", "not a canvas")); return N_Panic; }
  out = mk_int(as_inst(*recv)->fields[CF_W].i);
  return N_Ok;
}
NativeStatus n_canvas_height(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value* recv = val_deref(&a[0]);
  if (!is_canvas(vm, *recv)) { vm.panic(vm.L("絵ではありません", "not a canvas")); return N_Panic; }
  out = mk_int(as_inst(*recv)->fields[CF_H].i);
  return N_Ok;
}
NativeStatus n_canvas_to_png(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value* recv = val_deref(&a[0]);
  if (!is_canvas(vm, *recv)) { vm.panic(vm.L("絵ではありません", "not a canvas")); return N_Panic; }
  InstObj* o = as_inst(*recv);
  StrObj* px = as_str(o->fields[CF_Px]);
  out = mk_bytes(encode_png((const uint32_t*)px->s.data(), (int)o->fields[CF_W].i,
                            (int)o->fields[CF_H].i));
  return N_Ok;
}

// --- 作る・読む -----------------------------------------------------------
static NativeStatus u_canvas(VM& vm, Value* a, int n, Value& out) {
  // 色を渡さなければ、まるごと透明（0xff000000）で埋める
  uint32_t fill = n >= 3 ? to_color(A(a, 2)->i) : 0xff000000u;
  if (!make_canvas(vm, A(a, 0)->i, A(a, 1)->i, fill, out)) return N_Panic;
  return N_Ok;
}

// PNG を読む。読めなければ none（Canvas?）
static NativeStatus u_load_png(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  StrObj* src = as_str(*A(a, 0));
  int w = 0, h = 0;
  Str px;
  if (!png::load((const unsigned char*)src->s.data(), src->s.size(), &w, &h, px)) {
    out = mk_none();
    return N_Ok;
  }
  ClassInfo* cls = canvas_class(vm);
  if (!cls) {
    vm.panic(vm.L("この処理系は ui の絵を持っていません", "this build has no ui canvas"));
    return N_Panic;
  }
  out = mk_inst(cls);
  InstObj* o = as_inst(out);
  o->fields.push(mk_int(w));
  o->fields.push(mk_int(h));
  o->fields.push(mk_bytes(px));
  o->fields.push(mk_int(0));
  o->fields.push(mk_int(0));
  o->fields.push(mk_int(w - 1));
  o->fields.push(mk_int(h - 1));
  return N_Ok;
}

// --- 貼る -----------------------------------------------------------------
// 画素の並びを今の描き先に貼る。透けているところは下地が残る。
// 大きさを渡すと、その大きさに伸ばす（いちばん近い画素を取る。輪郭がぼけない）。
// 部品の側（ui.image）からも呼ぶので、絵そのものではなく画素と大きさで受ける
static void blit_scaled(const uint32_t* sp, int sw, int sh, int dx, int dy, int dw, int dh) {
  if (!sp || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
  for (int y = 0; y < dh; y++) {
    int ty = dy + y;
    if (ty < g_cy0 || ty > g_cy1) continue;
    int sy = (int)((int64_t)y * sh / dh);
    const uint32_t* row = sp + (size_t)sy * (size_t)sw;
    for (int x = 0; x < dw; x++) {
      int tx = dx + x;
      if (tx < g_cx0 || tx > g_cx1) continue;
      uint32_t c = row[(int)((int64_t)x * sw / dw)];
      if ((c >> 24) == 255) continue;   // まるごと透明なら、下地のまま
      uint32_t* p = &g_px[(size_t)ty * (size_t)g_w + tx];
      *p = (c >> 24) ? over(*p, c) : c;
    }
  }
}

static NativeStatus u_draw(VM& vm, Value* a, int n, Value& out) {
  if (!need_open(vm)) return N_Panic;
  Value* src = A(a, 0);
  if (!is_canvas(vm, *src)) {
    vm.panic(vm.L("絵ではありません", "not a canvas"));
    return N_Panic;
  }
  InstObj* o = as_inst(*src);
  int sw = (int)o->fields[CF_W].i, sh = (int)o->fields[CF_H].i;
  const uint32_t* sp = (const uint32_t*)as_str(o->fields[CF_Px])->s.data();
  int dx = (int)A(a, 1)->i, dy = (int)A(a, 2)->i;
  int dw = n >= 5 ? (int)A(a, 3)->i : sw;
  int dh = n >= 5 ? (int)A(a, 4)->i : sh;
  // 同じ絵に貼ろうとしても、読みながら書くことになるだけなので止めない
  blit_scaled(sp, sw, sh, dx, dy, dw, dh);
  out = mk_void();
  return N_Ok;
}

// --- 三角形と奥行き -------------------------------------------------------
// 3D は「奥行きのくらべっこ」だけが組み合わせで書けない。そこをここに置き、
// 見え方の計算（回す・遠近をつける）は Shark の側で書く。
//
// 奥行きの面は**1枚だけ**を使い回す。描き先の大きさに合わせて取り直す。
// 1度に描くのは1つの面なので、これで足りる（spec/library/ui.md）
static int32_t* g_depth = 0;
static int g_depth_w = 0, g_depth_h = 0;
static bool g_depth_on = false;
static const int32_t kFar = 0x7fffffff;   // いちばん遠い

static void depth_free() {
  if (g_depth) { sk_free(g_depth); g_depth = 0; }
  g_depth_w = g_depth_h = 0;
}
static void depth_clear() {
  if (!g_depth) return;
  for (size_t i = 0; i < (size_t)g_depth_w * (size_t)g_depth_h; i++) g_depth[i] = kFar;
}
// 今の描き先に合う大きさにする。大きさが変われば取り直して、まっさらにする
static bool depth_ensure() {
  if (g_depth && g_depth_w == g_w && g_depth_h == g_h) return true;
  depth_free();
  size_t bytes = (size_t)g_w * (size_t)g_h * sizeof(int32_t);
  if (sk_mem_limit() != 0 && bytes > sk_mem_limit()) return false;
  g_depth = (int32_t*)sk_alloc(bytes);
  if (!g_depth) return false;
  g_depth_w = g_w; g_depth_h = g_h;
  depth_clear();
  return true;
}

static NativeStatus u_depth(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  bool on = A(a, 0)->b;
  if (!on) { g_depth_on = false; depth_free(); out = mk_void(); return N_Ok; }
  if (!depth_ensure()) {
    vm.panic(vm.L("奥行きの面が、使ってよいメモリに入りません",
                  "the depth buffer does not fit in the memory limit"));
    return N_Panic;
  }
  g_depth_on = true;
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_clear_depth(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  if (g_depth_on && depth_ensure()) depth_clear();
  out = mk_void();
  return N_Ok;
}

static inline int imin3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int imax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

// 三角形を塗る。頂点は画面の座標（x, y）と奥行き（z）。**z は小さいほど手前**。
// どちら回りでも描く（表裏の選り分けは書く人が Shark 側でする）
static NativeStatus u_tri(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  int x0 = (int)A(a, 0)->i, y0 = (int)A(a, 1)->i, z0 = (int)A(a, 2)->i;
  int x1 = (int)A(a, 3)->i, y1 = (int)A(a, 4)->i, z1 = (int)A(a, 5)->i;
  int x2 = (int)A(a, 6)->i, y2 = (int)A(a, 7)->i, z2 = (int)A(a, 8)->i;
  uint32_t c = to_color(A(a, 9)->i);
  if ((c >> 24) == 255) { out = mk_void(); return N_Ok; }   // まるごと透明

  int64_t area = (int64_t)(x1 - x0) * (y2 - y0) - (int64_t)(x2 - x0) * (y1 - y0);
  if (area == 0) { out = mk_void(); return N_Ok; }          // つぶれている
  bool use_depth = g_depth_on && depth_ensure();

  int minx = imin3(x0, x1, x2), maxx = imax3(x0, x1, x2);
  int miny = imin3(y0, y1, y2), maxy = imax3(y0, y1, y2);
  if (minx < g_cx0) minx = g_cx0;
  if (miny < g_cy0) miny = g_cy0;
  if (maxx > g_cx1) maxx = g_cx1;
  if (maxy > g_cy1) maxy = g_cy1;

  for (int y = miny; y <= maxy; y++) {
    for (int x = minx; x <= maxx; x++) {
      // 辺の式。3つとも面積と同じ向きなら、その点は三角形の中
      int64_t e0 = (int64_t)(x2 - x1) * (y - y1) - (int64_t)(y2 - y1) * (x - x1);
      int64_t e1 = (int64_t)(x0 - x2) * (y - y2) - (int64_t)(y0 - y2) * (x - x2);
      int64_t e2 = (int64_t)(x1 - x0) * (y - y0) - (int64_t)(y1 - y0) * (x - x0);
      if (area > 0) { if (e0 < 0 || e1 < 0 || e2 < 0) continue; }
      else { if (e0 > 0 || e1 > 0 || e2 > 0) continue; }
      if (use_depth) {
        // 3つの頂点の奥行きを、面積の割合で混ぜる
        double z = ((double)e0 * z0 + (double)e1 * z1 + (double)e2 * z2) / (double)area;
        int32_t zi = z <= -2147483647.0 ? -2147483647 : (z >= 2147483646.0 ? 2147483646 : (int32_t)z);
        int32_t* d = &g_depth[(size_t)y * (size_t)g_depth_w + x];
        if (zi >= *d) continue;   // 手前に何かある
        *d = zi;
      }
      uint32_t* p = &g_px[(size_t)y * (size_t)g_w + x];
      *p = (c >> 24) ? over(*p, c) : c;
    }
  }
  out = mk_void();
  return N_Ok;
}

// ------------------------------------------------------------------ フォント
// 内蔵の字形は ASCII しか持たない。日本語などを出したいときは、
// 機種のフォントを読む（FreeType が要る → README「日本語の字を出す」）。
//
// 読むかどうかは**書く人が決める**。何もしなければ内蔵のままで、
// そのぶん、どの機種でも同じ大きさ・同じ形で出る（spec/library/ui.md）。

#if defined(SHARK_FREETYPE)

// 機種にありそうなフォント。上から順に見て、最初に開けたものを使う。
// どれも「その機種に**はじめから入っている**日本語の出るゴシック」を選んである
// （足したいときは、太さがふつう（Regular / W4）のものを上に置く）
static const char* const kFontPaths[] = {
#if defined(__APPLE__)
    // W4 が本文の太さ（Regular くらい）。W3 は細く見えるので後ろに置く
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
#elif defined(_WIN32)
    "C:\\Windows\\Fonts\\YuGothR.ttc",     // 游ゴシック（Windows 10 以降）
    "C:\\Windows\\Fonts\\YuGothM.ttc",
    "C:\\Windows\\Fonts\\meiryo.ttc",      // メイリオ（Vista 以降）
    "C:\\Windows\\Fonts\\msgothic.ttc",    // MS ゴシック（ずっとある）
    "C:\\Windows\\Fonts\\segoeui.ttf",     // 日本語が要らないとき
    "C:\\Windows\\Fonts\\arial.ttf",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",       // Debian / Ubuntu
    "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",     // Fedora
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",            // Arch
    "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",          // Debian の別名
    "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
    "/usr/share/fonts/ipa-gothic/ipag.ttf",
    "/usr/share/fonts/truetype/ipafont-gothic/ipag.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",              // 日本語は無い
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
    0};

// 控えのフォント。**本命に無い字**が来たときだけ、上から順に読む。
// 絵文字・ハングル・記号など、1つのフォントには入りきらないものがここに来る。
//
// 並びは「軽くて広く持っているもの」から。読んだぶんはそのまま抱えるので、
// 重いもの（CJK やハングルは 10 MB 近い）を後ろに置くと、
// よくある字はわずかな出費で済む（→ core/lib/font_ft.inc の「控え」）。
// 数を絞ってあるのも同じ理由で、似たものを何本も抱えても得にならない
static const char* const kFallbackPaths[] = {
#if defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",   // 記号を広く持っている
    "/System/Library/Fonts/Apple Color Emoji.ttc",            // 絵文字
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",             // ハングル
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",         // 日本語（本命が英字のとき）
#elif defined(_WIN32)
    "C:\\Windows\\Fonts\\segoeui.ttf",      // 英数字・記号・キリル文字など（1 MB ほど）
    "C:\\Windows\\Fonts\\seguiemj.ttf",     // 絵文字（Windows 8.1 以降）
    "C:\\Windows\\Fonts\\malgun.ttf",       // ハングル
    "C:\\Windows\\Fonts\\YuGothR.ttc",      // 日本語（本命が英字のとき）
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",      // 絵文字
    "/usr/share/fonts/noto/NotoColorEmoji.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", // 日本語（本命が英字のとき）
#endif
    0};

// フォントの中身を読む。コアはファイルを開かないので、移植層に頼む
// （spec/runtime/embedding.md）。日本語のフォントは数 MB あるので、
// プログラムのメモリ（--memory）には数えない malloc に置く
static bool read_font_file(const char* path, char** out, size_t* out_n) {
  const PlatformFile* f = platform().file;
  if (!f) return false;
  Str err;
  void* h = f->open(path, "r", &err);
  if (!h) return false;
  size_t cap = 1u << 20;
  int64_t known = 0;
  if (f->size && f->size(path, &known) && known > 0) cap = (size_t)known;
  char* buf = (char*)malloc(cap);
  if (!buf) { f->close(h); return false; }
  size_t len = 0;
  for (;;) {
    if (len == cap) {
      size_t nc = cap * 2;
      char* nb = (char*)realloc(buf, nc);
      if (!nb) { free(buf); f->close(h); return false; }
      buf = nb;
      cap = nc;
    }
    int got = f->read(h, buf + len, (int)(cap - len > (size_t)1 << 20 ? (size_t)1 << 20 : cap - len));
    if (got <= 0) break;
    len += (size_t)got;
  }
  f->close(h);
  if (len == 0) { free(buf); return false; }
  *out = buf;
  *out_n = len;
  return true;
}

// 控えの候補を、番号で1つずつ返す。
//   0 番から  環境変数 SHARK_FONT_FALLBACK（; 区切り。使う人が決めたものが勝つ）
//   そのあと  kFallbackPaths（機種にありそうなもの）
// 一覧を先に作らないのは、控えが要るとは限らないから（たいていは要らない）
static bool fallback_path_at(int index, Str* out) {
  const PlatformOS* os = platform().os;
  Str want;
  if (os && os->env && os->env("SHARK_FONT_FALLBACK", &want) && want.size() > 0) {
    int seen = 0;
    Str cur;
    for (int i = 0; i <= want.size(); i++) {
      if (i == want.size() || want[i] == ';') {
        if (cur.size()) {
          if (seen == index) { *out = cur; return true; }
          seen++;
        }
        cur.clear();
        continue;
      }
      cur.push(want[i]);
    }
    index -= seen;
  }
  for (int i = 0; kFallbackPaths[i]; i++) {
    if (index == 0) { *out = Str(kFallbackPaths[i]); return true; }
    index--;
  }
  return false;
}

// font_ft.inc から呼ばれる。コアはファイルを開かないので、読むのはこちら。
// 候補はあるが読めなかったときは data に 0 を入れて true（次の候補へ）
static bool fallback_font_src(int index, char** data, size_t* n, Str* name) {
  Str path;
  if (!fallback_path_at(index, &path)) return false;
  *name = path;
  if (!read_font_file(path.c_str(), data, n)) { *data = 0; *n = 0; }
  return true;
}

static bool load_font_path(const char* path, int px) {
  char* data = 0;
  size_t n = 0;
  if (!read_font_file(path, &data, &n)) return false;
  bool ok = ft::load(data, n, px, Str(path));
  free(data);
  if (ok) ft::set_fallback_source(fallback_font_src);
  return ok;
}

// 機種のフォントを探す。SHARK_FONT があればそれを先に見る
static bool load_font_auto(int px) {
  const PlatformOS* os = platform().os;
  if (os && os->env) {
    Str want;
    if (os->env("SHARK_FONT", &want) && want.size() > 0)
      if (load_font_path(want.c_str(), px)) return true;
  }
  for (int i = 0; kFontPaths[i]; i++)
    if (load_font_path(kFontPaths[i], px)) return true;
  return false;
}

#else   // FreeType 無しで作ったとき。字は内蔵の 5×7 だけになる

static bool load_font_path(const char* path, int px) { (void)path; (void)px; return false; }
static bool load_font_auto(int px) { (void)px; return false; }

#endif

// 移植層が字を描いてくれるなら、それを使う（ブラウザなど）。
// name が 0 なら、その機種でふつうに使えるものに任せる
static bool load_font_platform(const char* name, int px) {
  const PlatformFont* f = platform().font;
  if (!f) return false;
  if (px < 4) px = 4;
  if (px > 200) px = 200;
  if (!f->open(name, px)) return false;
  g_pfont = true;
  g_pfont_px = px;
  return true;
}

// ui.font(大きさ) — 機種のフォントを使う
static NativeStatus u_font_size(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int px = (int)A(a, 0)->i;
#if defined(SHARK_FREETYPE)
  if (ft::active() && ft::resize(px)) { out = mk_bool(true); return N_Ok; }
#endif
  if (load_font_auto(px)) { out = mk_bool(true); return N_Ok; }
  out = mk_bool(load_font_platform(0, px));
  return N_Ok;
}
// ui.font(場所, 大きさ)。移植層が字を持っている機種（ブラウザ）では、
// ファイルの場所ではなく**フォントの名前**として渡す
static NativeStatus u_font_path(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& s = as_str(*A(a, 0))->s;
  int px = (int)A(a, 1)->i;
  if (load_font_path(s.c_str(), px)) { out = mk_bool(true); return N_Ok; }
  out = mk_bool(load_font_platform(s.c_str(), px));
  return N_Ok;
}
// ui.font(中身, 大きさ) — 自分で読んだものを渡す
static NativeStatus u_font_bytes(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
#if defined(SHARK_FREETYPE)
  const Str& d = as_str(*A(a, 0))->s;
  bool ok = ft::load(d.data(), (size_t)d.size(), (int)A(a, 1)->i, Str("(bytes)"));
  if (ok) ft::set_fallback_source(fallback_font_src);   // 控えは、こちらでも効く
  out = mk_bool(ok);
#else
  (void)a;
  out = mk_bool(false);
#endif
  return N_Ok;
}
static NativeStatus u_font_builtin(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  font_close();
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_font_name(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
#if defined(SHARK_FREETYPE)
  if (ft::active()) { out = mk_str(ft::name()); return N_Ok; }
#endif
  if (g_pfont && platform().font) { out = mk_str(Str(platform().font->name())); return N_Ok; }
  out = mk_str(Str());
  return N_Ok;
}

// ------------------------------------------------------------ 宣言的な層
// 「今どうあるべきか」を **Widget 1つにして返す**（spec/library/ui.md）。
// 呼び出しにブロックを続ける記法は言語に無いので、入れ子は ui.col / ui.row / ui.grid に
// 配列で渡して表す。
//
//   func view(count: int) -> Widget {
//     return ui.col([ui.label(f"{count} 回"), ui.button("押す", "inc")]);
//   }
//   var hit = ui.show(view(count));     // 描いて、押されたものの名札が返る
//
// 部品は毎回作り直され、状態は持たない。押されたかどうかだけを名札で返し、
// 値は呼んだ側が持つ。だから「今の状態」と「画面」がずれない。
enum WidgetKind {
  WK_Label = 0, WK_Button, WK_Checkbox, WK_Slider, WK_Field, WK_Space, WK_Column, WK_Row,
  WK_Divider, WK_Grid, WK_Area, WK_Radio, WK_Combo, WK_List, WK_Tabs, WK_Number, WK_Spacer,
  WK_Stack, WK_Scroll, WK_Image, WK_Color, WK_Drag, WK_Tree
};
enum WidgetField {
  WF_Kind = 0, WF_Text, WF_Id, WF_A, WF_B, WF_C, WF_Kids,
  WF_Fg, WF_Bg, WF_Pad, WF_Wid, WF_Hei, WF_WidFr, WF_HeiFr, WF_Align, WF_Act, WF_Tip,
  WF_Hint, WF_Var,
  WF_Border, WF_BorderW, WF_Radius, WF_Flags, WF_Fa, WF_Fb, WF_Fc, WF_Opt, WF_Px, WF_VAlign,
  WF_Dec, WF_Sel, WF_Font, WF_Count
};
enum WidgetAlign { WA_Left = 0, WA_Center, WA_Right };
// 縦の寄せ方。-1（指定なし）のときは、置く側の決めた既定になる
enum WidgetVAlign { WV_Top = 0, WV_Middle, WV_Bottom };
// こまごました入切（WF_Flags）
enum WidgetFlag {
  WFL_Float = 1,    // 値が小数（ui.slider / ui.drag の float の形）
  WFL_Show = 2,     // 隠した字を見せる（ui.password）
  WFL_Multi = 4,    // いくつも選べる（ui.listbox）
  WFL_Open = 8,     // 開いている（ui.tree）
  WFL_Disabled = 16 // 使えない（.disabled）。中の部品にも受け継がれる
};

// 型検査に使う仮の型。本物の Widget クラスは型検査のときに作られる
// （core/check.cpp）ので、登録の時点では**名前だけ同じ**の仮の型を置いておき、
// 型検査がクラスを作ったところで ui_bind_widget_class が差し替える。
//
// 名前が本物と同じなので、関数の表の指紋（registry.h）は差し替えの前後で変わらない。
// バイトコードだけを動かす実行装置には型検査が無く、差し替えも起きないが、
// 指紋は同じものになる
static Type* widget_stub(TypeTable& t) { return t.generic(Str("Widget")); }

static bool is_stub_type(Type* t) {
  if (!t) return false;
  if (t->kind == T_Generic && t->name == "Widget") return true;
  if (t->kind == T_List && t->a && t->a->kind == T_Generic && t->a->name == "Widget") return true;
  return false;
}

void ui_bind_widget_class(Registry& r, ClassInfo* real) {
  if (!real) return;
  TypeTable& t = r.types();
  Type* w = t.class_type(real);
  Type* lw = t.list_of(w);
  for (int i = 0; i < r.size(); i++) {
    NativeEntry& e = r.at(i);
    if (is_stub_type(e.ret)) e.ret = (e.ret->kind == T_List) ? lw : w;
    for (int j = 0; j < e.params.size(); j++)
      if (is_stub_type(e.params[j])) e.params[j] = (e.params[j]->kind == T_List) ? lw : w;
  }
}

// 本物の Widget クラス。プログラムごとに変わるので、その都度引き直す
static ClassInfo* widget_class(VM& vm) {
  static Program* seen = 0;
  static ClassInfo* found = 0;
  if (vm.prog == seen) return found;
  seen = vm.prog;
  found = 0;
  if (!vm.prog) return 0;
  for (int i = 0; i < vm.prog->classes.size(); i++) {
    ClassInfo* c = vm.prog->classes[i];
    if (c->name == "Widget" && c->module == "std") { found = c; break; }
  }
  return found;
}

static bool make_widget(VM& vm, int kind, const Str& text, const Str& id, int64_t a, int64_t b,
                        int64_t c, Value* kids, Value& out, Value* action = 0) {
  ClassInfo* cls = widget_class(vm);
  if (!cls) {
    vm.panic(vm.L("この処理系は ui の部品を持っていません", "this build has no ui widgets"));
    return false;
  }
  out = mk_inst(cls);
  InstObj* o = as_inst(out);
  o->fields.push(mk_int(kind));
  o->fields.push(mk_str(text));
  o->fields.push(mk_str(id));
  o->fields.push(mk_int(a));
  o->fields.push(mk_int(b));
  o->fields.push(mk_int(c));
  o->fields.push(kids ? val_retain(*kids) : mk_list());
  o->fields.push(mk_int(-1));   // fg（-1 は指定なし）
  o->fields.push(mk_int(-1));   // bg
  o->fields.push(mk_int(0));    // pad
  o->fields.push(mk_int(0));    // wid（画素。0 は指定なし）
  o->fields.push(mk_int(0));    // hei
  o->fields.push(mk_float(0));  // wid の取り分（fr）。0 は指定なし
  o->fields.push(mk_float(0));  // hei の取り分
  o->fields.push(mk_int(WA_Left));
  // 押されたときに呼ぶ関数。持たないときは「値なし」を入れておく
  o->fields.push(action ? val_retain(*action) : mk_void());
  o->fields.push(mk_str(Str()));   // カーソルを合わせたときに出す説明（.tooltip）
  o->fields.push(mk_str(Str()));   // 何も入っていないときに、うすく出す字（.placeholder）
  o->fields.push(mk_int(-1));      // ref で受けたときの、書き戻す var の番号（-1 は無い）
  o->fields.push(mk_int(-1));      // 縁の色（.border）。-1 は指定なし
  o->fields.push(mk_int(0));       // 縁の太さ（画素）
  o->fields.push(mk_int(-1));      // 角の丸み（.radius）。-1 は指定なし
  o->fields.push(mk_int(0));       // こまごました入切（WidgetFlag）
  o->fields.push(mk_float(0));     // 小数の値
  o->fields.push(mk_float(0));     // 小数の下
  o->fields.push(mk_float(0));     // 小数の上
  o->fields.push(mk_str(Str()));   // こまかい指定（入力に通す字など）
  o->fields.push(mk_bytes(Str())); // 画像の画素（ui.image）
  o->fields.push(mk_int(-1));      // 縦の寄せ方（.valign）。-1 は指定なし
  o->fields.push(mk_int(-1));      // 出す小数の桁（.decimals）。-1 は限りの広さから決める
  o->fields.push(mk_list());       // いくつも選べる一覧で、選ばれている番号
  o->fields.push(mk_int(0));       // 字の大きさ（.font。画素）。0 は指定なし
  return true;
}


// 見た目を1つ変えたものを返す。**元は変えない**（すべての代入はコピー、と同じ考え）。
// まず自分だけの実体を作り、field に value を入れる
static InstObj* widget_copy(VM& vm, Value* a, Value& out) {
  Value* recv = val_deref(&a[0]);
  if (recv->k != V_Obj || recv->o->kind != O_Inst) {
    vm.panic(vm.L("部品ではありません", "not a widget"));
    return 0;
  }
  Value copy = val_retain(*recv);
  obj_unique(copy);            // 参照が2つ以上あれば、ここで自分だけの実体になる
  out = copy;
  return as_inst(copy);
}

static void widget_put(InstObj* o, int field, const Value& v) {
  if (field >= o->fields.size()) return;
  val_release(o->fields[field]);
  o->fields[field] = v;
}

// ref で受けた var の番号を覚えさせる。動いたら、そこへ直に書き戻す（hit）
static void set_var_slot(Value& w, int slot) {
  if (w.k != V_Obj || w.o->kind != O_Inst) return;
  widget_put(as_inst(w), WF_Var, mk_int(slot));
}

static NativeStatus widget_with(VM& vm, Value* a, int field, int64_t value, Value& out) {
  InstObj* o = widget_copy(vm, a, out);
  if (!o) return N_Panic;
  widget_put(o, field, mk_int(value));
  return N_Ok;
}
static NativeStatus widget_with_str(VM& vm, Value* a, int field, const Str& value, Value& out) {
  InstObj* o = widget_copy(vm, a, out);
  if (!o) return N_Panic;
  widget_put(o, field, mk_str(value));
  return N_Ok;
}

// 幅と高さは「画素（int）」と「取り分（fr。float）」のどちらか一方で決まる。
// あとから書いた方が残るので、書かなかった側は消しておく
static NativeStatus widget_size(VM& vm, Value* a, int px_field, int fr_field, int64_t px,
                                double fr, Value& out) {
  InstObj* o = widget_copy(vm, a, out);
  if (!o) return N_Panic;
  widget_put(o, px_field, mk_int(px));
  widget_put(o, fr_field, mk_float(fr));
  return N_Ok;
}

// 字の大きさ（画素）。0 以下は「指定なし」。大きすぎるものは頭打ちにする
NativeStatus n_widget_font(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t v = val_deref(&a[1])->i;
  if (v < 0) v = 0;
  if (v > 400) v = 400;
  return widget_with(vm, a, WF_Font, v, out);
}
// 使えなくする。false を渡せば、元どおり使える
NativeStatus n_widget_disabled(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  InstObj* o = widget_copy(vm, a, out);
  if (!o) return N_Panic;
  if (WF_Flags >= o->fields.size()) return N_Ok;
  int f = (int)o->fields[WF_Flags].i;
  if (val_deref(&a[1])->b) f |= WFL_Disabled;
  else f &= ~WFL_Disabled;
  widget_put(o, WF_Flags, mk_int(f));
  return N_Ok;
}

NativeStatus n_widget_color(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return widget_with(vm, a, WF_Fg, (int64_t)to_color(val_deref(&a[1])->i), out);
}
NativeStatus n_widget_background(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return widget_with(vm, a, WF_Bg, (int64_t)to_color(val_deref(&a[1])->i), out);
}
NativeStatus n_widget_padding(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t v = val_deref(&a[1])->i;
  return widget_with(vm, a, WF_Pad, v < 0 ? 0 : v, out);
}
NativeStatus n_widget_width(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return widget_size(vm, a, WF_Wid, WF_WidFr, val_deref(&a[1])->i, 0, out);
}
NativeStatus n_widget_height(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return widget_size(vm, a, WF_Hei, WF_HeiFr, val_deref(&a[1])->i, 0, out);
}
// 取り分（fr）で決める形。余った場所を、書いた数の比で分け合う。
// float.infinity() は「余りぜんぶ」。0 以下と NaN は指定なしとみなす
NativeStatus n_widget_width_fr(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  double f = val_deref(&a[1])->f;
  return widget_size(vm, a, WF_Wid, WF_WidFr, 0, f > 0 ? f : 0, out);
}
NativeStatus n_widget_height_fr(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  double f = val_deref(&a[1])->f;
  return widget_size(vm, a, WF_Hei, WF_HeiFr, 0, f > 0 ? f : 0, out);
}
// カーソルを合わせたときに出す説明
NativeStatus n_widget_tooltip(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return widget_with_str(vm, a, WF_Tip, as_str(*val_deref(&a[1]))->s, out);
}
// 何も入っていないときに、うすく出しておく字
NativeStatus n_widget_placeholder(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return widget_with_str(vm, a, WF_Hint, as_str(*val_deref(&a[1]))->s, out);
}
// 縁の色（.border）。太さを省くと 1 画素
NativeStatus n_widget_border(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  InstObj* o = widget_copy(vm, a, out);
  if (!o) return N_Panic;
  widget_put(o, WF_Border, mk_int((int64_t)to_color(val_deref(&a[1])->i)));
  if (o->fields[WF_BorderW].i < 1) widget_put(o, WF_BorderW, mk_int(1));
  return N_Ok;
}
// 縁の色と太さ（.border(色, 太さ)）
NativeStatus n_widget_border_w(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  InstObj* o = widget_copy(vm, a, out);
  if (!o) return N_Panic;
  int64_t t = val_deref(&a[2])->i;
  widget_put(o, WF_Border, mk_int((int64_t)to_color(val_deref(&a[1])->i)));
  widget_put(o, WF_BorderW, mk_int(t < 0 ? 0 : t));
  return N_Ok;
}
// 入力欄に入れてよい字（.filter）
NativeStatus n_widget_filter(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return widget_with_str(vm, a, WF_Opt, as_str(*val_deref(&a[1]))->s, out);
}
// 出す小数の桁（.decimals）。書かなければ、限りの広さから決まる
NativeStatus n_widget_decimals(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t v = val_deref(&a[1])->i;
  if (v < 0) v = 0;
  if (v > 9) v = 9;
  return widget_with(vm, a, WF_Dec, v, out);
}
// 角の丸み（.radius）。0 で四角に戻る
NativeStatus n_widget_radius(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t v = val_deref(&a[1])->i;
  return widget_with(vm, a, WF_Radius, v < 0 ? 0 : v, out);
}
// 縦の寄せ方（.valign）。書かなければ、置く側の決めた既定になる
NativeStatus n_widget_valign(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  const Str& s = as_str(*val_deref(&a[1]))->s;
  int va = WV_Top;
  if (s == "middle") va = WV_Middle;
  else if (s == "bottom") va = WV_Bottom;
  else if (!(s == "top")) {
    vm.panic(vm.L(Str("縦の寄せ方は top / middle / bottom のどれかです: ") + s,
                  Str("valign must be top, middle or bottom: ") + s));
    return N_Panic;
  }
  return widget_with(vm, a, WF_VAlign, va, out);
}
NativeStatus n_widget_align(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  const Str& s = as_str(*val_deref(&a[1]))->s;
  int al = WA_Left;
  if (s == "center") al = WA_Center;
  else if (s == "right") al = WA_Right;
  else if (!(s == "left")) {
    vm.panic(vm.L(Str("寄せ方は left / center / right のどれかです: ") + s,
                  Str("align must be left, center or right: ") + s));
    return N_Panic;
  }
  return widget_with(vm, a, WF_Align, al, out);
}

static int w_kind(const Value& v) { return (int)as_inst(v)->fields[WF_Kind].i; }
static const Str& w_text(const Value& v) { return as_str(as_inst(v)->fields[WF_Text])->s; }
static const Str& w_id(const Value& v) { return as_str(as_inst(v)->fields[WF_Id])->s; }
static int64_t w_a(const Value& v) { return as_inst(v)->fields[WF_A].i; }
static int64_t w_b(const Value& v) { return as_inst(v)->fields[WF_B].i; }
static int64_t w_c(const Value& v) { return as_inst(v)->fields[WF_C].i; }
static ListObj* w_kids(const Value& v) { return as_list(as_inst(v)->fields[WF_Kids]); }
// その部品が持っている関数（無ければ 0）
static Value* w_act(const Value& v) {
  InstObj* o = as_inst(v);
  if (WF_Act >= o->fields.size()) return 0;
  Value* a = &o->fields[WF_Act];
  return (a->k == V_Obj && a->o->kind == O_Func) ? a : 0;
}
static const Str& w_str(const Value& v, int i) {
  static const Str kNone;
  InstObj* o = as_inst(v);
  return i < o->fields.size() && o->fields[i].k == V_Obj && o->fields[i].o->kind == O_Str
             ? as_str(o->fields[i])->s : kNone;
}
// 画素の並び（bytes）。文字（string）とは持ち物の種類が違うので、こちらで読む
static const Str& w_bytes(const Value& v, int i) {
  static const Str kNone;
  InstObj* o = as_inst(v);
  return i < o->fields.size() && o->fields[i].k == V_Obj && o->fields[i].o->kind == O_Bytes
             ? as_str(o->fields[i])->s : kNone;
}
static int64_t w_field(const Value& v, int i) {
  InstObj* o = as_inst(v);
  return i < o->fields.size() ? o->fields[i].i : 0;
}
// 動いたときに書き戻す先の var の番号（-1 なら、名札か関数で受ける形）
static int w_var(const Value& v) { return (int)w_field(v, WF_Var); }
static double w_fieldf(const Value& v, int i) {
  InstObj* o = as_inst(v);
  return i < o->fields.size() ? o->fields[i].f : 0.0;
}
static int w_pad(const Value& v) { return (int)w_field(v, WF_Pad); }
static int w_wid(const Value& v) { return (int)w_field(v, WF_Wid); }
static int w_hei(const Value& v) { return (int)w_field(v, WF_Hei); }
// その向きの取り分（fr）。0 なら指定なし
static double w_fr(const Value& v, bool horiz) {
  double f = w_fieldf(v, horiz ? WF_WidFr : WF_HeiFr);
  return f > 0 ? f : 0.0;
}
// float.infinity() を渡されたか（＝余りぜんぶ）。これより大きい数は float に無い
static bool fr_is_fill(double f) { return f > 1.0e308; }
static int w_align(const Value& v) { return (int)w_field(v, WF_Align); }
// 指定が無ければ、いまの見た目の色を使う
// いま、使えない部品（.disabled）の中を置いているか
static bool g_disabled = false;
static uint32_t blend(uint32_t a, uint32_t b, double t);   // 下で定義（2色を混ぜる）

static uint32_t w_fg(const Value& v) {
  int64_t c = w_field(v, WF_Fg);
  if (c < 0) return g_fg;   // 決めていなければ、そのときの文字色（うすさは済んでいる）
  return g_disabled ? blend(g_bg, (uint32_t)c, 0.42) : (uint32_t)c;
}

// .font（字の大きさ）は、その部品と**中の部品ぜんぶ**に効く。
// 測るとき（measure）と置くとき（place）の入口で立てて、抜けるときに戻す。
// 部品の寸法は ui_unit() ごしにここから決まるので、字だけでなく余白や印も釣り合う
struct TextPx {
  int keep;
  TextPx(const Value& v) : keep(g_text_px) {
    int64_t f = w_field(v, WF_Font);
    if (f > 0) g_text_px = (int)f;
  }
  ~TextPx() { g_text_px = keep; }
};

// .disabled も同じく受け継ぐ。いちばん外側の1つだけが効かせて、中はそれに乗る。
//
//   ・色は下地に寄せてうすくする（w_fg と、g_fg / g_accent から作る部品の色）
//   ・カーソルを遠くへやる。**触られたかは inside() で見ている**ので、
//     これだけで押し・つまみ・車輪・カーソルの形まで、まとめて届かなくなる
struct DisabledScope {
  bool applied;
  int mx, my;
  uint32_t fg, accent;
  DisabledScope(const Value& v) : applied(false), mx(0), my(0), fg(0), accent(0) {
    if (g_disabled || !((int)w_field(v, WF_Flags) & WFL_Disabled)) return;
    applied = true;
    g_disabled = true;
    mx = g_mx;
    my = g_my;
    g_mx = -1000000;
    g_my = -1000000;
    fg = g_fg;
    accent = g_accent;
    g_fg = blend(g_bg, g_fg, 0.42);
    g_accent = blend(g_bg, g_accent, 0.35);
  }
  ~DisabledScope() {
    if (!applied) return;
    g_disabled = false;
    g_mx = mx;
    g_my = my;
    g_fg = fg;
    g_accent = accent;
  }
};

// --- 見た目の既定値（spec/library/ui.md）---------------------------------
// 部品の寸法は、**そのときの字の高さ**から決める。
// フォントを変えても、HiDPI で字を大きくしても、見た目の釣り合いが崩れない。
// 内蔵の 5×7（高さ 8）のときに、下の数がそれぞれ 4・6・5・3・96・120 になる
static int ui_unit() {
  int h = cur_text_px();   // その部品が .font を持っていれば、その大きさ
  return h < 6 ? 6 : h;
}
static int gap_y() { return ui_unit() / 2; }          // 縦に並べたときの間
static int gap_x() { return ui_unit() * 3 / 4; }      // 横に並べたときの間
static int pad_x() { return ui_unit() * 5 / 8; }      // ボタンの内側の余白（横）
static int pad_y() { return ui_unit() * 3 / 8; }      // 同じく縦
static int slide_w() { return ui_unit() * 12; }       // つまみの長さ
// 入力欄の内側の余白。ボタンと同じにして、横に並べたときに背が揃うようにする
static int field_pad_x() {
  int n = pad_x();
  return n < 3 ? 3 : n;
}
static int field_pad_y() {
  int n = pad_y();
  return n < 2 ? 2 : n;
}
static int field_w() { return ui_unit() * 15; }       // 入力欄の長さ
static int num_w() { return ui_unit() * 5; }         // 数の入力欄の、字を出すところ
static int step_w() { return ui_unit() * 5 / 4; }    // − と ＋ のボタンの幅
static int arrow_w() { return ui_unit() / 2; }       // 「開く」しるしの三角
// 複数行の入力欄で、右に空けておく巻物の帯のぶん。出ていない間も空けておく
// （出たり消えたりで折り返しが変わると、字が踊って読みにくい）
static int bar_w() {
  int n = ui_unit() / 4;
  return n < 3 ? 3 : n;
}
// 入力欄のカーソルの太さ。字が大きいほど太くする（内蔵の 5×7 なら 1 画素）
static int caret_w() {
  int n = ui_unit() / 8;
  return n < 1 ? 1 : n;
}
// 字の**見た目のまんなか**が、行の箱の上から何画素目か。
// put_text は行の箱を置くだけで、字のインクはその中で下に寄っている（上に行間があく）。
// 印と並べるときに行の箱の真ん中で合わせると、印だけ上にずれて見えるので、
// 大文字の高さの真ん中で合わせる
static int text_mid(int px) {
  if (font_active()) {
    int asc = font_ascender(px);
    FontGlyph g;
    if (font_glyph('H', px, &g) && g.h > 0) return asc - g.top + g.h / 2;
    return asc / 2;
  }
  return (kCellH - 2) * builtin_scale(px) / 2;   // 内蔵の 5×7 は、8 の枠の 0〜6 行目に乗る
}

// 印（チェックの四角・ラジオの丸）と、そのうしろの字とのあいだ
static int mark_gap() {
  int n = ui_unit() / 3;
  return n < 4 ? 4 : n;
}
// 高さ h の箱の中に字を置くときの y。put_text は「行の箱」を置くので、
// (h - line_h) / 2 にすると**字が下に寄って見える**（行の箱は上に行間があく）。
// 字の見た目のまんなかが箱のまんなかに来るところを返す
static int text_y_mid(int y, int h, int px) {
  return y + h / 2 - text_mid(px);
}

static int box_w();   // 下で定義
// 印と字を並べた1行の高さ。どちらもこの高さの真ん中に置くので、高いほうを取る
static int mark_row_h() {
  int h = line_h(cur_text_px()), bw = box_w();
  return bw > h ? bw : h;
}
// チェックの四角と、ラジオの丸。**字の高さに合わせる**（小さいと押しにくく、
// 字と並べたときに沈んで見える）
static int box_w() {
  int h = line_h(cur_text_px()) * 7 / 8;
  if (h < ui_unit()) h = ui_unit();
  if (h < 9) h = 9;
  return (h & 1) ? h : h + 1;   // 奇数にすると、丸が枠にきっちり収まる
}


// この回の送りぶんを、その部品の行の高さで**画素**に直す。
// 1画素に満たないぶんは持ち越すので、ゆっくり動かしても落ちない。
// 呼んだ部品がその回の送りを使い切る（ui.wheel() も 0 になる）
static int take_wheel_px(int lp) {
  if (g_wheel_y == 0) return 0;
  g_wheel_px_sub += g_wheel_y * lp;   // 1/100 行 × 画素/行 = 1/100 画素
  g_wheel_y = 0;
  g_wheel_lines_y = 0;
  int px = g_wheel_px_sub / 100;
  g_wheel_px_sub -= px * 100;
  return px;
}

// 巻物を、目当てのところへ**少しずつ**寄せる。残りの 2/5 ずつ詰め、
// 端数は 1 画素ずつ。60 こま／秒なら 0.2 秒たらずで着く
static int ease_to(int cur, int to) {
  int d = to - cur;
  if (d == 0) return cur;
  int step = d * 2 / 5;
  if (step == 0) step = d > 0 ? 1 : -1;
  int next = cur + step;
  return d > 0 ? (next > to ? to : next) : (next < to ? to : next);
}

// 角を丸めた四角の、i 行目で左右から削るぶん。半径 r の円に沿って削る
// （ui.fill_circle と同じ数え方）
static int round_cut(int i, int h, int r) {
  if (r <= 0) return 0;
  int dy;
  if (i < r) dy = r - i;                    // 上の角
  else if (i >= h - r) dy = i - (h - 1 - r);  // 下の角
  else return 0;
  int k = 0;
  while (k * k + dy * dy <= r * r) k++;     // 1つ行きすぎたところで止まる
  int hw = k - 1;                            // その行での、丸みの半幅
  if (hw < 0) hw = 0;
  return r - hw;
}

// 角を丸めた四角を塗る。r が 0 ならただの四角。
// 角を丸めるだけで、四角い部品はずいぶん柔らかく見える
static void fill_round(int x, int y, int w, int h, int r, uint32_t c) {
  if (w <= 0 || h <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  for (int i = 0; i < h; i++) {
    int cut = round_cut(i, h, r);
    span(x + cut, y + i, w - cut * 2, c);
  }
}

// 角を落とした四角の縁を引く。落とし方は fill_round と揃えてあるので、
// 同じ r を渡せば塗りの上にぴったり乗る
static void stroke_round(int x, int y, int w, int h, int r, int t, uint32_t c) {
  if (w <= 0 || h <= 0 || t <= 0) return;
  if (t * 2 > w) t = w / 2 > 0 ? w / 2 : 1;
  if (t * 2 > h) t = h / 2 > 0 ? h / 2 : 1;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;
  int iw = w - t * 2, ih = h - t * 2;
  int ir = r - t;
  if (ir < 0) ir = 0;
  for (int i = 0; i < h; i++) {
    int cut = round_cut(i, h, r);
    int x0 = x + cut, ww = w - cut * 2;
    if (ww <= 0) continue;
    int j = i - t;                       // 内側（くり抜くところ）から見た行
    if (j < 0 || j >= ih || iw <= 0) { span(x0, y + i, ww, c); continue; }
    int icut = round_cut(j, ih, ir);
    int ix0 = x + t + icut, iww = iw - icut * 2;
    if (iww <= 0) { span(x0, y + i, ww, c); continue; }
    span(x0, y + i, ix0 - x0, c);
    span(ix0 + iww, y + i, x0 + ww - (ix0 + iww), c);
  }
}

// 太さのある線（チェックの印に使う）
static void thick_line(int x0, int y0, int x1, int y1, int t, uint32_t c) {
  int dx = x1 - x0 < 0 ? x0 - x1 : x1 - x0;
  int dy = y1 - y0 < 0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  for (;;) {
    for (int k = 0; k < t; k++)
      for (int j = 0; j < t; j++) put(x0 + j, y0 + k, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err * 2;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 < dx) { err += dx; y0 += sy; }
  }
}

// チェックの印。箱の大きさに合わせて太さも長さも決め、**箱の真ん中**に置く。
// thick_line は右と下へ t 画素ぶん太るので、そのぶんも数に入れて中央に寄せる
static void check_mark(int bx, int by, int w, uint32_t c) {
  int t = w / 7;
  if (t < 1) t = 1;
  int mw = w * 56 / 100;          // 印の幅
  int mh = w * 40 / 100;          // 印の高さ（曲がり角から上まで）
  // 余りを偶数にしておくと、割り切れて**きっちり真ん中**に来る
  if (((w - mw - t) & 1) != 0 && mw + t + 1 <= w) mw++;
  if (((w - mh - t) & 1) != 0 && mh + t + 1 <= w) mh++;
  int ox = bx + (w - mw - t) / 2;
  int oy = by + (w - mh - t) / 2;
  int x0 = ox, y0 = oy + mh * 45 / 100;   // 左上の始まり
  int x1 = ox + mw * 34 / 100, y1 = oy + mh;   // 曲がり角（いちばん下）
  int x2 = ox + mw, y2 = oy;                   // 右上の終わり
  thick_line(x0, y0, x1, y1, t, c);
  thick_line(x1, y1, x2, y2, t, c);
}

static uint32_t blend(uint32_t a, uint32_t b, double t) {
  uint32_t r = 0;
  for (int sh = 16; sh >= 0; sh -= 8) {
    double v = (double)((a >> sh) & 0xff) * (1 - t) + (double)((b >> sh) & 0xff) * t;
    int k = (int)(v + 0.5);
    if (k < 0) k = 0;
    if (k > 255) k = 255;
    r |= (uint32_t)k << sh;
  }
  return r;
}

// --- この回に起きたこと ---------------------------------------------------
static Str g_focus;        // いま文字を受け取っている入力欄の名札
static Str g_hit_id;       // この回に押された部品の名札
static Value g_hit_action;  // その部品が持っていた関数（無ければ値なし）
static bool g_hit_any = false;   // 名札が無くても、何か押されたか
static int64_t g_hit_val = 0;
static double g_hit_valf = 0;   // 小数を持つ部品のときの、新しい値
static Value g_hit_list;        // いくつも選べる一覧の、新しい番号の並び
static Str g_hit_text;
// カーソルが乗っている絵（ui.image）の中の位置。乗っていなければ -1。
// 絵に描く道具（お絵かき）は、これと ui.mouse() で書ける
static int g_point_x = -1, g_point_y = -1;
static bool g_click_seen = false;   // この回にどこかが押されたか（焦点を外すのに使う）
static Str g_focus_next;
static bool g_edited = false;      // この回、ref で受けた部品が変数を書き換えたか
// 最後に ui.show() に渡された部品。窓の縁を引いている間、これを新しい大きさで置き直す
static Value g_last_view;
static bool g_has_view = false;
static int g_last_x = 0, g_last_y = 0;
static bool g_replay = false;      // いま置き直しの最中か（入力の処理は飛ばす）
// ui.show() を呼んだ処理系。ref で受けた入力欄の書き戻しに使う（下の write_var）
static VM* g_vm = 0;

static void clear_hit_action() {
  val_release(g_hit_action);
  g_hit_action = mk_void();
}

// その var に、新しい数（か入切）を入れ直す
static void write_var_num(VM& vm, int slot, int64_t n, bool as_bool) {
  if (slot < 0 || slot >= vm.globals.size()) return;
  Value v = as_bool ? mk_bool(n != 0) : mk_int(n);
  val_release(vm.globals[slot]);
  vm.globals[slot] = v;
}

// 押されたことを覚える。名札と、持っていれば関数も。
// ref で受けた形なら、**その var を直に書き換える**（名札も update() も要らない）
static void hit(const Value& v, int64_t val) {
  int slot = w_var(v);
  if (slot >= 0 && g_vm) {
    int k = w_kind(v);
    write_var_num(*g_vm, slot, val, k == WK_Checkbox || k == WK_Tree);
    g_edited = true;
  }
  g_hit_id = w_id(v);
  g_hit_val = val;
  g_hit_valf = (double)val;
  g_hit_any = true;
  clear_hit_action();
  if (Value* a = w_act(v)) g_hit_action = val_retain(*a);
}

// いくつも選べる一覧で、押された番号を入り切りしたとき。
// ref なら、その var に**新しい並び**を入れ直す（もとの並びは変えない）
static void hit_multi(const Value& v, int i);

// 小数を持つ部品が動いたとき。ref なら、その var に小数を入れ直す
static void hit_f(const Value& v, double val) {
  int slot = w_var(v);
  if (slot >= 0 && g_vm && slot < g_vm->globals.size()) {
    val_release(g_vm->globals[slot]);
    g_vm->globals[slot] = mk_float(val);
    g_edited = true;
  }
  g_hit_id = w_id(v);
  g_hit_val = (int64_t)val;
  g_hit_valf = val;
  g_hit_any = true;
  clear_hit_action();
  if (Value* a = w_act(v)) g_hit_action = val_retain(*a);
}

static void ui_reset_widgets() {
  clear_hit_action();
  val_release(g_hit_list);
  g_hit_list = mk_void();   // 空の並びを作って持ち続けない（ui.chosen がその場で作る）
  val_release(g_last_view);
  g_last_view = mk_void();
  g_has_view = false;
  g_replay = false;
  g_hit_any = false;
  g_edited = false;
  g_vm = 0;
  g_menu_on = false;
  g_menu_items.clear();
  g_menu_keys.clear();
  g_menu_owner.clear();
  g_menu_pick = -1;
  g_menu_px = g_menu_to = 0;
  g_menu_step_at = 0;
  g_menu_open_mx = g_menu_open_my = 0;
  g_caret = g_anchor = g_scroll = g_drag_anchor = g_fcaret = 0;
  g_acaret = g_aanchor = 0;
  g_area_px = g_area_to = 0;
  g_area_id.clear();
  g_vcaret = -1;
  g_agoal = -1;
  g_list_id.clear();
  g_list_px = g_list_to = 0;
  g_list_sel = -1;
  g_list_drag = false;
  g_num_id.clear();
  g_num_text.clear();
  g_slide_id.clear();
  g_sliding = false;
  g_dnum_id.clear();
  g_dnum_x = 0;
  g_dnum_moved = false;
  g_dnum_base = 0;
  g_sc_id.clear();
  g_sc_px.clear();
  g_sc_to.clear();
  g_sc_drag.clear();
  g_multi_id.clear();
  g_multi_n = 0;
  g_multi_at = 0;
  g_undo_id.clear();
  g_undo.clear();
  g_undo_at.clear();
  g_redo.clear();
  g_redo_at.clear();
  g_undo_last = 0;
  g_tip_text.clear();
  g_tip_prev.clear();
  g_tip_since = 0;
  g_menu_min_w = 0;
  g_color_id.clear();
  g_color_drag = 0;
  g_color_moved = false;
  g_dragging = false;
  g_want_caret = -1;
  g_want_id.clear();
  g_focus.clear();
  g_hit_id.clear();
  g_hit_text.clear();
  g_hit_val = 0;
  g_hit_valf = 0;
  g_point_x = g_point_y = -1;
}


// --- ref で受けた部品の、書き戻し先 ---------------------------------------
// 借用（Value*）をそのまま持ち続けると、渡した側の関数が返ったところで宙に浮く。
// そこで覚えるのは借用ではなく、**どの var か**（一番外側の var の番号）。
// 一番外側の var はプログラムが終わるまで生きているので、いつ書き戻しても指し先を失わない。
// 番号にできないもの（関数の中の変数）は型検査が断るので、ふつうはここに来ない（E0307）
static int var_slot(VM& vm, const Value* p) {
  int n = vm.globals.size();
  if (!p || n <= 0) return -1;
  uintptr_t base = (uintptr_t)vm.globals.data();
  uintptr_t q = (uintptr_t)p;
  if (q < base || q >= base + sizeof(Value) * (size_t)n) return -1;
  size_t off = (size_t)(q - base);
  if (off % sizeof(Value) != 0) return -1;
  return (int)(off / sizeof(Value));
}

// その var に、新しい文字を入れ直す
static void write_var(VM& vm, int slot, const Str& s) {
  if (slot < 0 || slot >= vm.globals.size()) return;
  Value v = mk_str(s);
  val_release(vm.globals[slot]);
  vm.globals[slot] = v;
}

// ref で受けた入力欄の名札。どの欄に焦点があるかは名札で覚えているので、番号から作る。
// 先頭に付ける 0x01 は名札に書くような字ではないので、自分で付けた名札とはぶつからない。
// この名札は ui.show() から返さないので、書く人の目に触れることもない
static Str var_field_id(int slot) {
  Str id("\x01");
  id += str_from_int(slot);
  return id;
}

static bool inside(int x, int y, int w, int h) {
  return g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
}

// --- 大きさを測る ---------------------------------------------------------
struct Box { int w, h; };

// いま、どちら向きの並びの中を測って（置いて）いるか。
// ui.spacer は「**並んでいる向き**に場所を取る」ので、これを見る。
// 測るとき（measure）と置くとき（place）と取り分を数えるとき（eff_fr）で、
// 同じ向きを見ていないと大きさが食い違うので、入れ物はどこでもこれを立て直す
static bool g_row_axis = false;
struct RowAxis {   // 入れ物の中にいるあいだだけ立てて、抜けたら戻す
  bool keep;
  RowAxis(bool row) : keep(g_row_axis) { g_row_axis = row; }
  ~RowAxis() { g_row_axis = keep; }
};
// 「余りをぜんぶ取る」取り分。float.infinity() と同じ扱いになる大きさ（fr_is_fill）
static const double kFillFr = 1.7e308;

static Box measure(const Value& v, int wrap_w = 0);
static void grid_axes(const Value& v, int avail_w, int avail_h, Vec<int>& cw, Vec<int>& rh);
static int opt_count(const Value& v);          // 下（並べる文字）で定義
static const Str& opt_at(const Value& v, int i);
static int opt_widest(const Value& v);
static bool is_multi(const Value& v);          // 下（一覧）で定義

// 中身そのものの大きさ（余白も指定も入れない）。
// wrap_w が正なら、文字（ui.label）はその幅で折り返して測る
static Box intrinsic(const Value& v, int wrap_w) {
  Box b;
  b.w = 0;
  b.h = 0;
  switch (w_kind(v)) {
    case WK_Label: {
      const Str& raw = w_text(v);
      if (wrap_w > 0 && text_px_width(raw, cur_text_px()) > wrap_w) {
        Str t = wrap_text(raw, wrap_w, cur_text_px());
        b.w = text_px_width(t, cur_text_px());
        b.h = line_h(cur_text_px()) + (text_lines(t) - 1) * line_pitch(cur_text_px());
      } else {
        b.w = text_px_width(raw, cur_text_px());
        b.h = line_h(cur_text_px()) + (text_lines(raw) - 1) * line_pitch(cur_text_px());
      }
      break;
    }
    case WK_Button: {
      ListObj* k = w_kids(v);
      if (k->v.size() > 0) {   // 中身に部品を入れた形（ui.button(部品, 名札)）
        Box c = measure(k->v[0], wrap_w > 0 ? wrap_w - pad_x() * 2 : 0);
        b.w = c.w + pad_x() * 2;
        b.h = c.h + pad_y() * 2;
      } else {
        b.w = text_px_width(w_text(v), cur_text_px()) + pad_x() * 2;
        b.h = line_h(cur_text_px()) + pad_y() * 2;
      }
      break;
    }
    // 高さは、字の行と印のどちらも収まるぶん。字の高さだけにすると、
    // 字の見た目のまんなかに合わせた印が下からはみ出すことがある
    case WK_Checkbox:
    case WK_Radio:
      b.w = box_w() + mark_gap() + text_px_width(w_text(v), cur_text_px());
      b.h = mark_row_h();
      break;
    case WK_Slider: b.w = slide_w(); b.h = line_h(cur_text_px()) + 2; break;
    case WK_Field: b.w = field_w(); b.h = line_h(cur_text_px()) + field_pad_y() * 2; break;
    case WK_Area: {
      int rows = (int)w_b(v);
      if (rows < 1) rows = 1;
      b.w = field_w();
      b.h = line_h(cur_text_px()) + (rows - 1) * line_pitch(cur_text_px()) + field_pad_y() * 2;
      break;
    }

    case WK_Combo:
      b.w = opt_widest(v) + pad_x() * 3 + arrow_w();
      b.h = line_h(cur_text_px()) + pad_y() * 2;
      break;
    case WK_List: {
      int rows = (int)w_b(v);
      if (rows < 1) rows = 1;
      b.w = opt_widest(v) + field_pad_x() * 2 + bar_w();
      if (is_multi(v)) b.w += line_h(cur_text_px()) + mark_gap();   // レ点のぶん
      b.h = line_h(cur_text_px()) + (rows - 1) * line_pitch(cur_text_px()) + field_pad_y() * 2;
      break;
    }
    case WK_Tabs: {
      for (int i = 0; i < opt_count(v); i++)
        b.w += text_px_width(opt_at(v, i), cur_text_px()) + pad_x() * 2;
      b.h = line_h(cur_text_px()) + pad_y() * 2 + 2;
      break;
    }
    case WK_Number:
      b.w = num_w() + field_pad_x() * 2 + step_w() * 2;
      b.h = line_h(cur_text_px()) + field_pad_y() * 2;
      break;
    case WK_Drag:
      b.w = num_w() + field_pad_x() * 2;
      b.h = line_h(cur_text_px()) + field_pad_y() * 2;
      break;
    case WK_Image:
      b.w = (int)w_a(v);
      b.h = (int)w_b(v);
      break;
    case WK_Color:
      b.h = line_h(cur_text_px()) + field_pad_y() * 2;
      b.w = b.h + field_pad_x() * 2 + text_px_width(Str("#000000"), cur_text_px());
      break;
    case WK_Divider: b.w = 0; b.h = ui_unit() / 2 + 1; break;   // 幅は置くときに決まる
    case WK_Space: b.h = (int)w_a(v); break;
    // 空き場所。**並んでいる向き**にだけ場所を取る（横並びなら横、縦並びなら縦）。
    // 伸びる形（ui.spacer()）は、ここでは 0。余りは取り分（eff_fr）で取る
    case WK_Spacer:
      if (g_row_axis) b.w = (int)w_a(v);
      else b.h = (int)w_a(v);
      break;
    case WK_Column: {
      RowAxis axis(false);
      ListObj* k = w_kids(v);
      for (int i = 0; i < k->v.size(); i++) {
        Box c = measure(k->v[i], wrap_w);
        if (c.w > b.w) b.w = c.w;
        b.h += c.h;
        if (i + 1 < k->v.size()) b.h += gap_y();
      }
      break;
    }
    case WK_Row: {
      RowAxis axis(true);
      ListObj* k = w_kids(v);
      for (int i = 0; i < k->v.size(); i++) {
        Box c = measure(k->v[i]);
        b.w += c.w;
        if (c.h > b.h) b.h = c.h;
        if (i + 1 < k->v.size()) b.w += gap_x();
      }
      break;
    }
    // 折りたためる木。見出しの1行と、開いていれば中身のぶん
    case WK_Tree: {
      int ind = box_w() + mark_gap();
      b.w = ind + text_px_width(w_text(v), cur_text_px());
      b.h = mark_row_h();
      if (w_a(v) != 0) {
        RowAxis axis(false);
        ListObj* k = w_kids(v);
        for (int i = 0; i < k->v.size(); i++) {
          Box c = measure(k->v[i], wrap_w > ind ? wrap_w - ind : 0);
          if (c.w + ind > b.w) b.w = c.w + ind;
          b.h += gap_y() + c.h;
        }
      }
      break;
    }
    // 重ね置き。いちばん大きい中身の大きさになる
    case WK_Stack: {
      ListObj* k = w_kids(v);
      for (int i = 0; i < k->v.size(); i++) {
        Box c = measure(k->v[i], wrap_w);
        if (c.w > b.w) b.w = c.w;
        if (c.h > b.h) b.h = c.h;
      }
      break;
    }
    // 巻物。中身は縦に並ぶ。高さを決めていなければ中身のぶん（巻かれない）
    case WK_Scroll: {
      RowAxis axis(false);
      ListObj* k = w_kids(v);
      for (int i = 0; i < k->v.size(); i++) {
        Box c = measure(k->v[i], wrap_w > 0 ? wrap_w - bar_w() : 0);
        if (c.w > b.w) b.w = c.w;
        b.h += c.h;
        if (i + 1 < k->v.size()) b.h += gap_y();
      }
      b.w += bar_w();
      break;
    }
    case WK_Grid: {
      Vec<int> cw, rh;
      grid_axes(v, 0, 0, cw, rh);
      for (int i = 0; i < cw.size(); i++) b.w += cw[i] + (i + 1 < cw.size() ? gap_x() : 0);
      for (int i = 0; i < rh.size(); i++) b.h += rh[i] + (i + 1 < rh.size() ? gap_y() : 0);
      break;
    }
    default: break;
  }
  return b;
}

// 外から見た大きさ。内側の余白（padding）と、決め打ちの幅・高さを入れる。
// wrap_w は「親がくれる幅」。幅を画素で決めてあれば、折り返しはそちらに合わせる
static Box measure(const Value& v, int wrap_w) {
  TextPx tp(v);          // .font はここから下（中の部品も）に効く
  int p = w_pad(v);
  int inner = w_wid(v) > 0 ? w_wid(v) : wrap_w;
  Box b = intrinsic(v, inner > p * 2 ? inner - p * 2 : 0);
  b.w += p * 2;
  b.h += p * 2;
  if (w_wid(v) > 0) b.w = w_wid(v);
  if (w_hei(v) > 0) b.h = w_hei(v);
  return b;
}

// --- 取り分（fr）を配る ---------------------------------------------------
static bool is_container(const Value& v) {
  int k = w_kind(v);
  return k == WK_Row || k == WK_Column || k == WK_Grid || k == WK_Stack || k == WK_Scroll;
}

static int grid_cols(const Value& v);   // 下（格子）で定義

// その部品の取り分（fr）。自分に書いていなければ、**入れ物は中身から受け継ぐ**。
// だから ui.row の中のボタンに .height(float.infinity()) と書くだけで、
// その row も外（col）の余りを求めて広がり、ボタンが下まで伸びる。
// 入れ物にその向きの大きさ（画素）を書いてあれば、そこで止まる。
//
// 受け継ぎ方は、伸び縮み（measure）と同じ考え方。
//   重なる向き（col の幅、row の高さ）      … いちばん大きいもの
//   積み上がる向き（col の高さ、row の幅）  … 合計
//   格子                                    … 列（行）ごとのいちばん大きいものの合計
static double eff_fr(const Value& v, bool horiz) {
  double f = w_fr(v, horiz);
  if (f > 0) return f;
  // 伸びる空き（ui.spacer()）は、並んでいる向きに余りをぜんぶ取る
  if (w_kind(v) == WK_Spacer && w_b(v) != 0) return horiz == g_row_axis ? kFillFr : 0;
  if (!is_container(v)) return 0;
  if (horiz ? w_wid(v) > 0 : w_hei(v) > 0) return 0;   // 画素で決めてあれば、そこで止まる
  ListObj* k = w_kids(v);
  int kind = w_kind(v);
  // 巻物は、縦には受け継がない。中身がいくら伸びたがっても、巻物の高さは
  // 書く人が決めるもので（決めなければ中身のぶん）、伸ばすと巻けなくなる
  if (kind == WK_Scroll && !horiz) return 0;
  // 中身を見に行くあいだは、その入れ物の向きにする（上の spacer が見る）
  RowAxis axis(kind == WK_Row);
  if (kind == WK_Grid) {
    int cols = grid_cols(v);
    int n = k->v.size();
    int rows = (n + cols - 1) / cols;
    int m = horiz ? cols : rows;
    double acc = 0;
    for (int j = 0; j < m; j++) {
      double mx = 0;
      for (int i = 0; i < n; i++) {
        if ((horiz ? i % cols : i / cols) != j) continue;
        double c = eff_fr(k->v[i], horiz);
        if (c > mx) mx = c;
      }
      acc += mx;
    }
    return acc;
  }
  // その向きに積み上がるか。重ね置き（ui.stack）はどちらにも積み上がらない
  bool stack = kind != WK_Stack && (kind == WK_Row) == horiz;
  double acc = 0;
  for (int i = 0; i < k->v.size(); i++) {
    double c = eff_fr(k->v[i], horiz);
    if (stack) acc += c;
    else if (c > acc) acc = c;
  }
  return acc;
}

// 余った場所を取り分の比で配る。sizes は中身の大きさ、frs はそれぞれの取り分（0 は無し）。
// 取り分を書いていないものは中身の大きさのまま。**余った場所**を、書いた数の比で分ける。
//
//   ui.row([a.width(1.0), b.width(2.0)])   余りを 1:2 で分ける
//   ui.row([a, b.width(float.infinity())]) a は中身の大きさ、b が残り全部
//
// float.infinity()（余りぜんぶ）が混じっている並びでは、ふつうの取り分は
// 中身の大きさに戻り、余りは infinity を書いたものどうしで等分する
static void distribute(Vec<int>& sizes, const Vec<double>& frs, int avail, int gap) {
  int n = sizes.size();
  bool fill = false;
  for (int i = 0; i < n; i++) if (fr_is_fill(frs[i])) fill = true;

  double wsum = 0;
  int fixed = n > 1 ? gap * (n - 1) : 0;
  for (int i = 0; i < n; i++) {
    double f = frs[i];
    if (f > 0 && (!fill || fr_is_fill(f))) wsum += fill ? 1.0 : f;
    else fixed += sizes[i];            // 大きさの決まっているもの。ここは配らない
  }
  if (wsum <= 0) return;               // 取り分を書いたものがいない

  double left = avail - fixed;
  if (left < 0) left = 0;
  double acc = 0;
  int done = 0;
  for (int i = 0; i < n; i++) {
    double f = frs[i];
    if (!(f > 0 && (!fill || fr_is_fill(f)))) continue;
    acc += fill ? 1.0 : f;
    // ここまでに配る量から数えることで、丸めの誤差がたまらない（合計は必ず left）
    int upto = (int)(left * acc / wsum + 0.5);
    sizes[i] = upto - done;
    done = upto;
  }
}

// 縦か横に並べたときの、子ひとりひとりの大きさ。
// wrap_w は折り返しに使う幅（縦に並べるとき、子がもらえる幅）
static void axis_sizes(ListObj* k, bool horiz, int avail, int gap, Vec<int>& out,
                       int wrap_w = 0) {
  Vec<double> frs;
  out.clear();
  for (int i = 0; i < k->v.size(); i++) {
    Box b = measure(k->v[i], horiz ? 0 : wrap_w);
    out.push(horiz ? b.w : b.h);
    frs.push(eff_fr(k->v[i], horiz));
  }
  distribute(out, frs, avail, gap);
}

// 格子の、列ごとの幅と行ごとの高さ。升は左上から右へ、いっぱいになれば次の行へ詰める。
// 列の幅はその列でいちばん広い升、行の高さはその行でいちばん高い升。
// 升に取り分（fr）を書いてあれば、その列（行）の取り分になり、余りを配る。
// avail が 0 のときは配らない（中身の大きさを測るだけ）
static int grid_cols(const Value& v) {
  int c = (int)w_a(v);
  return c < 1 ? 1 : c;
}
static void grid_axes(const Value& v, int avail_w, int avail_h, Vec<int>& cw, Vec<int>& rh) {
  ListObj* k = w_kids(v);
  int cols = grid_cols(v);
  int n = k->v.size();
  int rows = (n + cols - 1) / cols;
  Vec<double> cf, rf;
  cw.clear();
  rh.clear();
  for (int c = 0; c < cols; c++) { cw.push(0); cf.push(0); }
  for (int r = 0; r < rows; r++) { rh.push(0); rf.push(0); }
  for (int i = 0; i < n; i++) {
    int c = i % cols, r = i / cols;
    Box b = measure(k->v[i]);
    if (b.w > cw[c]) cw[c] = b.w;
    if (b.h > rh[r]) rh[r] = b.h;
    double fw = eff_fr(k->v[i], true), fh = eff_fr(k->v[i], false);
    if (fw > cf[c]) cf[c] = fw;
    if (fh > rf[r]) rf[r] = fh;
  }
  if (avail_w > 0) distribute(cw, cf, avail_w, gap_x());
  if (avail_h > 0) distribute(rh, rf, avail_h, gap_y());
}

// --- 描いて、触られたかを見る ---------------------------------------------
// 親がくれる場所（avail_w × avail_h）の中に置く。取り分（fr）を書いた向きは、
// 親がそこに配ったぶんがそのまま渡ってくる（下の axis_sizes）
// root は、ui.show() に渡された（いちばん外の）部品のときだけ true
static void place(const Value& v, int x, int y, int avail_w, int avail_h, bool root = false);

static void place_button(const Value& v, int x, int y, const Box& b) {
  bool over = inside(x, y, b.w, b.h);
  if (over) g_cursor_want = SCUR_Hand;   // 押せるところ
  int64_t bg = w_field(v, WF_Bg);
  uint32_t face = bg >= 0 ? (uint32_t)bg : blend(g_bg, g_accent, over ? 0.55 : 0.3);
  if (bg >= 0 && over) face = blend(face, g_fg, 0.2);
  uint32_t edge = blend(g_bg, g_accent, over ? 1.0 : 0.7);
  for (int i = 0; i < b.h; i++) span(x, y + i, b.w, face);
  span(x + 1, y, b.w - 2, edge);
  span(x + 1, y + b.h - 1, b.w - 2, edge);
  for (int i = 1; i < b.h - 1; i++) { put(x, y + i, edge); put(x + b.w - 1, y + i, edge); }
  // 中身に部品を入れてあれば、それを真ん中に置く（ui.button(部品, 名札)）
  ListObj* k = w_kids(v);
  if (k->v.size() > 0) {
    Box cb = measure(k->v[0], b.w);
    int cx = x + (b.w - cb.w) / 2, cy = y + (b.h - cb.h) / 2;
    // 中身に押されたことを二重に数えさせない（押しを受けるのはボタン）
    int smx = g_mx, smy = g_my;
    g_mx = -1000000;
    g_my = -1000000;
    place(k->v[0], cx, cy, cb.w, cb.h);
    g_mx = smx;
    g_my = smy;
  } else {
    // 決め打ちで広げたときは、ラベルを真ん中に置く
    int tw = text_px_width(w_text(v), cur_text_px());
    put_text(x + (b.w - tw) / 2, text_y_mid(y, b.h, cur_text_px()), w_text(v), cur_text_px(), w_fg(v));
  }
  if (over && g_mpress[0]) hit(v, 1);
}

// 四角い枠と、真ん中のレ点。**中は塗らない**（塗ると印が読みにくく、
// 並べたときに重い）。入っているかは、枠の濃さと印の有無で分かる
static void place_checkbox(const Value& v, int x, int y, const Box& b) {
  bool on = w_a(v) != 0;
  bool over = inside(x, y, b.w, b.h);
  if (over) g_cursor_want = SCUR_Hand;
  int bw = box_w();
  int by = y + (b.h - bw) / 2;              // 印も字も、部品のまんなかに合わせる
  int ty = text_y_mid(y, b.h, cur_text_px());
  uint32_t edge = blend(g_bg, g_accent, on ? 1.0 : (over ? 0.85 : 0.5));
  uint32_t face = blend(g_bg, g_accent, over ? 0.25 : 0.12);
  for (int i = 0; i < bw; i++) span(x, by + i, bw, edge);
  for (int i = 1; i < bw - 1; i++) span(x + 1, by + i, bw - 2, face);
  if (on) check_mark(x, by, bw, blend(g_bg, g_accent, 1.0));
  put_text(x + bw + mark_gap(), ty, w_text(v), cur_text_px(), w_fg(v));
  if (over && g_mpress[0]) hit(v, on ? 0 : 1);
}

static Str widget_key(const Value& v, int x, int y);   // 下（並べる文字）で定義

static void place_slider(const Value& v, int x, int y, const Box& b) {
  // 小数の形（ui.slider(ref v: float, ...)）は、値も限りも WF_Fa/Fb/Fc の側にある
  bool isf = (w_field(v, WF_Flags) & WFL_Float) != 0;
  double flo = isf ? w_fieldf(v, WF_Fb) : 0, fhi = isf ? w_fieldf(v, WF_Fc) : 0;
  double fval = isf ? w_fieldf(v, WF_Fa) : 0;
  if (isf) {
    if (!(fhi > flo)) fhi = flo + 1;
    if (fval < flo) fval = flo;
    if (fval > fhi) fval = fhi;
  }
  int64_t lo = w_b(v), hi = w_c(v), val = w_a(v);
  if (hi <= lo) hi = lo + 1;
  if (val < lo) val = lo;
  if (val > hi) val = hi;
  Str key = widget_key(v, x, y);
  int tw = b.w, knob = box_w() / 2 + 1;
  int usable = tw - knob;
  bool over = inside(x - 3, y - 3, tw + 6, b.h + 6);

  // つかむ・離す。**つかんでいるあいだは、外へ出ても付いてくる**
  if (over && g_mpress[0]) { g_sliding = true; g_slide_id = key; }
  if (!g_mb[0] && g_sliding && g_slide_id == key) { g_sliding = false; g_slide_id.clear(); }
  bool mine = g_sliding && g_slide_id.size() > 0 && g_slide_id == key;
  if (over || mine) g_cursor_want = SCUR_Hand;
  if (mine && usable > 0) {   // つかんでいる間は、そのつど今の位置から値を出す
    int rel = g_mx - x - knob / 2;
    if (rel < 0) rel = 0;
    if (rel > usable) rel = usable;
    if (isf) {
      double nv = flo + (fhi - flo) * (double)rel / (double)usable;
      hit_f(v, nv);
      fval = nv;
    } else {
      int64_t nv = lo + (int64_t)rel * (hi - lo) / usable;
      hit(v, nv);
      val = nv;               // 描くのも新しいところ（1こま遅れない）
    }
  }

  // 描く
  int track_y = y + b.h / 2 - 1;
  span(x, track_y, tw, blend(g_bg, g_fg, 0.35));
  span(x, track_y + 1, tw, blend(g_bg, g_fg, 0.35));
  int kx = isf ? x + (int)((fval - flo) * usable / (fhi - flo) + 0.5)
              : x + (int)((val - lo) * usable / (hi - lo));
  // 通ってきたところは差し色で塗って、どのあたりか分かるようにする
  span(x, track_y, kx - x, blend(g_bg, g_accent, 0.8));
  span(x, track_y + 1, kx - x, blend(g_bg, g_accent, 0.8));
  int r = knob / 3;
  uint32_t face = blend(g_bg, g_accent, mine ? 1.0 : (over ? 0.95 : 0.85));
  fill_round(kx, y, knob, b.h, r, face);
}


// --- 右で押したときのメニュー ---------------------------------------------
// 出すのはこちら（面に描く）。どの出し先でも同じに出る
// この機種で、取り消しや切り貼りに使う修飾キーの書き方
static const char* mod_name() {
  const PlatformOS* os = platform().os;
  if (os && os->name) {
    Str n(os->name());
    if (n == "macos") return "Cmd";
  }
  return "Ctrl";
}

static void menu_open_at(int x, int y, const Vec<Str>& items, const Str& owner,
                         const Vec<Str>* keys = 0) {
  g_menu_on = items.size() > 0;
  g_menu_x = x;
  g_menu_y = y;
  g_menu_items = items;
  g_menu_keys.clear();
  for (int i = 0; i < items.size(); i++)
    g_menu_keys.push(keys && i < keys->size() ? (*keys)[i] : Str());
  g_menu_owner = owner;
  g_menu_min_w = 0;
  g_menu_px = 0;
  g_menu_to = 0;
  g_menu_step_at = 0;
  g_menu_open_mx = g_mx;
  g_menu_open_my = g_my;
}
static int menu_item_h() { return line_h(cur_text_px()) + pad_y() * 2; }
// キーの書き方をいちばん広く出すのに要る幅（無ければ 0）
static int menu_key_w() {
  int w = 0;
  for (int i = 0; i < g_menu_keys.size(); i++) {
    int t = text_px_width(g_menu_keys[i], cur_text_px());
    if (t > w) w = t;
  }
  return w;
}
static int menu_w() {
  int w = 0;
  for (int i = 0; i < g_menu_items.size(); i++) {
    int t = text_px_width(g_menu_items[i], cur_text_px());
    if (t > w) w = t;
  }
  int kw = menu_key_w();
  // キーがあるときは、右の余白を詰めて端っこに寄せる（名前の側は変えない）
  if (kw > 0) w += pad_x() * 2 + kw + pad_x();   // 名前の後ろの空き＋キー＋右の余白
  else w += pad_x() * 2;                          // 右の余白（ふつう）
  w += pad_x() * 2;                               // 左の余白
  return w < g_menu_min_w ? g_menu_min_w : w;
}
// 面に入りきらないときは、**上下に送るしるし**（▲▼）をつけて巻物にする。
// その帯に合わせているあいだ、少しずつ送る（押したまま端まで動かしても送れる）
static int menu_arrow_h() {
  int a = menu_item_h() / 2;
  return a < 4 ? 4 : a;
}
// 一度に見せられる数
static int menu_rows() {
  int n = g_menu_items.size();
  int ih = menu_item_h();
  int avail = g_h - 4;
  if (ih <= 0 || n * ih <= avail) return n;
  int r = (avail - menu_arrow_h() * 2) / ih;
  return r < 1 ? 1 : r;
}
static bool menu_scrolls() { return g_menu_items.size() > menu_rows(); }
static int menu_h() {
  return menu_rows() * menu_item_h() + (menu_scrolls() ? menu_arrow_h() * 2 : 0);
}
// いちばん下まで送ったときの、隠れぶん（画素）
static int menu_max_px() {
  int n = g_menu_items.size(), rows = menu_rows();
  return n > rows ? (n - rows) * menu_item_h() : 0;
}
// 上と下の隠れているぶんを送る。0 は「合わせていない」
static void menu_scroll_step(int x, int y, int w, int h) {
  if (!menu_scrolls()) { g_menu_step_at = 0; return; }
  int ah = menu_arrow_h(), ih = menu_item_h();
  bool up = inside(x, y, w, ah), down = inside(x, y + h - ah, w, ah);
  if (!up && !down) { g_menu_step_at = 0; return; }
  int64_t now = platform().monotonic_nanos();
  if (g_menu_step_at != 0 && now < g_menu_step_at) return;
  g_menu_to += up ? -ih : ih;
  if (g_menu_to < 0) g_menu_to = 0;
  if (g_menu_to > menu_max_px()) g_menu_to = menu_max_px();
  g_menu_step_at = now + 60000000LL;   // 1つ送るごとに 0.06 秒
}
// ui.show の頭で呼ぶ。メニューが出ているあいだの押しは、**下の部品には渡さない**
static void menu_hit() {
  g_menu_pick = -1;
  if (!g_menu_on) return;
  int n = g_menu_items.size();
  int w = menu_w(), ih = menu_item_h(), h = menu_h();
  int rows = menu_rows(), ah = menu_scrolls() ? menu_arrow_h() : 0;
  int x = g_menu_x, y = g_menu_y;   // 置き場は menu_draw が決めたもの（前の回のもの）
  int by = y + ah;
  // 車輪（ホイール）で送る。合っているあいだは、こちらが使い切る
  if (menu_scrolls() && g_wheel_y != 0 && inside(x, y, w, h)) {
    g_menu_to += take_wheel_px(ih);
  }
  menu_scroll_step(x, y, w, h);
  if (g_menu_to < 0) g_menu_to = 0;
  if (g_menu_to > menu_max_px()) g_menu_to = menu_max_px();
  g_menu_px = ease_to(g_menu_px, g_menu_to);   // 目当てへ、少しずつ寄せる
  if (g_menu_px < 0) g_menu_px = 0;
  if (g_menu_px > menu_max_px()) g_menu_px = menu_max_px();
  // カーソルの下にある項目
  int idx = -1;
  if (inside(x, by, w, rows * ih)) {
    int i = (g_my - by + g_menu_px) / ih;
    if (i >= 0 && i < n) idx = i;
  }
  if (g_mpress[0]) {
    if (idx >= 0) g_menu_pick = idx;
    // 送るしるしを押しただけなら、閉じずに送る
    if (idx < 0 && ah > 0 && inside(x, y, w, h)) { g_mpress[0] = false; return; }
    g_menu_on = false;
    g_mpress[0] = false;   // 選んだ／閉じた押しは、ここで飲み込む
    g_mb[0] = false;
  } else if (g_mrel[0]) {
    // **押したまま動かして、項目の上で離した**（プルダウンの選び方）。
    // 出したところ（部品の上）で離しただけなら、開いたままにする
    if (idx >= 0) {
      g_menu_pick = idx;
      g_menu_on = false;
    }
  } else if (g_mrel[2]) {
    // **右で押したまま動かして、項目の上で離した**（プルダウンと同じ選び方）。
    // ほとんど動かさずに離しただけなら、出した場所の真上でも選んだことにしない
    // （出す押しがそのまま項目の上に乗ることがあるため）
    int dx = g_mx - g_menu_open_mx, dy = g_my - g_menu_open_my;
    bool dragged = dx * dx + dy * dy > kMenuDragPx * kMenuDragPx;
    if (idx >= 0 && dragged) {
      g_menu_pick = idx;
      g_menu_on = false;
    }
  } else if (g_mpress[2]) {
    g_menu_on = false;     // 右で押し直したら、いったん閉じる
  }
}
// 三角のしるし。▼ は上が広く、▲ は下が広い
static void tri_down(int x, int y, int w, uint32_t c) {
  for (int i = 0; i * 2 < w; i++) span(x + i, y + i, w - i * 2, c);
}
// 右を向いた三角（閉じている木のしるし）。tri_down を横に倒したもの
static void tri_right(int x, int y, int w, uint32_t c) {
  for (int i = 0; i * 2 < w; i++)
    for (int j = i; j < w - i; j++) put(x + i, y + j, c);
}
static void tri_up(int x, int y, int w, uint32_t c) {
  int rows = (w + 1) / 2;
  for (int i = 0; i < rows; i++) {
    int k = rows - 1 - i;
    span(x + k, y + i, w - k * 2, c);
  }
}
// ui.show の終わりで呼ぶ。いちばん上に描く
static void menu_draw() {
  if (!g_menu_on) return;
  int n = g_menu_items.size();
  int w = menu_w(), ih = menu_item_h(), h = menu_h();
  int rows = menu_rows(), ah = menu_scrolls() ? menu_arrow_h() : 0;
  int x = g_menu_x, y = g_menu_y;
  if (x + w > g_w) x = g_w - w;          // 面からはみ出さない
  if (y + h > g_h) y = g_h - h;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  g_menu_x = x;
  g_menu_y = y;
  uint32_t face = blend(g_bg, g_fg, 0.12), edge = blend(g_bg, g_accent, 0.8);
  for (int i = 0; i < h; i++) span(x, y + i, w, face);
  span(x, y, w, edge);
  span(x, y + h - 1, w, edge);
  for (int i = 0; i < h; i++) { put(x, y + i, edge); put(x + w - 1, y + i, edge); }
  if (inside(x, y, w, h)) g_cursor_want = SCUR_Hand;
  int by = y + ah;
  int top = g_menu_px / ih, off = g_menu_px - (g_menu_px / ih) * ih;
  // 項目は、送るしるしのあいだにだけ出す（半端に切れて出るので、切り抜く）
  int kx0 = g_cx0, ky0 = g_cy0, kx1 = g_cx1, ky1 = g_cy1;
  if (x + 1 > g_cx0) g_cx0 = x + 1;
  if (by > g_cy0) g_cy0 = by;
  if (x + w - 2 < g_cx1) g_cx1 = x + w - 2;
  if (by + rows * ih - 1 < g_cy1) g_cy1 = by + rows * ih - 1;
  for (int r = 0; r <= rows; r++) {
    int i = top + r;
    if (i < 0 || i >= n) break;
    int iy = by + r * ih - off;
    if (inside(x, iy, w, ih))
      for (int k = 1; k < ih - 1; k++) span(x + 1, iy + k, w - 2, blend(g_bg, g_accent, 0.45));
    put_text(x + pad_x() * 2, text_y_mid(iy, ih, cur_text_px()), g_menu_items[i], cur_text_px(), g_fg);
    // キーの書き方は、右に寄せてうすく（読めるが、目立たない）
    if (i < g_menu_keys.size() && g_menu_keys[i].size() > 0) {
      int kw = text_px_width(g_menu_keys[i], cur_text_px());
      put_text(x + w - pad_x() - kw, text_y_mid(iy, ih, cur_text_px()), g_menu_keys[i], cur_text_px(),
               blend(g_bg, g_fg, 0.45));
    }
  }
  g_cx0 = kx0;
  g_cy0 = ky0;
  g_cx1 = kx1;
  g_cy1 = ky1;
  if (ah > 0) {   // まだ上（下）に隠れていれば、送るしるしを濃く出す
    int aw = ah, ax = x + (w - aw) / 2;
    uint32_t more = blend(g_bg, g_fg, 0.9), none = blend(g_bg, g_fg, 0.25);
    for (int k = 0; k < ah; k++) span(x + 1, y + 1 + k, w - 2, face);          // しるしの下地
    for (int k = 0; k < ah; k++) span(x + 1, y + h - 1 - ah + k, w - 2, face);
    tri_up(ax, y + (ah - aw / 2) / 2, aw, g_menu_px > 0 ? more : none);
    tri_down(ax, y + h - ah + (ah - aw / 2) / 2, aw,
             g_menu_px < menu_max_px() ? more : none);
  }
}

// --- 入れてよい字（.filter）-----------------------------------------------
// 決まった呼び名（"digit" "hex" …）か、正規表現の字の組（"[0-9A-F]" "[^ ]"）か、
// 並べた字そのもの（"0123456789.-"）で決める。1字ずつ見るので、打っている
// 途中の「まだ形になっていない」中身でも困らない
static bool class_has(const Str& spec, int cp) {
  int i = 1;                       // 先頭の [ を飛ばす
  bool neg = false;
  if (i < spec.size() && spec[i] == '^') { neg = true; i++; }
  bool found = false;
  int end = spec.size() - 1;       // 末尾の ]
  while (i < end) {
    int lo = 0;
    int adv = utf8_decode(spec, i, &lo);
    if (adv <= 0) break;
    i += adv;
    int hi = lo;
    if (i + 1 < end && spec[i] == '-') {
      int nx = 0;
      int a2 = utf8_decode(spec, i + 1, &nx);
      if (a2 > 0) { hi = nx; i += 1 + a2; }
    }
    if (cp >= lo && cp <= hi) found = true;
  }
  return neg ? !found : found;
}

static bool filter_allows(const Str& spec, int cp) {
  if (spec.size() == 0) return true;
  if (spec == "digit") return cp >= '0' && cp <= '9';
  if (spec == "bin") return cp == '0' || cp == '1';
  if (spec == "oct") return cp >= '0' && cp <= '7';
  if (spec == "hex")
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'f') || (cp >= 'A' && cp <= 'F');
  if (spec == "alpha") return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
  if (spec == "alnum")
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
  if (spec == "ascii") return cp >= 0x20 && cp < 0x7f;
  if (spec.size() >= 2 && spec[0] == '[' && spec[spec.size() - 1] == ']')
    return class_has(spec, cp);
  // 並べた字そのもの
  int at = 0;
  while (at < spec.size()) {
    int c = 0;
    int adv = utf8_decode(spec, at, &c);
    if (adv <= 0) break;
    if (c == cp) return true;
    at += adv;
  }
  return false;
}

// 通らない字を落とした中身。改行は（複数行の入力欄のために）そのまま通す
static Str filter_keep(const Str& spec, const Str& text) {
  if (spec.size() == 0) return text;
  Str out;
  int at = 0;
  while (at < text.size()) {
    int cp = 0;
    int adv = utf8_decode(text, at, &cp);
    if (adv <= 0) break;
    if (cp == '\n' || filter_allows(spec, cp)) out.append(text.data() + at, adv);
    at += adv;
  }
  return out;
}

// マウスの位置が、何文字目にあたるか（from 文字目から右へ数える）
static int char_at_x(const Str& s, int from, int x0, int mx) {
  int n = utf8_len(s);
  int at = utf8_offset(s, from), cx = x0, i = from;
  while (i < n) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    int w = advance_of(cp, cur_text_px());
    if (mx < cx + w / 2) break;
    cx += w;
    at += adv;
    i++;
  }
  return i;
}

// 文字と文字の**あいだ**が、画素のどこにあたるか。字はその枠の左端から描かれるので、
// 境目はその1つ手前にある。ここを外すと、カーソルが右どなりの字に重なって見える
static int edge_x(const Str& s, int from, int upto, int x0) {
  if (upto < from) upto = from;
  return x0 + text_px_width(sub_chars(s, from, upto - from), cur_text_px()) - 1;
}

// --- 取り消しとやり直し ---------------------------------------------------
// 入力欄の中身を、変わる**前**の姿で控えておく。Cmd-Z（Windows と Linux は
// Ctrl-Z）で戻し、Shift を足すとやり直す。
//
// 受け皿（macOS の NSTextView など）の取り消し帳には頼らない。機種ごとに
// 別物になるうえ、こちらが中身を入れ直したときに食い違うため。
static const int kUndoMax = 200;           // これ以上は古いほうから捨てる

static void undo_forget(const Str& id) {
  g_undo_id = id;
  g_undo.clear();
  g_undo_at.clear();
  g_redo.clear();
  g_redo_at.clear();
  g_undo_last = 0;
}

// 中身が変わる前に呼ぶ。続けて打った字は、まとめて1回ぶんにする
static void undo_push(const Str& id, const Str& before, int caret) {
  if (!(g_undo_id == id)) undo_forget(id);
  int64_t now = platform().monotonic_nanos();
  bool join = g_undo.size() > 0 && now - g_undo_last < 500000000LL;   // 0.5 秒
  g_undo_last = now;
  g_redo.clear();          // 新しく打ったら、やり直せる先は消える
  g_redo_at.clear();
  if (join) return;        // 続きなので、前に控えたものをそのまま使う
  if (g_undo.size() >= kUndoMax) {
    g_undo.remove(0);
    g_undo_at.remove(0);
  }
  g_undo.push(before);
  g_undo_at.push(caret);
}

// 取り消し（やり直し）。戻す中身があれば true を返し、out に入れる
static bool undo_take(const Str& id, bool redo, const Str& now_text, int now_caret,
                      Str* out, int* out_caret) {
  if (!(g_undo_id == id)) return false;
  Vec<Str>& from = redo ? g_redo : g_undo;
  Vec<int>& from_at = redo ? g_redo_at : g_undo_at;
  Vec<Str>& to = redo ? g_undo : g_redo;
  Vec<int>& to_at = redo ? g_undo_at : g_redo_at;
  if (from.size() == 0) return false;
  *out = from[from.size() - 1];
  *out_caret = from_at[from_at.size() - 1];
  from.remove(from.size() - 1);
  from_at.remove(from_at.size() - 1);
  to.push(now_text);
  to_at.push(now_caret);
  g_undo_last = 0;         // 戻したあとの打鍵は、まとめない
  return true;
}

// 取り消しの合図か。macOS は Cmd、Windows と Linux は Ctrl。
// Shift を足すとやり直し（Ctrl-Y でもやり直せる）
static bool undo_key(bool* redo) {
  bool mod = g_key[SKEY_Meta] || g_key[SKEY_Ctrl];
  if (!mod) return false;
  if (g_press['y']) { *redo = true; return true; }
  if (!g_press['z']) return false;
  *redo = g_key[SKEY_Shift];
  return true;
}

// --- 続けて押したときの選び方 ---------------------------------------------
// 2回で語、3回で行、4回でぜんぶ。同じ入力欄の近いところを、短いあいだに
// 押したときだけ「続き」と数える
static int multi_click(const Str& id) {
  int64_t now = platform().monotonic_nanos();
  int dx = g_mx - g_multi_x, dy = g_my - g_multi_y;
  if (dx < 0) dx = -dx;
  if (dy < 0) dy = -dy;
  bool same = g_multi_id.size() > 0 && g_multi_id == id && now - g_multi_at < 450000000LL &&
              dx <= 3 && dy <= 3;
  g_multi_n = same ? g_multi_n + 1 : 1;
  g_multi_at = now;
  g_multi_x = g_mx;
  g_multi_y = g_my;
  g_multi_id = id;
  return g_multi_n;
}

// 語の切れ目。空白（半角・全角）とタブと改行で切る。
// 日本語には空白が無いので、書かれたひと続きがまるごと語になる
static bool is_break_cp(int cp) {
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == 0x3000;
}

// i のところを含む「空白で区切られたひと続き」。押したところが空白なら、その空白のかたまり
static void word_range(const Str& s, int i, int* from, int* to) {
  Vec<int> brk;
  int at = 0;
  while (at < s.size()) {   // 終わりを先に見る（utf8_decode は端を見てくれない）
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    brk.push(is_break_cp(cp) ? 1 : 0);
    at += adv;
  }
  int n = brk.size();
  if (n == 0) { *from = 0; *to = 0; return; }
  if (i >= n) i = n - 1;
  if (i < 0) i = 0;
  int want = brk[i], a = i, b = i;
  while (a > 0 && brk[a - 1] == want) a--;
  while (b + 1 < n && brk[b + 1] == want) b++;
  *from = a;
  *to = b + 1;
}

// i のところを含むひと行（改行から改行まで）。折り返しではなく、書かれた改行で切る
static void line_range(const Str& s, int i, int* from, int* to) {
  int n = utf8_len(s);
  if (i > n) i = n;
  if (i < 0) i = 0;
  int at = 0, a = 0, b = n;
  for (int k = 0; k < n; k++) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    if (cp == '\n') {
      if (k < i) a = k + 1;
      else { b = k; break; }
    }
    at += adv;
  }
  *from = a;
  *to = b;
}

// 続けて押された回数から、選ぶ範囲を決める。1回なら「その場所」（幅 0）
static void click_range(const Str& s, int i, int clicks, int* from, int* len) {
  if (clicks >= 4) { *from = 0; *len = utf8_len(s); return; }
  int a = i, b = i;
  if (clicks == 3) line_range(s, i, &a, &b);
  else if (clicks == 2) word_range(s, i, &a, &b);
  *from = a;
  *len = b - a;
}

// 入力欄の中で、右で押したときに出すもの。
// 伏せ字の欄（ui.password）では、中身を取り出すもの（コピー・切り取り）は出さない
static void field_menu(int x, int y, const Str& id, bool masked) {
  Vec<Str> items, keys;
  Str mod(mod_name());
  if (!masked) {
    items.push(Str(g_lang_ja ? "コピー" : "Copy"));
    keys.push(mod + "+C");
    items.push(Str(g_lang_ja ? "切り取り" : "Cut"));
    keys.push(mod + "+X");
  }
  items.push(Str(g_lang_ja ? "貼り付け" : "Paste"));
  keys.push(mod + "+V");
  items.push(Str(g_lang_ja ? "すべて選ぶ" : "Select All"));
  keys.push(mod + "+A");
  menu_open_at(x, y, items, id, &keys);
}

// 伏せ字にした見た目。1字を1つの * に置き換える（数は変えない）
static Str mask_of(const Str& s) {
  Str r;
  int n = utf8_len(s);
  for (int i = 0; i < n; i++) r.push('*');
  return r;
}

static void place_field(const Value& v, int x, int y, const Box& b) {
  Str text = w_text(v);
  Str id = w_id(v);
  // 伏せ字の欄（ui.password）。中身は本物のまま持ち、**出すときと測るときだけ**
  // * に置き換える。字の数は変わらないので、位置の数え方はそのままでよい
  bool masked = w_b(v) != 0;
  bool over = inside(x, y, b.w, b.h);
  if (over) g_cursor_want = SCUR_Text;   // 文字を打つところ
  bool focused = !g_disabled && g_focus.size() > 0 && g_focus == id;
  int tx = x + field_pad_x(), ty = text_y_mid(y, b.h, cur_text_px());
  if (ty < y) ty = y;
  int room = b.w - field_pad_x() * 2;

  // --- 押された・なぞられた ---
  if (over && g_mpress[0]) {
    g_focus_next = id;
    if (!focused) { g_scroll = 0; g_anchor = g_caret = utf8_len(text); }
    int i = char_at_x(masked ? mask_of(text) : text, g_scroll, tx, g_mx);
    // 続けて押されていれば、2回で語、3回で行、4回でぜんぶ
    int st0 = 0, ln0 = 0;
    click_range(text, i, multi_click(id), &st0, &ln0);
    sel_set(text, st0, ln0);
    // 選んでいるところを移植層が持つ形（macOS の窓）では、**受け皿はこのあと
    // input_frame で用意される**ので、いまの sel_set はまだ効かない。
    // 選んだところを覚えておいて、用意できてから入れ直す
    if (sel_from_platform()) { g_want_caret = st0; g_want_len = ln0; g_want_id = id; }
    g_drag_anchor = i;
    g_dragging = ln0 == 0;   // 語や行を選んだあとは、なぞりで上書きしない
  } else if (over && g_mpress[2]) {   // 右で押したら、切り貼りのメニュー
    g_focus_next = id;
    field_menu(g_mx, g_my, id, masked);
  }
  if (g_dragging && focused && g_mb[0]) {
    int j = char_at_x(masked ? mask_of(text) : text, g_scroll, tx, g_mx);
    int a = g_drag_anchor < j ? g_drag_anchor : j;
    int c = g_drag_anchor < j ? j : g_drag_anchor;
    sel_set(text, a, c - a);
  }
  if (!g_mb[0]) g_dragging = false;

  // --- メニューで選ばれたこと ---
  if (focused && g_menu_pick >= 0 && g_menu_owner.size() > 0 && g_menu_owner == id) {
    int st = 0, ln = 0;
    sel_get(text, &st, &ln);
    int pick = masked ? g_menu_pick + 2 : g_menu_pick;   // 伏せ字だと上2つが無い
    if (pick == 0) {
      if (ln > 0) clip_set(sub_chars(text, st, ln));
    } else if (pick == 1) {
      if (ln > 0) { clip_set(sub_chars(text, st, ln)); text = sel_replace(text, Str()); }
    } else if (pick == 2) {
      Str c;
      if (clip_get(&c)) text = sel_replace(text, c);
    } else if (pick == 3) {
      sel_set(text, 0, utf8_len(text));
    }
  }

  // --- 文字を受け取る（置き直しの最中は、描くだけにする）---
  Str marked;
  if (focused && !g_replay) {
    bool was_composing = g_marked.size() > 0;
    // 取り消し・やり直し。合図を先に見て、あとの打鍵と混ざらないようにする
    bool redo = false;
    if (undo_key(&redo)) {
      int st1 = 0, ln1 = 0;
      sel_get(text, &st1, &ln1);
      Str back;
      int back_at = 0;
      if (undo_take(id, redo, text, st1 + ln1, &back, &back_at)) {
        // 中身をまるごと戻す。受け皿を持つ機種では、下の input_frame が
        // 「呼んだ側の中身が変わった」と見て入れ直してくれる
        text = back;
        sel_set(text, back_at > utf8_len(text) ? utf8_len(text) : back_at, 0);
      }
      g_press['z'] = false;   // この欄が使い切る
      g_press['y'] = false;
    }
    Str conf;
    input_frame(tx, ty, line_h(cur_text_px()), text, &conf, &marked);
    // 入れてよい字だけ通す（.filter）。落とした字は、次の回に受け皿へも入れ直される
    conf = filter_keep(w_str(v, WF_Opt), conf);
    if (g_press[SKEY_Enter] && !was_composing) g_focus_next.clear();
    if (!(conf == text)) {   // 打たれて変わった。変わる前の姿を控える
      int st1 = 0, ln1 = 0;
      sel_get(text, &st1, &ln1);
      undo_push(id, text, st1 + ln1);
    }
    text = conf;
    // 受け皿ができた最初の回。押されたところにカーソルを合わせる
    if (g_want_caret >= 0 && g_want_id == id) {
      sel_set(text, g_want_caret, g_want_len);
      g_want_caret = -1;
      g_want_len = 0;
      g_want_id.clear();
    }
    if (!(conf == w_text(v))) {
      int slot = w_var(v);
      if (slot >= 0 && g_vm) {   // ref で受けた形。その var を直に書き換える
        write_var(*g_vm, slot, conf);
        g_edited = true;
      } else {                   // 名札で受ける形。ui.show() が名札を返す
        hit(v, 0);
      }
      g_hit_text = conf;
    }
    // 打たれた字は、この欄が使い切る。こうすると、同じ回にもう一度 ui.show() しても
    // 二度は入らない（ui.run が、状態を変えたあとすぐ描き直すのに要る）
    take_input();
  }

  // --- 見せるところを決める（カーソルが見えるように左を隠す）---
  int st = 0, ln = 0;
  if (focused) sel_get(text, &st, &ln);
  // 変換中は、移植層の「選んでいるところ」が**変換中の字も数に入れて**返るのに、
  // こちらが持っている中身（text）にはその字が入っていない。物差しが違うので、
  // そのまま使うと変換中の字の数だけずれる。変換のあいだは、始めたところに置く
  if (focused && marked.size() > 0) {
    st = g_fcaret;
    ln = 0;
  } else if (focused) {
    g_fcaret = st;   // 選んでいたところは、変換が始まれば置き換わる
  }
  int caret = st + ln;
  if (!focused) g_scroll = 0;
  else {
    Str m = masked ? mask_of(text) : text;
    int n = utf8_len(m);
    if (g_scroll > n) g_scroll = 0;
    if (caret < g_scroll) g_scroll = caret;
    while (g_scroll < caret &&
           text_px_width(sub_chars(m, g_scroll, caret - g_scroll), cur_text_px()) + 2 > room)
      g_scroll++;
    // 末尾を消すなどして右に空きができたら、左に隠した分を戻して詰める
    while (g_scroll > 0 &&
           text_px_width(sub_chars(m, g_scroll - 1, n - (g_scroll - 1)), cur_text_px()) + 2 <= room)
      g_scroll--;
  }
  int scroll = focused ? g_scroll : 0;
  // ここから先、出すのと測るのは view のほう（伏せ字なら * の並び）
  Str view = masked ? mask_of(text) : text;
  if (masked) marked = mask_of(marked);

  // --- 描く ---
  uint32_t edge = blend(g_bg, focused ? g_accent : g_fg, focused ? 1.0 : 0.45);
  int64_t fbg = w_field(v, WF_Bg);
  for (int i = 0; i < b.h; i++)
    span(x, y + i, b.w, fbg >= 0 ? (uint32_t)fbg : blend(g_bg, g_fg, 0.08));
  span(x, y, b.w, edge);
  span(x, y + b.h - 1, b.w, edge);
  for (int i = 0; i < b.h; i++) { put(x, y + i, edge); put(x + b.w - 1, y + i, edge); }

  // カーソルと帯は、字の少し上から少し下まで。字と同じところに揃える
  int top = ty - 1, bot = ty + line_h(cur_text_px());
  if (top < y + 1) top = y + 1;
  if (bot > y + b.h - 2) bot = y + b.h - 2;

  // 枠の外にはみ出さないように切り抜く。左を隠して見せているので、
  // **カーソルより後ろの字は枠より右に伸びる**。ここで止める
  int kx0 = g_cx0, ky0 = g_cy0, kx1 = g_cx1, ky1 = g_cy1;
  if (x + 1 > g_cx0) g_cx0 = x + 1;
  if (y + 1 > g_cy0) g_cy0 = y + 1;
  if (x + b.w - 2 < g_cx1) g_cx1 = x + b.w - 2;
  if (y + b.h - 2 < g_cy1) g_cy1 = y + b.h - 2;

  // 選んでいるところに帯を敷く
  if (focused && ln > 0) {
    int a = st < scroll ? scroll : st;
    int ax = edge_x(view, scroll, a, tx);
    int bx = edge_x(view, scroll, st + ln, tx);
    if (bx > x + b.w - field_pad_x()) bx = x + b.w - field_pad_x();
    if (bx > ax)
      for (int i = top; i <= bot; i++) span(ax, i, bx - ax, blend(g_bg, g_accent, 0.5));
  }

  Str shown = sub_chars(view, scroll, utf8_len(view) - scroll);
  if (focused && marked.size() > 0) {   // 変換中の字は、カーソルのところに挟む
    int n = utf8_len(view);
    shown = sub_chars(view, scroll, caret - scroll);
    shown += marked;
    shown += sub_chars(view, caret, n - caret);
  }
  // 何も入っていなければ、うすく「何を入れるところか」を出す（.placeholder）。
  // 打ち始めたら消える。伏せ字の欄でも、これは伏せない（中身ではないため）
  const Str& hint = w_str(v, WF_Hint);
  if (shown.size() == 0 && hint.size() > 0)
    put_text(tx, ty, hint, cur_text_px(), blend(g_bg, w_fg(v), 0.45));
  else
    put_text(tx, ty, shown, cur_text_px(), w_fg(v));

  if (focused) {
    int cx = edge_x(view, scroll, caret, tx);
    if (marked.size() > 0) {   // 変換中のところに下線
      int mw = text_px_width(marked, cur_text_px());
      span(cx + 1, ty + line_h(cur_text_px()) - 1, mw, g_accent);
      cx += mw;
    }
    // カーソルは差し色で、字の大きさに合わせて太く引く。枠も差し色だが、
    // 太さが違うので見分けがつく（枠は1画素の線）
    if (ln == 0 || marked.size() > 0) {
      int cw = caret_w();
      int cx0 = cx - (cw - 1) / 2;
      if (cx0 < x + 1) cx0 = x + 1;                          // 枠には掛けない
      if (cx0 + cw > x + b.w - 1) cx0 = x + b.w - 1 - cw;
      for (int i = top; i <= bot; i++) span(cx0, i, cw, g_accent);
    }
  }
  g_cx0 = kx0;
  g_cy0 = ky0;
  g_cx1 = kx1;
  g_cy1 = ky1;
}

// --- 複数行の入力欄 -------------------------------------------------------
//
// 中身は改行を持つ**1つの文字列**。見た目の行は、改行と、置ける幅からの折り返しで
// 決まる。折り返す幅を知っているのはこちらなので、**行にまつわるキー
// （enter・上下・home・end）はここが受け持つ**。移植層の受け皿には渡さない
// （platform.h の text_input の multiline）。渡すと、向こうの折り返し方で
// もう一度動いてしまう。
//
// 数え方はどこも**文字**（バイトではない）。starts[i] はその行の最初の文字、
// counts[i] はその行の文字の数で、行末の改行は数に入れない
static void area_lines(const Str& s, int room, Vec<int>* starts, Vec<int>* counts) {
  starts->clear();
  counts->clear();
  int n = utf8_len(s);
  int at = 0, i = 0;      // バイトの位置と、文字の位置
  int start = 0;          // この行の最初の文字
  int width = 0;          // ここまでの幅
  int sp = -1, sp_w = 0;  // 最後に見た空白の**次**の文字と、そこまでの幅
  while (i < n) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    if (cp == '\n') {     // ここで行が終わる（改行そのものは行に入れない）
      starts->push(start);
      counts->push(i - start);
      at += adv;
      i++;
      start = i;
      width = 0;
      sp = -1;
      continue;
    }
    int cw = advance_of(cp, cur_text_px());
    if (room > 0 && width + cw > room && i > start) {   // 入りきらないので折り返す
      starts->push(start);
      if (sp > start) {   // その行の空白で折る
        counts->push(sp - start);
        start = sp;
        width -= sp_w;
      } else {            // 空白が無ければ、この字の手前で折る
        counts->push(i - start);
        start = i;
        width = 0;
      }
      sp = -1;
    }
    if (cp == ' ') { sp = i + 1; sp_w = width + cw; }
    width += cw;
    at += adv;
    i++;
  }
  starts->push(start);
  counts->push(n - start);
}

// その文字が何行目にあるか。折り返したところは、次の行の頭とみなす
static int area_row_of(const Vec<int>& starts, int caret) {
  int r = 0;
  for (int i = 0; i < starts.size(); i++) {
    if (starts[i] > caret) break;
    r = i;
  }
  return r;
}

// 箱の中身の高さに、見た目の行が何行入るか
static int area_rows_shown(int inner_h) {
  int lh = line_h(cur_text_px()), lp = line_pitch(cur_text_px());
  if (lp <= 0 || inner_h < lh) return 1;
  return 1 + (inner_h - lh) / lp;
}

// マウスの居るところが、何文字目にあたるか
// cur は、上に隠しているぶん（画素）。半端に切れて出ている行も数に入れる
static int area_index_at(const Str& text, const Vec<int>& starts, const Vec<int>& counts,
                         int ty, int tx, int shown, int cur) {
  int lp = line_pitch(cur_text_px());
  if (lp <= 0) lp = 1;
  int top = cur / lp;
  int r = g_my < ty ? top : (g_my - ty + cur) / lp;
  if (r > top + shown) r = top + shown;
  if (r >= starts.size()) r = starts.size() - 1;
  if (r < 0) r = 0;
  int i = char_at_x(text, starts[r], tx, g_mx);
  int end = starts[r] + counts[r];
  return i > end ? end : i;
}

// カーソルと錨を、いま選ばれているところに合わせる。
// 移植層は「選んでいるところ」は持つが、**どちら端にカーソルがあるか**までは
// 持たないので、食い違いを見つけたときだけ右端に置き直す
static void area_sync(const Str& text) {
  int st = 0, ln = 0;
  sel_get(text, &st, &ln);
  int lo = g_acaret < g_aanchor ? g_acaret : g_aanchor;
  int hi = g_acaret < g_aanchor ? g_aanchor : g_acaret;
  if (lo == st && hi == st + ln) return;
  if (!sel_from_platform()) { g_aanchor = g_anchor; g_acaret = g_caret; return; }
  g_aanchor = st;
  g_acaret = st + ln;
}

// カーソルを動かす。shift を押していれば、錨はそのままで選びながら動く
static void area_move(const Str& text, int to, bool shift) {
  int n = utf8_len(text);
  if (to < 0) to = 0;
  if (to > n) to = n;
  g_acaret = to;
  if (!shift) g_aanchor = to;
  int a = g_aanchor < g_acaret ? g_aanchor : g_acaret;
  int c = g_aanchor < g_acaret ? g_acaret : g_aanchor;
  sel_set(text, a, c - a);
  // 自前で数えている出し先では、どちら端にカーソルがあるかも入れ直しておく
  if (!sel_from_platform()) { g_anchor = g_aanchor; g_caret = g_acaret; }
}

static void place_area(const Value& v, int x, int y, const Box& b) {
  Str text = w_text(v);
  Str id = w_id(v);
  bool over = inside(x, y, b.w, b.h);
  if (over) g_cursor_want = SCUR_Text;   // 文字を打つところ
  bool focused = !g_disabled && g_focus.size() > 0 && g_focus == id;
  int lh = line_h(cur_text_px()), lp = line_pitch(cur_text_px());
  int tx = x + field_pad_x(), ty = y + field_pad_y();
  int room = b.w - field_pad_x() * 2 - bar_w();
  if (room < 1) room = 1;
  int shown = area_rows_shown(b.h - field_pad_y() * 2);

  Vec<int> starts, counts;
  area_lines(text, room, &starts, &counts);
  int nrows = starts.size();

  // 見せるところ。覚えているのは**最後に触った入力欄のぶんだけ**。
  // 車輪（ホイール）なら、焦点が無くても乗せているだけで送れる。
  // 位置は画素で持ち、目当て（to）へ少しずつ寄せる（送っている途中が出る）
  bool mine = g_area_id.size() > 0 && g_area_id == id;
  int max_px = nrows > shown ? (nrows - shown) * lp : 0;
  int to = mine ? g_area_to : 0, cur = mine ? g_area_px : 0;
  if (over && g_wheel_y != 0 && nrows > shown) {
    to += take_wheel_px(lp);          // この入力欄が使い切る
    g_area_id = id;
    mine = true;
  }
  if (to < 0) to = 0;
  if (to > max_px) to = max_px;
  cur = ease_to(cur, to);
  if (cur < 0) cur = 0;
  if (cur > max_px) cur = max_px;
  int top = lp > 0 ? cur / lp : 0;
  int off = cur - top * lp;

  // --- 押された・なぞられた ---
  if (over && g_mpress[0]) {
    g_focus_next = id;
    if (!focused) g_aanchor = g_acaret = 0;
    g_agoal = -1;
    int i = area_index_at(text, starts, counts, ty, tx, shown, cur);
    // 続けて押されていれば、2回で語、3回で行（折り返しではなく書かれた改行で）、
    // 4回でぜんぶ
    int st0 = 0, ln0 = 0;
    click_range(text, i, multi_click(id), &st0, &ln0);
    sel_set(text, st0, ln0);
    g_aanchor = st0;
    g_acaret = st0 + ln0;
    if (ln0 > 0) i = st0 + ln0;
    // 選んでいるところを移植層が持つ形では、受け皿は焦点が移る次の回にできる。
    // 押されたところを覚えておいて、受け皿ができてから入れ直す（ui.field と同じ）
    if (sel_from_platform()) { g_want_caret = st0; g_want_len = ln0; g_want_id = id; }
    g_drag_anchor = i;
    g_dragging = ln0 == 0;   // 語や行を選んだあとは、なぞりで上書きしない
  } else if (over && g_mpress[2]) {   // 右で押したら、切り貼りのメニュー
    g_focus_next = id;
    field_menu(g_mx, g_my, id, false);
  }
  if (g_dragging && focused && g_mb[0]) {
    int j = area_index_at(text, starts, counts, ty, tx, shown, cur);
    int a = g_drag_anchor < j ? g_drag_anchor : j;
    int c = g_drag_anchor < j ? j : g_drag_anchor;
    sel_set(text, a, c - a);
    g_aanchor = g_drag_anchor;
    g_acaret = j;
  }
  if (!g_mb[0]) g_dragging = false;

  // --- メニューで選ばれたこと ---
  if (focused && g_menu_pick >= 0 && g_menu_owner.size() > 0 && g_menu_owner == id) {
    int st = 0, ln = 0;
    sel_get(text, &st, &ln);
    if (g_menu_pick == 0) {
      if (ln > 0) clip_set(sub_chars(text, st, ln));
    } else if (g_menu_pick == 1) {
      if (ln > 0) { clip_set(sub_chars(text, st, ln)); text = sel_replace(text, Str()); }
    } else if (g_menu_pick == 2) {
      Str c;
      if (clip_get(&c)) text = sel_replace(text, c);
    } else if (g_menu_pick == 3) {
      sel_set(text, 0, utf8_len(text));
    }
    area_lines(text, room, &starts, &counts);
    nrows = starts.size();
  }

  // --- 文字を受け取る（置き直しの最中は、描くだけにする）---
  Str marked;
  if (focused && !g_replay) {
    bool was_composing = g_marked.size() > 0;
    bool shift = g_key[SKEY_Shift];
    // 押されたばかりで、受け皿がまだできていない回。カーソルは下で入れ直すので、
    // この回だけ行を動かすキーを見送る（覚えているカーソルがまだ当てにならない）
    bool settling = g_want_caret >= 0 && g_want_id == id;
    // 行にまつわるキーは、折り返しを知っているこちらが受け持つ。
    // 変換の最中は、そのキーは変換のもの（確定・取り消し・候補選び）なので触らない
    if (!was_composing && !settling) {
      if (g_press[SKEY_Enter]) {   // 改行を入れる（ui.field は入力欄から離れる）
        int st = 0, ln = 0;
        sel_get(text, &st, &ln);
        text = sel_replace(text, Str("\n"));
        g_aanchor = g_acaret = st + 1;
        area_lines(text, room, &starts, &counts);
        nrows = starts.size();
        g_agoal = -1;
      }
      bool up = g_press[SKEY_Up], down = g_press[SKEY_Down];
      bool pgup = g_press[SKEY_PageUp], pgdn = g_press[SKEY_PageDown];
      int page = shown - 1 > 0 ? shown - 1 : 1;
      if (up || down || pgup || pgdn) {
        int r = area_row_of(starts, g_acaret);
        // 上下に動かすときは、**最初にいた横の位置**を目指す。
        // 短い行を通り抜けても、元の位置に戻ってくる
        int gx = g_agoal >= 0
                     ? g_agoal
                     : text_px_width(sub_chars(text, starts[r], g_acaret - starts[r]), cur_text_px());
        int nr = r + (up ? -1 : down ? 1 : pgup ? -page : page);
        if (nr < 0) nr = 0;
        if (nr >= nrows) nr = nrows - 1;
        int to = char_at_x(text, starts[nr], 0, gx);
        int end = starts[nr] + counts[nr];
        area_move(text, to > end ? end : to, shift);
        g_agoal = gx;
      } else if (g_press[SKEY_Home] || g_press[SKEY_End]) {
        int r = area_row_of(starts, g_acaret);
        area_move(text, g_press[SKEY_Home] ? starts[r] : starts[r] + counts[r], shift);
      } else if (g_press[SKEY_Escape]) {
        g_focus_next.clear();   // 入力欄から離れる（外を押すのと同じ）
      }
    }
    // 横に動いたら、上下の目当ては忘れる
    if (g_typed.size() > 0 || g_press[SKEY_Left] || g_press[SKEY_Right] ||
        g_press[SKEY_Back] || g_press[SKEY_Delete] || g_press[SKEY_Home] || g_press[SKEY_End])
      g_agoal = -1;

    // 取り消し・やり直し
    bool redo = false;
    if (!was_composing && undo_key(&redo)) {
      Str back;
      int back_at = 0;
      if (undo_take(id, redo, text, g_acaret, &back, &back_at)) {
        text = back;   // 中身をまるごと戻す（受け皿は input_frame が入れ直す）
        int n = utf8_len(text);
        g_aanchor = g_acaret = back_at > n ? n : back_at;
        sel_set(text, g_acaret, 0);
        area_lines(text, room, &starts, &counts);
        nrows = starts.size();
      }
      g_press['z'] = false;
      g_press['y'] = false;
    }

    // 変換の候補は、カーソルのある行のあたりに出してもらう
    int cr = area_row_of(starts, g_acaret) - top;
    if (cr < 0) cr = 0;
    if (cr > shown - 1) cr = shown - 1;
    Str conf;
    input_frame(tx, ty + cr * lp, lh, text, &conf, &marked, true);
    conf = filter_keep(w_str(v, WF_Opt), conf);           // 入れてよい字だけ通す（.filter）
    if (!(conf == text)) undo_push(id, text, g_acaret);   // 変わる前の姿を控える
    text = conf;
    // 受け皿ができた最初の回。押されたところにカーソルを合わせる
    if (g_want_caret >= 0 && g_want_id == id) {
      sel_set(text, g_want_caret, g_want_len);
      g_aanchor = g_want_caret;
      g_acaret = g_want_caret + g_want_len;
      g_want_caret = -1;
      g_want_len = 0;
      g_want_id.clear();
    }
    if (!(conf == w_text(v))) {
      int slot = w_var(v);
      if (slot >= 0 && g_vm) {   // ref で受けた形。その var を直に書き換える
        write_var(*g_vm, slot, conf);
        g_edited = true;
      } else {                   // 名札で受ける形。ui.show() が名札を返す
        hit(v, 0);
      }
      g_hit_text = conf;
    }
    // 打たれた字と、行を動かすキーは、この欄が使い切る
    take_input(true);
    area_lines(text, room, &starts, &counts);
    nrows = starts.size();
    // 変換中は、移植層の「選んでいるところ」が**変換中の字も数に入れて**返るのに、
    // こちらが持っている中身（text）にはその字が入っていない。物差しが違うので、
    // そのまま合わせると変換中の字の数だけずれる。だから変換のあいだは動かさず、
    // **始めたところ**に置いたままにして、確定してから数え直す
    if (marked.size() == 0) {
      area_sync(text);
    } else if (!was_composing) {
      // 変換が始まった回。選んでいたところは変換中の字で置き換わるので、
      // カーソルはその頭に寄せる
      if (g_aanchor < g_acaret) g_acaret = g_aanchor;
      else g_aanchor = g_acaret;
    }
  }

  // --- カーソルの行が見えるところまで送る ---
  int st = 0, ln = 0, caret = 0;
  max_px = nrows > shown ? (nrows - shown) * lp : 0;   // 中身が変わっているかもしれない
  if (focused) {
    st = g_aanchor < g_acaret ? g_aanchor : g_acaret;
    ln = (g_aanchor < g_acaret ? g_acaret : g_aanchor) - st;
    caret = g_acaret;
    int r = area_row_of(starts, caret);
    // 送るのは**カーソルが動いた回だけ**。毎回だと、車輪でずらしたそばから戻ってしまう。
    // 打っている最中に間が空くと読みにくいので、こちらはその場で合わせる
    if (!mine || g_vcaret != caret) {
      if (r * lp < to) to = r * lp;
      if (r * lp > to + (shown - 1) * lp) to = (r - shown + 1) * lp;
      if (to < 0) to = 0;
      if (to > max_px) to = max_px;
      cur = to;
    }
    g_vcaret = caret;
  }
  if (to > max_px) to = max_px;
  if (cur > max_px) cur = max_px;
  if (to < 0) to = 0;
  if (cur < 0) cur = 0;
  top = lp > 0 ? cur / lp : 0;
  off = cur - top * lp;
  // 巻物の位置を覚えるのは、触った入力欄のぶんだけ
  if (mine || focused) {
    g_area_id = id;
    g_area_px = cur;
    g_area_to = to;
  }

  // --- 描く ---
  uint32_t edge = blend(g_bg, focused ? g_accent : g_fg, focused ? 1.0 : 0.45);
  int64_t fbg = w_field(v, WF_Bg);
  for (int i = 0; i < b.h; i++)
    span(x, y + i, b.w, fbg >= 0 ? (uint32_t)fbg : blend(g_bg, g_fg, 0.08));
  span(x, y, b.w, edge);
  span(x, y + b.h - 1, b.w, edge);
  for (int i = 0; i < b.h; i++) { put(x, y + i, edge); put(x + b.w - 1, y + i, edge); }

  // 枠の外にはみ出さないように切り抜く。字も帯もカーソルも、この中だけに描く
  int kx0 = g_cx0, ky0 = g_cy0, kx1 = g_cx1, ky1 = g_cy1;
  if (x + 1 > g_cx0) g_cx0 = x + 1;
  if (y + 1 > g_cy0) g_cy0 = y + 1;
  if (x + b.w - 2 < g_cx1) g_cx1 = x + b.w - 2;
  if (y + b.h - 2 < g_cy1) g_cy1 = y + b.h - 2;

  // 何も入っていなければ、うすく「何を入れるところか」を出す（.placeholder）
  const Str& hint = w_str(v, WF_Hint);
  if (text.size() == 0 && marked.size() == 0 && hint.size() > 0) {
    Str t = text_px_width(hint, cur_text_px()) > room ? wrap_text(hint, room, cur_text_px()) : hint;
    put_text(tx, ty, t, cur_text_px(), blend(g_bg, w_fg(v), 0.45));
  }

  int caret_row = focused ? area_row_of(starts, caret) : -1;
  for (int r = top; r < nrows && r - top <= shown; r++) {
    int ry = ty + (r - top) * lp - off;
    int rs = starts[r], re = starts[r] + counts[r];
    // 選んでいるところに帯を敷く。改行まで選んでいるときは、そのぶんも出す
    // （折り返しただけの行末には、選ぶものが無いので足さない）
    bool wrapped = r + 1 < nrows && starts[r + 1] == re;
    if (focused && ln > 0) {
      int a = st > rs ? st : rs;
      int c = (st + ln) < re ? (st + ln) : re;
      bool crosses = !wrapped && st <= re && st + ln > re;
      if (a <= c && (c > a || crosses)) {
        int ax = edge_x(text, rs, a, tx);
        int bx = edge_x(text, rs, c, tx) + (crosses ? advance_of(' ', cur_text_px()) : 0);
        if (bx > ax)
          for (int k = ry - 1; k <= ry + lh; k++) span(ax, k, bx - ax, blend(g_bg, g_accent, 0.5));
      }
    }
    // 字。変換中の文字は、カーソルのところに挟んで、下線をつけて出す
    if (focused && marked.size() > 0 && r == caret_row) {
      Str head = sub_chars(text, rs, caret - rs);
      put_text(tx, ry, head, cur_text_px(), w_fg(v));
      int mx0 = tx + text_px_width(head, cur_text_px());
      put_text(mx0, ry, marked, cur_text_px(), w_fg(v));
      int mw = text_px_width(marked, cur_text_px());
      span(mx0, ry + lh - 1, mw, g_accent);
      put_text(mx0 + mw, ry, sub_chars(text, caret, re - caret), cur_text_px(), w_fg(v));
    } else {
      put_text(tx, ry, sub_chars(text, rs, re - rs), cur_text_px(), w_fg(v));
    }
    // カーソル。差し色で、字の大きさに合わせて太く引く
    if (focused && r == caret_row && (ln == 0 || marked.size() > 0)) {
      int cx = edge_x(text, rs, caret, tx);
      if (marked.size() > 0) cx += text_px_width(marked, cur_text_px());
      int cw = caret_w();
      for (int k = ry - 1; k <= ry + lh; k++) span(cx, k, cw, g_accent);
    }
  }

  // 入りきらないときの帯。どのあたりを見ているかが分かる
  if (nrows > shown) {
    int bw = bar_w() - 1;
    if (bw < 2) bw = 2;
    int bx = x + b.w - 1 - bw, by = y + 2, bh = b.h - 4;
    for (int i = 0; i < bh; i++) span(bx, by + i, bw, blend(g_bg, g_fg, 0.18));
    int th = bh * shown / nrows;
    if (th < 4) th = 4;
    if (th > bh) th = bh;
    int off = (bh - th) * top / (nrows - shown);
    for (int i = 0; i < th; i++) span(bx, by + off + i, bw, blend(g_bg, g_accent, 0.7));
  }
  g_cx0 = kx0;
  g_cy0 = ky0;
  g_cx1 = kx1;
  g_cy1 = ky1;
}

// --- ラジオ・選ぶ・一覧・タブ・数 ------------------------------------------
//
// どれも「値は呼んだ側が持ち、部品は状態を持たない」は同じ。動いたときに
// ui.show() が名札を返し、新しい値は ui.value() で受け取る。

// 塗った丸（ラジオボタン用）。ui.fill_circle と同じ書き方
static void disc(int cx, int cy, int r, uint32_t c) {
  for (int dy = -r; dy <= r; dy++) {
    int w = 0;
    while ((w + 1) * (w + 1) + dy * dy <= r * r) w++;
    span(cx - w, cy + dy, w * 2 + 1, c);
  }
}

// 並べる文字（選ぶ・一覧・タブ）。入れ物ではないので、中身の置き場（kids）を
// 部品ではなく**文字の並び**として借りている
static int opt_count(const Value& v) { return w_kids(v)->v.size(); }
static const Str& opt_at(const Value& v, int i) {
  static const Str kNone;
  ListObj* k = w_kids(v);
  if (i < 0 || i >= k->v.size()) return kNone;
  const Value& e = k->v[i];
  return (e.k == V_Obj && e.o->kind == O_Str) ? as_str(e)->s : kNone;
}
static int opt_widest(const Value& v) {
  int w = 0;
  for (int i = 0; i < opt_count(v); i++) {
    int t = text_px_width(opt_at(v, i), cur_text_px());
    if (t > w) w = t;
  }
  return w;
}

// 名札の無い形（関数を渡した形）でも、焦点や一覧の持ち主を見分けられるように、
// 置かれた場所から名札を作る。先頭の 0x02 は名札に書くような字ではないので、
// 自分で付けた名札とはぶつからない
static Str widget_key(const Value& v, int x, int y) {
  const Str& id = w_id(v);
  if (id.size() > 0) return id;
  int slot = w_var(v);
  if (slot >= 0) {   // ref で受けた形。どの var かで見分けられる（置き場所が動いても同じ）
    Str k("\x03");
    k += str_from_int(slot);
    return k;
  }
  Str k("\x02");
  k += str_from_int(x);
  k.push(',');
  k += str_from_int(y);
  return k;
}

// いくつも選べる一覧（ui.listbox の list<int> を渡す形）で、その番号が選ばれているか
static bool is_multi(const Value& v) {
  return (w_field(v, WF_Flags) & WFL_Multi) != 0;
}
static ListObj* chosen_of(const Value& v) {
  InstObj* o = as_inst(v);
  if (WF_Sel >= o->fields.size()) return 0;
  Value& f = o->fields[WF_Sel];
  return (f.k == V_Obj && f.o->kind == O_List) ? as_list(f) : 0;
}
static bool is_chosen(const Value& v, int i) {
  ListObj* c = chosen_of(v);
  if (!c) return false;
  for (int j = 0; j < c->v.size(); j++) if (c->v[j].i == i) return true;
  return false;
}
// 押された番号を入り切りした、新しい並び（もとの順は変えず、足すときは末尾へ）
static Value chosen_toggled(const Value& v, int i) {
  Value out = mk_list();
  ListObj* c = chosen_of(v);
  bool had = false;
  if (c) {
    for (int j = 0; j < c->v.size(); j++) {
      if (c->v[j].i == i) { had = true; continue; }
      as_list(out)->v.push(mk_int(c->v[j].i));
    }
  }
  if (!had) as_list(out)->v.push(mk_int(i));
  return out;
}

static void hit_multi(const Value& v, int i) {
  Value next = chosen_toggled(v, i);
  val_release(g_hit_list);
  g_hit_list = val_retain(next);
  int slot = w_var(v);
  if (slot >= 0 && g_vm && slot < g_vm->globals.size()) {
    val_release(g_vm->globals[slot]);
    g_vm->globals[slot] = val_retain(next);
    g_edited = true;
  }
  val_release(next);
  g_hit_id = w_id(v);
  g_hit_val = i;
  g_hit_valf = (double)i;
  g_hit_any = true;
  clear_hit_action();
  if (Value* a = w_act(v)) g_hit_action = val_retain(*a);
}

static int64_t clamp_i(int64_t v, int64_t lo, int64_t hi) {
  if (hi < lo) hi = lo;
  return v < lo ? lo : (v > hi ? hi : v);
}

// 差し色の輪と、選ばれているときの真ん中の点。四角のチェックと見分けがつくので、
// 「いくつも選べる」のか「1つだけ」なのかがひと目で分かる
static void place_radio(const Value& v, int x, int y, const Box& b) {
  bool on = w_a(v) != 0;
  bool over = inside(x, y, b.w, b.h);
  if (over) g_cursor_want = SCUR_Hand;
  int bw = box_w(), r = bw / 2;
  int by = y + (b.h - bw) / 2;                   // 丸も字も、部品のまんなかに合わせる
  int ty = text_y_mid(y, b.h, cur_text_px());
  int cx = x + r, cy = by + r;
  uint32_t edge = blend(g_bg, g_accent, on ? 1.0 : (over ? 0.85 : 0.5));
  uint32_t face = on ? g_bg : blend(g_bg, g_accent, over ? 0.25 : 0.12);
  int t = bw / 8;
  if (t < 1) t = 1;
  disc(cx, cy, r, edge);              // 外の輪
  disc(cx, cy, r - t, face);
  if (on) {                           // 選ばれているしるしは、真ん中の点
    int dot = r / 2;
    if (dot < 2) dot = 2;
    disc(cx, cy, dot, edge);
  }
  put_text(x + bw + mark_gap(), ty, w_text(v), cur_text_px(), w_fg(v));
  if (over && g_mpress[0]) hit(v, w_b(v));   // ref の形なら「自分の数」、名札なら 1
}

// 選ぶ（押すと一覧が出て、選んだものになる）。一覧は ui.menu と同じ仕組みで出す
static void place_combo(const Value& v, int x, int y, const Box& b) {
  int n = opt_count(v);
  int idx = (int)w_a(v);
  Str key = widget_key(v, x, y);
  bool over = inside(x, y, b.w, b.h);
  if (over) g_cursor_want = SCUR_Hand;

  int64_t bg = w_field(v, WF_Bg);
  uint32_t face = bg >= 0 ? (uint32_t)bg : blend(g_bg, g_accent, over ? 0.35 : 0.2);
  uint32_t edge = blend(g_bg, g_accent, over ? 1.0 : 0.7);
  for (int i = 0; i < b.h; i++) span(x, y + i, b.w, face);
  span(x + 1, y, b.w - 2, edge);
  span(x + 1, y + b.h - 1, b.w - 2, edge);
  for (int i = 1; i < b.h - 1; i++) { put(x, y + i, edge); put(x + b.w - 1, y + i, edge); }
  if (idx >= 0 && idx < n)
    put_text(x + pad_x(), text_y_mid(y, b.h, cur_text_px()), opt_at(v, idx), cur_text_px(), w_fg(v));
  // 「開く」しるしの三角（▼）
  int aw = arrow_w();
  tri_down(x + b.w - pad_x() - aw, y + (b.h - aw / 2) / 2, aw, w_fg(v));

  if (over && g_mpress[0] && n > 0) {   // 押されたので一覧を出す
    Vec<Str> items;
    for (int i = 0; i < n; i++) items.push(opt_at(v, i));
    menu_open_at(x, y + b.h, items, key);
    g_menu_min_w = b.w;
    g_mpress[0] = false;   // この押しは一覧を出すのに使い切る
  }
  // 一覧で選ばれた
  if (g_menu_pick >= 0 && g_menu_owner.size() > 0 && g_menu_owner == key) hit(v, g_menu_pick);
}

// 一覧から1つ選ぶ。入りきらないときは巻物の帯が出て、
// 押して焦点が来ているあいだは上下の矢印でも選べる
static void place_list(const Value& v, int x, int y, const Box& b) {
  int n = opt_count(v);
  bool multi = is_multi(v);
  int idx = (int)w_a(v);
  Str key = widget_key(v, x, y);
  bool over = inside(x, y, b.w, b.h);
  bool focused = g_focus.size() > 0 && g_focus == key;
  int lp = line_pitch(cur_text_px()), lh = line_h(cur_text_px());
  int tx = x + field_pad_x(), ty = y + field_pad_y();
  int shown = area_rows_shown(b.h - field_pad_y() * 2);
  int row_w = b.w - field_pad_x() * 2 - bar_w();

  // 右の帯（入りきらないときだけ出る）。つまんで動かせる
  int bw = bar_w() - 1;
  if (bw < 2) bw = 2;
  int bx = x + b.w - 1 - bw, by = y + 2, bh = b.h - 4;
  bool has_bar = n > shown;
  bool on_bar = has_bar && g_mx >= bx;
  int th = has_bar ? bh * shown / n : bh;   // つまみの長さ
  if (th < 4) th = 4;
  if (th > bh) th = bh;

  int max_px = has_bar ? (n - shown) * lp : 0;   // いちばん下まで送ったときの隠れぶん

  bool mine = g_list_id.size() > 0 && g_list_id == key;
  bool first = !mine;   // 初めて出す回。ここは寄せずに、その場で合わせる
  if (has_bar && g_mpress[0] && inside(bx, by, bw + 1, bh)) {
    g_list_drag = true;                      // 帯をつかんだ
    g_focus_next = key;
    g_list_id = key;
    mine = true;
  }
  if (!g_mb[0]) g_list_drag = false;
  bool dragging = g_list_drag && mine;
  // 車輪（ホイール）で送る。押さなくても、乗せているだけで送れる
  int wheel = 0;
  if (has_bar && over && g_wheel_y != 0) {
    wheel = take_wheel_px(lp);               // この一覧が使い切る
    g_list_id = key;
    mine = true;
    first = false;                           // 送ったのだから、寄せて見せる
  }

  // 見せるところを先に決める。押されたところを数えるのに要る。
  // 覚えているのは最後に触った一覧のぶんだけ（触っていなければ、選から出し直す）
  int to = mine ? g_list_to : 0, cur = mine ? g_list_px : 0;
  if (dragging) {
    // つまみの真ん中が、カーソルのところに来るように。
    // つまんでいる間は指に付いてくるべきなので、寄せずにその場で合わせる
    int room = bh - th;
    to = room > 0 ? (g_my - by - th / 2) * max_px / room : 0;
    cur = to;
  } else if (wheel != 0) {
    to += wheel;
  } else if (idx >= 0 && idx < n && (!mine || g_list_sel != idx)) {
    // 選ばれているものが**変わったときだけ**、見えるところまで動かす。
    // 毎回動かすと、帯でずらしたそばから選のところへ戻ってしまう
    if (idx * lp < to) to = idx * lp;
    if (idx * lp > to + (shown - 1) * lp) to = (idx - shown + 1) * lp;
  }
  if (to < 0) to = 0;
  if (to > max_px) to = max_px;
  // 目当てへ、少しずつ寄せる（途中も出る）。ただし初めて出す回は、その場で合わせる。
  // 画面の無いところ（1回しか描かない）でも、正しいところが出るように
  cur = first ? to : ease_to(cur, to);
  if (cur < 0) cur = 0;
  if (cur > max_px) cur = max_px;
  int top = lp > 0 ? cur / lp : 0;   // いちばん上に見えている行
  int off = cur - top * lp;          // その行が、どれだけ上へずれているか

  // 押された・矢印で動かされた（帯の上を押したときは、行は選ばない）
  if (over && g_mpress[0] && !on_bar) {
    g_focus_next = key;
    int pick = g_my < ty ? top : (g_my - ty + cur) / (lp > 0 ? lp : 1);
    if (pick >= 0 && pick < n) {
      if (multi) hit_multi(v, pick);        // いくつも選べる形は、押すたびに入り切り
      else if (pick != idx) hit(v, pick);
    }
  }
  if (focused && n > 0 && !multi && (g_press[SKEY_Up] || g_press[SKEY_Down])) {
    int next = idx + (g_press[SKEY_Down] ? 1 : -1);
    if (next < 0) next = 0;
    if (next >= n) next = n - 1;
    if (next != idx) hit(v, next);
    idx = next;
    if (idx * lp < to) to = idx * lp;                  // 選んだものが見えるまで動かす
    if (idx * lp > to + (shown - 1) * lp) to = (idx - shown + 1) * lp;
    if (to < 0) to = 0;
    if (to > max_px) to = max_px;
    g_press[SKEY_Up] = false;                          // この一覧が使い切る
    g_press[SKEY_Down] = false;
  }
  g_list_id = key;
  g_list_px = cur;
  g_list_to = to;
  g_list_sel = idx;

  // 描く
  uint32_t edge = blend(g_bg, focused ? g_accent : g_fg, focused ? 1.0 : 0.45);
  int64_t bg = w_field(v, WF_Bg);
  for (int i = 0; i < b.h; i++)
    span(x, y + i, b.w, bg >= 0 ? (uint32_t)bg : blend(g_bg, g_fg, 0.08));
  span(x, y, b.w, edge);
  span(x, y + b.h - 1, b.w, edge);
  for (int i = 0; i < b.h; i++) { put(x, y + i, edge); put(x + b.w - 1, y + i, edge); }

  int kx0 = g_cx0, ky0 = g_cy0, kx1 = g_cx1, ky1 = g_cy1;
  if (x + 1 > g_cx0) g_cx0 = x + 1;
  if (y + 1 > g_cy0) g_cy0 = y + 1;
  if (x + b.w - 2 < g_cx1) g_cx1 = x + b.w - 2;
  if (y + b.h - 2 < g_cy1) g_cy1 = y + b.h - 2;
  // 上と下は半端に切れて出る（送っている途中が見える）
  for (int r = top; r < n && r - top <= shown; r++) {
    int ry = ty + (r - top) * lp - off;
    bool hot = inside(x + 1, ry - 1, b.w - 2, lp);
    bool on = multi ? is_chosen(v, r) : r == idx;
    if (on)
      for (int k = ry - 1; k <= ry + lh; k++)
        span(tx - 2, k, row_w + 4, blend(g_bg, g_accent, 0.5));
    else if (hot)
      for (int k = ry - 1; k <= ry + lh; k++)
        span(tx - 2, k, row_w + 4, blend(g_bg, g_accent, 0.2));
    // いくつも選べる形は、選んだものにレ点を付ける（1つだけの形と見分けがつく）
    if (multi) {
      int mw = lh;
      if (on) check_mark(tx, ry, mw, w_fg(v));
      put_text(tx + mw + mark_gap(), ry, opt_at(v, r), cur_text_px(), w_fg(v));
    } else {
      put_text(tx, ry, opt_at(v, r), cur_text_px(), w_fg(v));
    }
  }
  if (over) g_cursor_want = SCUR_Hand;
  if (has_bar) {   // 入りきらないときの帯。つまんで動かせる
    for (int i = 0; i < bh; i++) span(bx, by + i, bw, blend(g_bg, g_fg, 0.18));
    int at = max_px > 0 ? (bh - th) * cur / max_px : 0;
    double lit = dragging ? 1.0 : (on_bar ? 0.85 : 0.7);
    for (int i = 0; i < th; i++) span(bx, by + at + i, bw, blend(g_bg, g_accent, lit));
  }
  g_cx0 = kx0;
  g_cy0 = ky0;
  g_cx1 = kx1;
  g_cy1 = ky1;
}

// タブ。**出すのは見出しだけ**で、中身は書く人が選んで返す
//   ui.col([ui.tabs("tab", ["あ", "い"], tab), page()])
static void place_tabs(const Value& v, int x, int y, const Box& b) {
  int n = opt_count(v);
  int idx = (int)w_a(v);
  int base = y + b.h - 2;   // 見出しの下に通す線
  span(x, base, b.w, blend(g_bg, g_fg, 0.3));
  int xx = x;
  for (int i = 0; i < n; i++) {
    int tw = text_px_width(opt_at(v, i), cur_text_px()) + pad_x() * 2;
    bool over = inside(xx, y, tw, b.h - 2);
    bool sel = i == idx;
    if (over) g_cursor_want = SCUR_Hand;
    if (sel || over) {
      uint32_t face = blend(g_bg, g_accent, sel ? 0.3 : 0.15);
      for (int k = 0; k < b.h - 2; k++) span(xx, y + k, tw, face);
    }
    put_text(xx + pad_x(), text_y_mid(y, b.h - 2, cur_text_px()), opt_at(v, i), cur_text_px(), w_fg(v));
    if (sel) {   // 選ばれているタブの下だけ、差し色で太く引く
      span(xx, base, tw, g_accent);
      span(xx, base + 1, tw, g_accent);
    }
    if (over && g_mpress[0] && !sel) hit(v, i);
    xx += tw;
  }
}

// 数の入力欄。**上と下の限りから外に出られない**。
// 打てるのは数字（と、下が負なら先頭の -）だけで、それ以外の字は入らない。
// 打っている途中の字だけは、値にできない形（空や "-"）もあるので、
// 焦点のあるあいだだけこちらで覚えておく（巻物の位置と同じ、見た目のための覚え）
static bool num_parse(const Str& s, int64_t* out) {
  if (s.size() == 0) return false;
  int i = 0;
  bool neg = false;
  if (s[0] == '-') { neg = true; i = 1; }
  if (i >= s.size()) return false;
  int64_t n = 0;
  for (; i < s.size(); i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    if (n > 92233720368547758LL) return false;   // これ以上は int に入らない
    n = n * 10 + (s[i] - '0');
  }
  *out = neg ? -n : n;
  return true;
}

// 小数も読む。「-」だけ・「1.」など打ちかけの形は false（打つ途中として通す側で見る）
static bool num_parse_f(const Str& s, double* out) {
  if (s.size() == 0) return false;
  int i = 0;
  bool neg = false;
  if (s[0] == '-') { neg = true; i = 1; }
  if (i >= s.size()) return false;
  double n = 0;
  bool any = false;
  for (; i < s.size(); i++) {
    if (s[i] == '.') break;
    if (s[i] < '0' || s[i] > '9') return false;
    n = n * 10 + (s[i] - '0');
    any = true;
  }
  if (i < s.size() && s[i] == '.') {
    i++;
    double sc = 0.1;
    for (; i < s.size(); i++) {
      if (s[i] < '0' || s[i] > '9') return false;
      n += (s[i] - '0') * sc;
      sc /= 10;
      any = true;
    }
  }
  if (!any) return false;
  *out = neg ? -n : n;
  return true;
}

// 小数を桁を決めて書く。環境差を出さないよう、丸めるところまで自前でやる
// （lib/format.cpp の fixed と同じ考え方）
static Str num_text_f(double v, int dec) {
  if (v != v) return Str("0");
  bool neg = v < 0;
  if (neg) v = -v;
  if (dec < 0) dec = 0;
  if (dec > 9) dec = 9;
  double scale = 1;
  for (int i = 0; i < dec; i++) scale *= 10;
  double scaled = v * scale;
  if (scaled >= 9.0e18) return str_from_float(neg ? -v : v);
  int64_t ip = (int64_t)(scaled + 0.5);
  Str digits = str_from_int(ip);
  Str out;
  if (dec == 0) {
    out = digits;
  } else {
    while (digits.size() <= dec) digits = Str("0") + digits;
    out = digits.sub(0, digits.size() - dec) + "." + digits.sub(digits.size() - dec, dec);
  }
  if (neg && !(ip == 0)) out = Str("-") + out;
  return out;
}

// 出す小数の桁。書いてなければ（-1）、限りの広さから決める
static int num_dec(const Value& v, double lo, double hi) {
  int d = (int)w_field(v, WF_Dec);
  if (d >= 0) return d > 9 ? 9 : d;
  double w = hi - lo;
  if (w <= 2.0) return 3;
  if (w <= 20.0) return 2;
  return 1;
}

static double clamp_d(double v, double lo, double hi) {
  if (hi < lo) hi = lo;
  return v < lo ? lo : (v > hi ? hi : v);
}

static void place_number(const Value& v, int x, int y, const Box& b) {
  int64_t val = w_a(v), lo = w_b(v), hi = w_c(v);
  Str key = widget_key(v, x, y);
  bool focused = g_focus.size() > 0 && g_focus == key;
  int sw = step_w();
  int fw = b.w - sw * 2;   // 字を出すところの幅
  if (fw < 1) fw = 1;
  int ty = text_y_mid(y, b.h, cur_text_px());

  // --- 押された ---
  bool over_box = inside(x, y, fw, b.h);
  int mx0 = x + fw, px0 = x + fw + sw;
  bool over_minus = inside(mx0, y, sw, b.h), over_plus = inside(px0, y, sw, b.h);
  if (over_minus || over_plus) g_cursor_want = SCUR_Hand;
  else if (over_box) g_cursor_want = SCUR_Text;
  if (over_box && g_mpress[0]) {
    g_focus_next = key;
    if (!focused) { g_num_id = key; g_num_text = str_from_int(val); }
  }
  if (g_mpress[0] && (over_minus || over_plus)) {
    int64_t nv = clamp_i(val + (over_plus ? 1 : -1), lo, hi);
    if (nv != val) hit(v, nv);
    if (g_num_id == key) g_num_text = str_from_int(nv);   // 打ちかけの字も合わせる
  }

  // --- 打たれた ---
  if (focused && !g_replay) {
    if (!(g_num_id == key)) { g_num_id = key; g_num_text = str_from_int(val); }
    Str next = g_num_text;
    if (g_press[SKEY_Back] && next.size() > 0) next = next.sub(0, next.size() - 1);
    for (int i = 0; i < g_typed.size(); i++) {
      char c = g_typed[i];
      bool digit = c >= '0' && c <= '9';
      bool minus = c == '-' && next.size() == 0 && lo < 0;
      if (!digit && !minus) continue;   // 数にならない字は入れない
      Str t = next;
      t.push(c);
      int64_t nv = 0;
      // 限りから出る数は打てない。「-」だけ、空だけ、は打っている途中として通す
      if (num_parse(t, &nv) && (nv < lo || nv > hi)) continue;
      next = t;
    }
    if (g_press[SKEY_Up] || g_press[SKEY_Down]) {
      int64_t nv = clamp_i(val + (g_press[SKEY_Up] ? 1 : -1), lo, hi);
      next = str_from_int(nv);
    }
    if (g_press[SKEY_Enter]) g_focus_next.clear();   // 打ち終わり
    if (g_press[SKEY_Escape]) { g_focus_next.clear(); next = str_from_int(val); }
    g_num_text = next;
    int64_t nv = 0;
    if (num_parse(next, &nv)) {
      nv = clamp_i(nv, lo, hi);
      if (nv != val) hit(v, nv);
    }
    // 打たれた字は、この欄が使い切る
    g_typed.clear();
    g_press[SKEY_Back] = false;
    g_press[SKEY_Up] = false;
    g_press[SKEY_Down] = false;
    g_press[SKEY_Enter] = false;
  } else if (g_num_id.size() > 0 && g_num_id == key) {
    g_num_id.clear();   // 離れたので、打ちかけの字は捨てる
    g_num_text.clear();
  }

  Str shown = focused && g_num_id == key ? g_num_text : str_from_int(val);

  // --- 描く ---
  uint32_t edge = blend(g_bg, focused ? g_accent : g_fg, focused ? 1.0 : 0.45);
  int64_t bg = w_field(v, WF_Bg);
  for (int i = 0; i < b.h; i++)
    span(x, y + i, fw, bg >= 0 ? (uint32_t)bg : blend(g_bg, g_fg, 0.08));
  span(x, y, fw, edge);
  span(x, y + b.h - 1, fw, edge);
  for (int i = 0; i < b.h; i++) put(x, y + i, edge);
  // 数は右に寄せる（桁が揃う）。枠からはみ出さないように切り抜く
  int tw = text_px_width(shown, cur_text_px());
  int sx = x + fw - field_pad_x() - tw;
  if (sx < x + field_pad_x()) sx = x + field_pad_x();
  int kx0 = g_cx0, ky0 = g_cy0, kx1 = g_cx1, ky1 = g_cy1;
  if (x + 1 > g_cx0) g_cx0 = x + 1;
  if (y + 1 > g_cy0) g_cy0 = y + 1;
  if (x + fw - 2 < g_cx1) g_cx1 = x + fw - 2;
  if (y + b.h - 2 < g_cy1) g_cy1 = y + b.h - 2;
  const Str& hint = w_str(v, WF_Hint);
  if (shown.size() == 0 && hint.size() > 0)
    put_text(x + field_pad_x(), ty, hint, cur_text_px(), blend(g_bg, w_fg(v), 0.45));
  else
    put_text(sx, ty, shown, cur_text_px(), w_fg(v));
  if (focused) {
    int cw = caret_w();
    for (int i = ty - 1; i <= ty + line_h(cur_text_px()); i++) span(sx + tw, i, cw, g_accent);
  }
  g_cx0 = kx0;
  g_cy0 = ky0;
  g_cx1 = kx1;
  g_cy1 = ky1;
  // − と ＋ のボタン
  for (int k = 0; k < 2; k++) {
    int bx = k == 0 ? mx0 : px0;
    bool hot = k == 0 ? over_minus : over_plus;
    bool able = k == 0 ? val > lo : val < hi;
    uint32_t face = blend(g_bg, g_accent, hot && able ? 0.55 : 0.3);
    for (int i = 0; i < b.h; i++) span(bx, y + i, sw, face);
    uint32_t line = blend(g_bg, g_accent, 0.7);
    span(bx, y, sw, line);
    span(bx, y + b.h - 1, sw, line);
    for (int i = 0; i < b.h; i++) put(bx, y + i, line);
    if (k == 1) for (int i = 0; i < b.h; i++) put(bx + sw - 1, y + i, line);
    uint32_t mark = able ? w_fg(v) : blend(g_bg, w_fg(v), 0.4);
    int mw = sw / 2, cx = bx + sw / 2, cy = y + b.h / 2;
    span(cx - mw / 2, cy, mw, mark);
    if (k == 1) for (int i = 0; i < mw; i++) put(cx, cy - mw / 2 + i, mark);
  }
}

// 引いて変える欄（ui.drag）。**押したまま横へ引くと数が変わり、
// 動かさずに離すと打ち込みに入る**。狭いところに数をいくつも並べるのに向く。
// 打ち込みは数の入力欄（place_number）と同じ覚え（g_num_id / g_num_text）を使う
static void place_drag(const Value& v, int x, int y, const Box& b) {
  bool isf = (w_field(v, WF_Flags) & WFL_Float) != 0;
  double lo = isf ? w_fieldf(v, WF_Fb) : (double)w_b(v);
  double hi = isf ? w_fieldf(v, WF_Fc) : (double)w_c(v);
  double val = isf ? w_fieldf(v, WF_Fa) : (double)w_a(v);
  if (hi < lo) hi = lo;
  val = clamp_d(val, lo, hi);
  int dec = isf ? num_dec(v, lo, hi) : 0;
  Str key = widget_key(v, x, y);
  bool focused = g_focus.size() > 0 && g_focus == key;
  bool over = inside(x, y, b.w, b.h);
  int ty = text_y_mid(y, b.h, cur_text_px());

  // --- つかんで引く ---
  bool mine = g_dnum_id.size() > 0 && g_dnum_id == key;
  if (over && g_mpress[0] && !g_replay) {
    g_dnum_id = key;
    g_dnum_x = g_mx;
    g_dnum_moved = false;
    g_dnum_base = val;
    mine = true;
  }
  if (mine && g_mb[0] && !g_replay) {
    int dx = g_mx - g_dnum_x;
    if (dx > 2 || dx < -2) g_dnum_moved = true;
    if (g_dnum_moved) {
      // 端から端までを 200 画素ぶんで。限りが無いに等しいときは 1 画素 1
      double per = hi > lo ? (hi - lo) / 200.0 : 1.0;
      double nv = g_dnum_base + dx * per;
      if (!isf) nv = (double)(int64_t)(nv < 0 ? nv - 0.5 : nv + 0.5);
      nv = clamp_d(nv, lo, hi);
      if (nv != val) {
        if (isf) hit_f(v, nv);
        else hit(v, (int64_t)nv);
        val = nv;
      }
      g_focus_next.clear();   // 引いている間は、打ち込みに入らない
    }
  }
  if (mine && !g_mb[0]) {
    // 動かさずに離したら、打ち込みに入る
    if (!g_dnum_moved && over) {
      g_focus_next = key;
      if (!focused) {
        g_num_id = key;
        g_num_text = isf ? num_text_f(val, dec) : str_from_int((int64_t)val);
      }
    }
    g_dnum_id.clear();
    g_dnum_moved = false;
  }
  if (over || mine) g_cursor_want = (focused && !mine) ? SCUR_Text : SCUR_Hand;

  // --- 打たれた ---
  if (focused && !g_replay) {
    if (!(g_num_id == key)) {
      g_num_id = key;
      g_num_text = isf ? num_text_f(val, dec) : str_from_int((int64_t)val);
    }
    Str next = g_num_text;
    if (g_press[SKEY_Back] && next.size() > 0) next = next.sub(0, next.size() - 1);
    for (int i = 0; i < g_typed.size(); i++) {
      char c = g_typed[i];
      bool digit = c >= '0' && c <= '9';
      bool minus = c == '-' && next.size() == 0 && lo < 0;
      bool dot = false;
      if (c == '.' && isf) {
        dot = true;
        for (int j = 0; j < next.size(); j++) if (next[j] == '.') dot = false;
      }
      if (!digit && !minus && !dot) continue;   // 数にならない字は入れない
      Str t = next;
      t.push(c);
      double nv = 0;
      if (num_parse_f(t, &nv) && (nv < lo || nv > hi)) continue;
      next = t;
    }
    if (g_press[SKEY_Enter]) g_focus_next.clear();
    if (g_press[SKEY_Escape]) {
      g_focus_next.clear();
      next = isf ? num_text_f(val, dec) : str_from_int((int64_t)val);
    }
    g_num_text = next;
    double nv = 0;
    if (num_parse_f(next, &nv)) {
      nv = clamp_d(nv, lo, hi);
      if (nv != val) {
        if (isf) hit_f(v, nv);
        else hit(v, (int64_t)nv);
      }
    }
    g_typed.clear();
    g_press[SKEY_Back] = false;
    g_press[SKEY_Enter] = false;
  } else if (g_num_id.size() > 0 && g_num_id == key) {
    g_num_id.clear();
    g_num_text.clear();
  }

  Str shown = (focused && g_num_id == key) ? g_num_text
                                           : (isf ? num_text_f(val, dec) : str_from_int((int64_t)val));

  // --- 描く ---
  int64_t bg = w_field(v, WF_Bg);
  uint32_t face = bg >= 0 ? (uint32_t)bg : blend(g_bg, g_fg, over || focused ? 0.16 : 0.08);
  uint32_t edge = blend(g_bg, focused ? g_accent : g_fg, focused ? 1.0 : (over ? 0.6 : 0.35));
  int rad = (int)w_field(v, WF_Radius);
  if (rad > 0) fill_round(x, y, b.w, b.h, rad, face);
  else for (int i = 0; i < b.h; i++) span(x, y + i, b.w, face);
  if (w_field(v, WF_Border) < 0) stroke_round(x, y, b.w, b.h, rad > 0 ? rad : 0, 1, edge);

  // 限りのどのあたりかを、下地の帯で見せる（引く部品だと分かる）
  if (hi > lo && !focused) {
    int fillw = (int)((val - lo) * (b.w - 2) / (hi - lo));
    for (int i = 1; i < b.h - 1; i++) span(x + 1, y + i, fillw, blend(face, g_accent, 0.22));
  }
  int tw = text_px_width(shown, cur_text_px());
  int sx = x + (b.w - tw) / 2;
  int kx0 = g_cx0, ky0 = g_cy0, kx1 = g_cx1, ky1 = g_cy1;
  if (x + 1 > g_cx0) g_cx0 = x + 1;
  if (x + b.w - 2 < g_cx1) g_cx1 = x + b.w - 2;
  put_text(sx, ty, shown, cur_text_px(), w_fg(v));
  if (focused) {
    int cw = caret_w();
    for (int i = ty - 1; i <= ty + line_h(cur_text_px()); i++) span(sx + tw, i, cw, g_accent);
  } else if (over) {
    // 引けるしるし。左右に小さな三角
    uint32_t m = blend(g_bg, g_fg, 0.5);
    int aw = arrow_w() / 2;
    if (aw < 2) aw = 2;
    for (int i = 0; i < aw; i++) {
      span(x + 2 + i, y + b.h / 2 - i, 1, m);
      span(x + 2 + i, y + b.h / 2 + i, 1, m);
      span(x + b.w - 3 - i, y + b.h / 2 - i, 1, m);
      span(x + b.w - 3 - i, y + b.h / 2 + i, 1, m);
    }
  }
  g_cx0 = kx0;
  g_cy0 = ky0;
  g_cx1 = kx1;
  g_cy1 = ky1;
}

// 絵を出す部品（ui.image）。大きさを決めていなければ、絵そのものの大きさで出す。
// 名札を渡してあれば押されたことも返り、カーソルが乗っている間は
// **絵の中のどこか**（ui.point_x / ui.point_y）が分かる
static void place_image(const Value& v, int x, int y, const Box& b) {
  int sw = (int)w_a(v), sh = (int)w_b(v);
  const Str& px = w_bytes(v, WF_Px);
  if (sw <= 0 || sh <= 0 || px.size() < (int)((size_t)sw * (size_t)sh * sizeof(uint32_t))) return;
  blit_scaled((const uint32_t*)px.data(), sw, sh, x, y, b.w, b.h);
  if (!inside(x, y, b.w, b.h)) return;
  // 絵の中の位置（伸ばしてあれば、そのぶん戻して数える）
  g_point_x = b.w > 0 ? (g_mx - x) * sw / b.w : 0;
  g_point_y = b.h > 0 ? (g_my - y) * sh / b.h : 0;
  if (g_point_x >= sw) g_point_x = sw - 1;
  if (g_point_y >= sh) g_point_y = sh - 1;
  if (w_id(v).size() > 0 || w_act(v)) {
    g_cursor_want = SCUR_Hand;
    if (g_mpress[0]) hit(v, 1);
  }
}

// --- カーソルを合わせたときに出す説明（.tooltip）--------------------------
// 部品を置くたびに「いまカーソルが乗っているもの」を控えておき、
// ぜんぶ置き終わってから、いちばん上に描く（menu と同じ扱い）
static void tip_draw() {
  if (g_tip_text.size() == 0 || g_menu_on) return;
  // 合わせてすぐには出さない。同じものに乗せ続けているあいだだけ数える
  if (g_visible) {
    int64_t now = platform().monotonic_nanos();
    if (!(g_tip_text == g_tip_prev)) { g_tip_prev = g_tip_text; g_tip_since = now; }
    if (now - g_tip_since < 400000000LL) return;   // 0.4 秒
  }
  int max_w = g_w * 2 / 3;
  Str t = text_px_width(g_tip_text, cur_text_px()) > max_w ? wrap_text(g_tip_text, max_w, cur_text_px()) : g_tip_text;
  int tw = text_px_width(t, cur_text_px()), lines = text_lines(t);
  int w = tw + pad_x() * 2;
  int h = line_h(cur_text_px()) + (lines - 1) * line_pitch(cur_text_px()) + pad_y() * 2;
  int x = g_tip_x + ui_unit(), y = g_tip_y + ui_unit();
  if (x + w > g_w) x = g_w - w;
  if (y + h > g_h) y = g_tip_y - h - 2;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  uint32_t face = blend(g_bg, g_fg, 0.15), edge = blend(g_bg, g_accent, 0.8);
  for (int i = 0; i < h; i++) span(x, y + i, w, face);
  span(x, y, w, edge);
  span(x, y + h - 1, w, edge);
  for (int i = 0; i < h; i++) { put(x, y + i, edge); put(x + w - 1, y + i, edge); }
  put_text(x + pad_x(), y + pad_y(), t, cur_text_px(), g_fg);
}

// 横に並べたとき、背の低い部品を真ん中に置く（つまみと文字が並んだときに揃う）
// 親がくれた高さ（ch）の中で、高さ h の子をどこに置くか。
// 子が .valign を書いていればそれに従い、書いていなければ def になる
static int yy_place(const Value& child, int cy, int ch, int h, int def) {
  int va = (int)w_field(child, WF_VAlign);
  if (va < 0) va = def;
  if (ch <= h) return cy;
  if (va == WV_Middle) return cy + (ch - h) / 2;
  if (va == WV_Bottom) return cy + ch - h;
  return cy;
}

// --- 色を選ぶ（ui.color）--------------------------------------------------
// 見本を押すと、色を選ぶ板が下に出る。板は部品より**上に描き**、出ているあいだの
// 押しは板が先に受け取る（メニューと同じ扱い）。
// 色そのものを持つのは書く人で、板が覚えるのは「いま開いているのはどれか」だけ
static int color_sq() { return ui_unit() * 12; }        // 濃さ・明るさの四角
static int color_bar() { return ui_unit() * 3 / 2; }    // 色みの帯の幅
static int color_gap() { return ui_unit() / 2; }
static int color_w() { return color_sq() + color_gap() + color_bar() + pad_x() * 2; }
static int color_h() { return color_sq() + pad_y() * 2; }

// 色み（0〜360）・濃さ・明るさ（0〜1）から色を作る
static uint32_t hsv_rgb(double h, double sat, double val) {
  while (h < 0) h += 360;
  while (h >= 360) h -= 360;
  if (sat < 0) sat = 0;
  if (sat > 1) sat = 1;
  if (val < 0) val = 0;
  if (val > 1) val = 1;
  double c = val * sat;
  double hh = h / 60.0;
  double t = hh - (double)(int)(hh / 2) * 2 - 1;
  if (t < 0) t = -t;
  double xx = c * (1 - t);
  double m = val - c;
  double r = 0, g = 0, b = 0;
  int i = (int)hh;
  if (i == 0) { r = c; g = xx; }
  else if (i == 1) { r = xx; g = c; }
  else if (i == 2) { g = c; b = xx; }
  else if (i == 3) { g = xx; b = c; }
  else if (i == 4) { r = xx; b = c; }
  else { r = c; b = xx; }
  int R = (int)((r + m) * 255 + 0.5), G = (int)((g + m) * 255 + 0.5), B = (int)((b + m) * 255 + 0.5);
  if (R > 255) R = 255;
  if (G > 255) G = 255;
  if (B > 255) B = 255;
  return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

static void rgb_hsv(uint32_t col, double* h, double* sat, double* val) {
  double r = (double)((col >> 16) & 0xff) / 255.0;
  double g = (double)((col >> 8) & 0xff) / 255.0;
  double b = (double)(col & 0xff) / 255.0;
  double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  double d = mx - mn;
  double hh = 0;
  if (d > 0) {
    if (mx == r) hh = 60 * ((g - b) / d);
    else if (mx == g) hh = 60 * (2 + (b - r) / d);
    else hh = 60 * (4 + (r - g) / d);
    if (hh < 0) hh += 360;
  }
  *h = hh;
  *sat = mx > 0 ? d / mx : 0;
  *val = mx;
}

// 色を 16 進で。#RRGGBB の形（見本のとなりに出す）
static Str color_hex(uint32_t c) {
  Str out("#");
  Str body = str_from_uint_base((uint64_t)(c & 0xffffffu), 16, true);
  for (int i = body.size(); i < 6; i++) out.push('0');
  out += body;
  return out;
}

// 板が出ているあいだ、押し・引きを先に受け取る（ui.show の初めに呼ぶ）
static void color_hit() {
  g_color_moved = false;
  if (g_color_id.size() == 0) return;
  int x = g_color_x, y = g_color_y, w = color_w(), h = color_h();
  int sq = color_sq();
  int sx = x + pad_x(), sy = y + pad_y();
  int bx = sx + sq + color_gap(), bw = color_bar();
  bool in_panel = inside(x, y, w, h);
  if (g_mpress[0]) {
    if (!in_panel) { g_color_id.clear(); g_color_drag = 0; return; }   // 外を押したら閉じる
    if (inside(sx, sy, sq, sq)) g_color_drag = 1;
    else if (inside(bx, sy, bw, sq)) g_color_drag = 2;
    g_mpress[0] = false;      // 板の上の押しは、板が使い切る
  }
  if (!g_mb[0]) g_color_drag = 0;
  if (in_panel || g_color_drag) g_cursor_want = SCUR_Hand;
  // つかんでいるあいだは、外へ出ても付いてくる（つまみと同じ）
  if (g_color_drag == 1 && sq > 1) {
    double ss = (double)(g_mx - sx) / (double)(sq - 1);
    double vv = 1.0 - (double)(g_my - sy) / (double)(sq - 1);
    g_color_s = ss < 0 ? 0 : (ss > 1 ? 1 : ss);
    g_color_v = vv < 0 ? 0 : (vv > 1 ? 1 : vv);
    g_color_moved = true;
  } else if (g_color_drag == 2 && sq > 1) {
    double hh = (double)(g_my - sy) / (double)(sq - 1);
    if (hh < 0) hh = 0;
    if (hh > 1) hh = 1;
    g_color_h = hh * 359.9;
    g_color_moved = true;
  }
  if (g_press[SKEY_Escape]) { g_color_id.clear(); g_color_drag = 0; }
}

// 板を描く（ui.show の終わり、メニューの手前）
static void color_draw() {
  if (g_color_id.size() == 0) return;
  int x = g_color_x, y = g_color_y, w = color_w(), h = color_h();
  int sq = color_sq();
  int sx = x + pad_x(), sy = y + pad_y();
  int bx = sx + sq + color_gap(), bw = color_bar();
  fill_round(x, y, w, h, ui_unit() / 2, blend(g_bg, g_fg, 0.12));
  stroke_round(x, y, w, h, ui_unit() / 2, 1, blend(g_bg, g_fg, 0.45));
  // 濃さ（横）と明るさ（縦）の四角
  for (int j = 0; j < sq; j++) {
    double vv = 1.0 - (double)j / (double)(sq - 1);
    for (int i = 0; i < sq; i++) {
      double ss = (double)i / (double)(sq - 1);
      put(sx + i, sy + j, hsv_rgb(g_color_h, ss, vv));
    }
  }
  // 色みの帯
  for (int j = 0; j < sq; j++) {
    uint32_t c = hsv_rgb((double)j / (double)(sq - 1) * 359.9, 1.0, 1.0);
    span(bx, sy + j, bw, c);
  }
  // いまの場所のしるし。明るいところでは黒、暗いところでは白で描く
  int px = sx + (int)(g_color_s * (sq - 1) + 0.5);
  int py = sy + (int)((1.0 - g_color_v) * (sq - 1) + 0.5);
  uint32_t ring = g_color_v > 0.6 && g_color_s < 0.6 ? 0x000000u : 0xffffffu;
  for (int i = -3; i <= 3; i++) {
    if (i > -2 && i < 2) continue;
    put(px + i, py, ring);
    put(px, py + i, ring);
  }
  int hy = sy + (int)(g_color_h / 359.9 * (sq - 1) + 0.5);
  span(bx - 2, hy, bw + 4, 0xffffffu);
}

// 折りたためる木（ui.tree）。見出しを押すと開け閉めし、開いていれば中身を
// 1段下げて縦に並べる。開いているかどうかは**書く人が持つ**（ほかの部品と同じ）
static void place_tree(const Value& v, int x, int y, const Box& b) {
  bool open = w_a(v) != 0;
  int ind = box_w() + mark_gap();
  int head_h = mark_row_h();
  bool over = inside(x, y, b.w, head_h);
  if (over) g_cursor_want = SCUR_Hand;
  // 開け閉めのしるし。開いていれば下、閉じていれば右を向く
  int aw = arrow_w();
  uint32_t mark = blend(g_bg, w_fg(v), over ? 1.0 : 0.75);
  if (open) tri_down(x + (box_w() - aw) / 2, y + (head_h - aw / 2) / 2, aw, mark);
  else tri_right(x + (box_w() - aw / 2) / 2, y + (head_h - aw) / 2, aw, mark);
  put_text(x + ind, text_y_mid(y, head_h, cur_text_px()), w_text(v), cur_text_px(), w_fg(v));
  if (over && g_mpress[0]) hit(v, open ? 0 : 1);
  if (!open) return;

  RowAxis axis(false);
  ListObj* k = w_kids(v);
  int yy = y + head_h;
  int cw = b.w - ind;
  if (cw < 1) cw = 1;
  for (int i = 0; i < k->v.size(); i++) {
    int hh = measure(k->v[i], cw).h;
    yy += gap_y();
    place(k->v[i], x + ind, yy, cw, hh);
    yy += hh;
  }
}

// 色の入力（ui.color）。見本と 16 進を出し、押すと選ぶ板が下に出る
static void place_color(const Value& v, int x, int y, const Box& b) {
  uint32_t c = (uint32_t)w_a(v);
  Str key = widget_key(v, x, y);
  bool open = g_color_id.size() > 0 && g_color_id == key;
  bool over = inside(x, y, b.w, b.h);
  if (over) g_cursor_want = SCUR_Hand;

  // 板で動かされていれば、その色にする（1こま遅れない）
  if (open && g_color_moved) {
    uint32_t nv = hsv_rgb(g_color_h, g_color_s, g_color_v) | (g_color_keep & 0xff000000u);
    if (nv != c) hit(v, (int64_t)nv);
    c = nv;
  }

  int rad = (int)w_field(v, WF_Radius);
  if (rad < 0) rad = ui_unit() / 4;
  uint32_t edge = blend(g_bg, open ? g_accent : g_fg, open ? 1.0 : (over ? 0.7 : 0.4));
  int sw = b.h;                        // 見本は正方形
  fill_round(x, y, b.w, b.h, rad, blend(g_bg, g_fg, 0.08));
  fill_round(x + 1, y + 1, sw - 2, b.h - 2, rad, c);
  stroke_round(x, y, b.w, b.h, rad, 1, edge);
  put_text(x + sw + field_pad_x(), text_y_mid(y, b.h, cur_text_px()), color_hex(c), cur_text_px(), w_fg(v));

  if (over && g_mpress[0]) {
    if (open) {
      g_color_id.clear();
    } else {
      g_color_id = key;
      g_color_x = x;
      g_color_y = y + b.h + 2;
      if (g_color_x + color_w() > g_w) g_color_x = g_w - color_w();
      if (g_color_y + color_h() > g_h) g_color_y = y - color_h() - 2;
      if (g_color_x < 0) g_color_x = 0;
      if (g_color_y < 0) g_color_y = 0;
      rgb_hsv(c, &g_color_h, &g_color_s, &g_color_v);
      g_color_keep = c;
      g_color_drag = 0;
    }
    g_mpress[0] = false;   // この押しは見本が使い切る
  }
}

// 巻物（ui.scroll）。中身は縦に並び、はみ出したぶんは右の帯で送る。
// 覚えているのは「いま隠しているぶん」だけで、中身は毎回作り直されたままでよい
static int sc_slot(const Str& id) {
  for (int i = 0; i < g_sc_id.size(); i++) if (g_sc_id[i] == id) return i;
  g_sc_id.push(id);
  g_sc_px.push(0);
  g_sc_to.push(0);
  return g_sc_id.size() - 1;
}

static void place_scroll(const Value& v, int x, int y, const Box& b) {
  RowAxis axis(false);
  ListObj* k = w_kids(v);
  int n = k->v.size();
  Str key = widget_key(v, x, y);
  int lp = line_pitch(cur_text_px());

  // 中身の高さを測る。帯が出ると中身のもらえる幅が狭まるので、そのときは測り直す
  int inner_w = b.w;
  int content = 0;
  for (int pass = 0; pass < 2; pass++) {
    content = 0;
    for (int i = 0; i < n; i++) {
      content += measure(k->v[i], inner_w).h;
      if (i + 1 < n) content += gap_y();
    }
    if (content <= b.h || pass == 1) break;
    inner_w = b.w - bar_w();
  }
  bool has_bar = content > b.h;
  if (!has_bar) inner_w = b.w;
  int max_px = has_bar ? content - b.h : 0;

  int slot = sc_slot(key);
  int to = g_sc_to[slot], cur = g_sc_px[slot];
  bool over = inside(x, y, b.w, b.h);

  // 右の帯。つまんで動かせる
  int bw = bar_w() - 1;
  if (bw < 2) bw = 2;
  int bx = x + b.w - bw, by = y, bh = b.h;
  int th = (has_bar && content > 0) ? bh * b.h / content : bh;
  if (th < 8) th = 8;
  if (th > bh) th = bh;
  bool on_bar = has_bar && over && g_mx >= bx;
  if (has_bar && g_mpress[0] && inside(bx, by, bw, bh)) {
    g_sc_drag = key;
    g_mpress[0] = false;      // この押しは帯が使い切る
  }
  if (!g_mb[0] && g_sc_drag.size() > 0 && g_sc_drag == key) g_sc_drag.clear();
  bool dragging = has_bar && g_sc_drag.size() > 0 && g_sc_drag == key;

  if (dragging) {
    // つまみの真ん中がカーソルに来るように。つまんでいる間は寄せずにその場で合わせる
    int room = bh - th;
    to = room > 0 ? (g_my - by - th / 2) * max_px / room : 0;
    cur = to;
  } else if (has_bar && over && g_wheel_y != 0) {
    to += take_wheel_px(lp);   // この巻物が送りを使い切る
  }
  if (to < 0) to = 0;
  if (to > max_px) to = max_px;
  cur = dragging ? to : ease_to(cur, to);
  if (cur < 0) cur = 0;
  if (cur > max_px) cur = max_px;
  g_sc_px[slot] = cur;
  g_sc_to[slot] = to;

  // 中身を、送ったぶん上にずらして置く。切り抜きの外へは描かれない
  int kx0 = g_cx0, ky0 = g_cy0, kx1 = g_cx1, ky1 = g_cy1;
  if (x > g_cx0) g_cx0 = x;
  if (y > g_cy0) g_cy0 = y;
  if (x + inner_w - 1 < g_cx1) g_cx1 = x + inner_w - 1;
  if (y + b.h - 1 < g_cy1) g_cy1 = y + b.h - 1;
  // 巻いて隠れた部品が押しを受け取らないよう、外にいる間はカーソルを遠くへやる
  int smx = g_mx, smy = g_my;
  if (!over) { g_mx = -1000000; g_my = -1000000; }
  int yy = y - cur;
  for (int i = 0; i < n; i++) {
    int hh = measure(k->v[i], inner_w).h;
    place(k->v[i], x, yy, inner_w, hh);
    yy += hh + gap_y();
  }
  g_mx = smx;
  g_my = smy;
  g_cx0 = kx0;
  g_cy0 = ky0;
  g_cx1 = kx1;
  g_cy1 = ky1;

  if (has_bar) {
    for (int i = 0; i < bh; i++) span(bx, by + i, bw, blend(g_bg, g_fg, 0.18));
    int at = max_px > 0 ? (bh - th) * cur / max_px : 0;
    double lit = dragging ? 1.0 : (on_bar ? 0.85 : 0.7);
    for (int i = 0; i < th; i++) span(bx, by + at + i, bw, blend(g_bg, g_accent, lit));
  }
}

// 親がくれた場所の中で、その部品がどこに、どれだけの大きさで置かれるか。
// 置くとき（place）と、重ね置きの当たり判定とで同じものを見るために分けてある
struct Placed { int x, y, w, h; };

static Placed layout_of(const Value& v, int x, int y, int avail_w, int avail_h, bool root) {
  TextPx tp(v);          // 測るときの字の大きさは、その部品のもの
  Box b = measure(v, avail_w);
  int w = b.w, h = b.h;
  // 取り分（fr）のある向きは、親が配ってくれたぶんいっぱいに広がる
  // （入れ物は、中身の取り分も受け継ぐ。eff_fr）
  if (eff_fr(v, true) > 0 && avail_w > 0) w = avail_w;
  if (eff_fr(v, false) > 0 && avail_h > 0) h = avail_h;
  // 寄せ方は「親がくれた幅の中で、自分をどこに置くか」
  int al = w_align(v);
  // 入れ物（ui.col / ui.row / ui.grid）の大きさは**中身に合わせて伸び縮みする**（上の measure）。
  // ただし、いちばん外（ui.show に渡された部品）だけは、寄せていなければ置ける幅いっぱいに
  // 広がる。こうすると、いちばん外の並びでは、区切り線や寄せ（.align）が面の幅を見られる。
  // 寄せた入れ物（ui.center など）は中身の大きさのまま、そのかたまりを寄せる
  if (root && is_container(v) && w_wid(v) <= 0 && w_fr(v, true) <= 0 && al == WA_Left &&
      avail_w > w)
    w = avail_w;
  // 区切り線は、置ける幅いっぱいに引く
  if (w_kind(v) == WK_Divider && w_wid(v) <= 0) w = avail_w;

  if (avail_w > w) {
    if (al == WA_Center) x += (avail_w - w) / 2;
    else if (al == WA_Right) x += avail_w - w;
  }
  Placed r;
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return r;
}

static void place(const Value& v, int x, int y, int avail_w, int avail_h, bool root) {
  TextPx tp(v);          // .font と .disabled は、ここから下ぜんぶに効く
  DisabledScope dis(v);
  Placed pl = layout_of(v, x, y, avail_w, avail_h, root);
  x = pl.x;
  y = pl.y;
  int w = pl.w, h = pl.h;
  int64_t bg = w_field(v, WF_Bg);
  int bk = w_kind(v);
  int rad = (int)w_field(v, WF_Radius);
  // 自分で下地を塗る部品は、ここでは塗らない（枠の中だけを塗るため）
  bool self_bg = bk == WK_Button || bk == WK_Field || bk == WK_Area || bk == WK_Combo ||
                 bk == WK_List || bk == WK_Number || bk == WK_Color || bk == WK_Drag;
  if (bg >= 0 && !self_bg) {
    if (rad > 0) fill_round(x, y, w, h, rad, (uint32_t)bg);
    else for (int i = 0; i < h; i++) span(x, y + i, w, (uint32_t)bg);
  }

  // カーソルが乗っていて、説明を持っていれば控えておく（描くのは、ぜんぶ置いたあと）。
  // 入れ物より中身のほうが後に置かれるので、細かいほうの説明が残る
  const Str& tip = w_str(v, WF_Tip);
  if (tip.size() > 0 && inside(x, y, w, h)) {
    g_tip_text = tip;
    g_tip_x = g_mx;
    g_tip_y = g_my;
  }

  int p = w_pad(v);
  int cx = x + p, cy = y + p, cw = w - p * 2, ch = h - p * 2;
  Box cb;
  cb.w = cw;
  cb.h = ch;

  switch (w_kind(v)) {
    case WK_Label: {
      // 測ったとき（measure）と同じ幅で折り返して描く
      int lw = (w_wid(v) > 0 ? w_wid(v) : avail_w) - p * 2;
      if (lw > 0 && text_px_width(w_text(v), cur_text_px()) > lw)
        put_text(cx, cy, wrap_text(w_text(v), lw, cur_text_px()), cur_text_px(), w_fg(v));
      else
        put_text(cx, cy, w_text(v), cur_text_px(), w_fg(v));
      break;
    }
    case WK_Button: place_button(v, cx, cy, cb); break;
    case WK_Checkbox: place_checkbox(v, cx, cy, cb); break;
    case WK_Slider: place_slider(v, cx, cy, cb); break;
    case WK_Field: place_field(v, cx, cy, cb); break;
    case WK_Area: place_area(v, cx, cy, cb); break;
    case WK_Radio: place_radio(v, cx, cy, cb); break;
    case WK_Combo: place_combo(v, cx, cy, cb); break;
    case WK_List: place_list(v, cx, cy, cb); break;
    case WK_Tabs: place_tabs(v, cx, cy, cb); break;
    case WK_Number: place_number(v, cx, cy, cb); break;
    case WK_Drag: place_drag(v, cx, cy, cb); break;
    case WK_Image: place_image(v, cx, cy, cb); break;
    case WK_Space: break;
    case WK_Divider: {
      uint32_t c = w_field(v, WF_Fg) >= 0 ? w_fg(v) : blend(g_bg, g_fg, 0.3);
      span(cx, cy + ch / 2, cw, c);
      break;
    }
    case WK_Column: {
      RowAxis axis(false);
      ListObj* k = w_kids(v);
      Vec<int> hs;
      axis_sizes(k, false, ch, gap_y(), hs, cw);   // 縦に並べる。余りは高さの取り分へ
      int yy = cy;
      for (int i = 0; i < k->v.size(); i++) {
        place(k->v[i], cx, yy, cw, hs[i]);
        yy += hs[i] + gap_y();
      }
      break;
    }
    case WK_Row: {
      RowAxis axis(true);
      ListObj* k = w_kids(v);
      Vec<int> ws;
      axis_sizes(k, true, cw, gap_x(), ws);    // 横に並べる。余りは幅の取り分へ
      int xx = cx;
      for (int i = 0; i < k->v.size(); i++) {
        // 高さの取り分のある子は、並びの高さいっぱいに伸びる
        int hh = eff_fr(k->v[i], false) > 0 ? ch : measure(k->v[i], ws[i]).h;
        place(k->v[i], xx, yy_place(k->v[i], cy, ch, hh, WV_Middle), ws[i], hh);
        xx += ws[i] + gap_x();
      }
      break;
    }
    // 重ね置き。中身を同じところに、書いた順で重ねる（あとのものが上）
    case WK_Stack: {
      ListObj* k = w_kids(v);
      int kn = k->v.size();
      // 触られたかは、**カーソルに重なっているうち、いちばん上のもの**が取る。
      // 下に敷いたものには届かない。だからダイアログは、面ぜんぶを覆う幕を
      // 1枚かぶせるだけで、うしろの画面がまとめて止まる（spec/library/ui.md）
      int top = -1;
      for (int i = 0; i < kn; i++) {
        int hh = eff_fr(k->v[i], false) > 0 ? ch : measure(k->v[i], cw).h;
        int yy = yy_place(k->v[i], cy, ch, hh, WV_Top);
        Placed pl = layout_of(k->v[i], cx, yy, cw, hh, false);
        if (inside(pl.x, pl.y, pl.w, pl.h)) top = i;
      }
      for (int i = 0; i < kn; i++) {
        int hh = eff_fr(k->v[i], false) > 0 ? ch : measure(k->v[i], cw).h;
        int smx = g_mx, smy = g_my;
        if (top >= 0 && i != top) { g_mx = -1000000; g_my = -1000000; }
        place(k->v[i], cx, yy_place(k->v[i], cy, ch, hh, WV_Top), cw, hh);
        g_mx = smx;
        g_my = smy;
      }
      break;
    }
    case WK_Scroll: place_scroll(v, cx, cy, cb); break;
    case WK_Tree: place_tree(v, cx, cy, cb); break;
    case WK_Color: place_color(v, cx, cy, cb); break;
    case WK_Grid: {
      ListObj* k = w_kids(v);
      int cols = grid_cols(v);
      Vec<int> gw, gh;
      grid_axes(v, cw, ch, gw, gh);            // 列の幅と行の高さ。余りは取り分へ
      int yy = cy;
      for (int r = 0; r < gh.size(); r++) {
        int xx = cx;
        for (int c = 0; c < cols; c++) {
          int i = r * cols + c;
          if (i >= k->v.size()) break;
          // 横に並べたときと同じく、背の低い升は行の真ん中に置く
          int hh = eff_fr(k->v[i], false) > 0 ? gh[r] : measure(k->v[i], gw[c]).h;
          place(k->v[i], xx, yy_place(k->v[i], yy, gh[r], hh, WV_Middle), gw[c], hh);
          xx += gw[c] + gap_x();
        }
        yy += gh[r] + gap_y();
      }
      break;
    }
    default: break;
  }

  // 縁（.border）は中身のあとに引く。細い線でも中身に隠れない
  int64_t bd = w_field(v, WF_Border);
  if (bd >= 0) {
    int t = (int)w_field(v, WF_BorderW);
    if (t < 1) t = 1;
    stroke_round(x, y, w, h, rad > 0 ? rad : 0, t, (uint32_t)bd);
  }
}

// --- 作る -----------------------------------------------------------------
static NativeStatus u_label(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Label, as_str(*A(a, 0))->s, Str(), 0, 0, 0, 0, out) ? N_Ok : N_Panic;
}
static NativeStatus u_button(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Button, as_str(*A(a, 0))->s, as_str(*A(a, 1))->s, 0, 0, 0, 0, out)
             ? N_Ok : N_Panic;
}
// 絵を出す部品。画素の並びは**そのまま借りる**（写さない）ので、
// 毎こま作り直しても重くならない（絵を書き換えれば、写しはそのときに起きる）
static bool make_image(VM& vm, const Str& id, Value* img, Value& out, Value* action) {
  if (!is_canvas(vm, *img)) {
    vm.panic(vm.L("絵ではありません", "not a canvas"));
    return false;
  }
  InstObj* o = as_inst(*img);
  int64_t w = o->fields[CF_W].i, h = o->fields[CF_H].i;
  if (!make_widget(vm, WK_Image, Str(), id, w, h, 0, 0, out, action)) return false;
  widget_put(as_inst(out), WF_Px, val_retain(o->fields[CF_Px]));
  return true;
}
static NativeStatus u_image(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_image(vm, Str(), A(a, 0), out, 0) ? N_Ok : N_Panic;
}
static NativeStatus u_image_id(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_image(vm, as_str(*A(a, 0))->s, A(a, 1), out, 0) ? N_Ok : N_Panic;
}
static NativeStatus u_image_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_image(vm, Str(), A(a, 1), out, A(a, 0)) ? N_Ok : N_Panic;
}
static NativeStatus u_point_x(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_int(g_point_x);
  return N_Ok;
}
static NativeStatus u_point_y(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_int(g_point_y);
  return N_Ok;
}

// 中身に部品を入れた形。ラベルの代わりに、その部品をボタンの真ん中に置く
static NativeStatus u_button_w(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value kids = mk_list();
  as_list(kids)->v.push(val_retain(*A(a, 0)));
  bool ok = make_widget(vm, WK_Button, Str(), as_str(*A(a, 1))->s, 0, 0, 0, &kids, out);
  val_release(kids);
  return ok ? N_Ok : N_Panic;
}
static NativeStatus u_button_w_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value kids = mk_list();
  as_list(kids)->v.push(val_retain(*A(a, 0)));
  bool ok = make_widget(vm, WK_Button, Str(), Str(), 0, 0, 0, &kids, out, A(a, 1));
  val_release(kids);
  return ok ? N_Ok : N_Panic;
}
// 関数を渡す形。押されたら、その関数が呼ばれる（名札は要らない）
static NativeStatus u_button_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Button, as_str(*A(a, 0))->s, Str(), 0, 0, 0, 0, out, A(a, 1))
             ? N_Ok : N_Panic;
}
static NativeStatus u_checkbox_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Checkbox, as_str(*A(a, 0))->s, Str(), A(a, 2)->b ? 1 : 0, 0, 0, 0,
                     out, A(a, 1))
             ? N_Ok : N_Panic;
}
static NativeStatus u_slider_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Slider, Str(), Str(), A(a, 1)->i, A(a, 2)->i, A(a, 3)->i, 0, out,
                     A(a, 0))
             ? N_Ok : N_Panic;
}

static NativeStatus u_checkbox(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Checkbox, as_str(*A(a, 0))->s, as_str(*A(a, 1))->s,
                     A(a, 2)->b ? 1 : 0, 0, 0, 0, out) ? N_Ok : N_Panic;
}
static NativeStatus u_slider(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Slider, Str(), as_str(*A(a, 0))->s, A(a, 1)->i, A(a, 2)->i,
                     A(a, 3)->i, 0, out) ? N_Ok : N_Panic;
}
static NativeStatus u_field(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Field, as_str(*A(a, 1))->s, as_str(*A(a, 0))->s, -1, 0, 0, 0, out)
             ? N_Ok : N_Panic;
}
// ref で受ける形。打たれるたびに、渡された var が書き換わる（名札は要らない）。
// 覚えるのは借用そのものではなく var の番号で、書き戻すのは ui.show() の中
static NativeStatus u_field_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Value* p = val_deref(&a[0]);
  int slot = var_slot(vm, p);
  if (slot < 0) {
    vm.panic(vm.L("入力欄に ref で渡せるのは、一番外側の var だけです",
                  "ui.field(ref ...) takes a top-level var"));
    return N_Panic;
  }
  if (!(p->k == V_Obj && p->o->kind == O_Str)) {
    vm.panic(vm.L("入力欄に ref で渡せるのは string の var です",
                  "ui.field(ref ...) takes a string var"));
    return N_Panic;
  }
  if (!make_widget(vm, WK_Field, as_str(*p)->s, var_field_id(slot), 0, 0, 0, 0, out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
// 複数行の入力欄。行数（rows）を省くと 4 行ぶん
static const int kAreaRows = 4;
static NativeStatus u_textarea(VM& vm, Value* a, int n, Value& out) {
  int64_t rows = n >= 3 ? A(a, 2)->i : kAreaRows;
  if (rows < 1) rows = 1;
  return make_widget(vm, WK_Area, as_str(*A(a, 1))->s, as_str(*A(a, 0))->s, -1, rows, 0, 0, out)
             ? N_Ok : N_Panic;
}
// ref で受ける形。ui.field(ref ...) と同じで、覚えるのは「どの var か」
static NativeStatus u_textarea_ref(VM& vm, Value* a, int n, Value& out) {
  Value* p = val_deref(&a[0]);
  int slot = var_slot(vm, p);
  if (slot < 0) {
    vm.panic(vm.L("入力欄に ref で渡せるのは、一番外側の var だけです",
                  "ui.textarea(ref ...) takes a top-level var"));
    return N_Panic;
  }
  if (!(p->k == V_Obj && p->o->kind == O_Str)) {
    vm.panic(vm.L("入力欄に ref で渡せるのは string の var です",
                  "ui.textarea(ref ...) takes a string var"));
    return N_Panic;
  }
  int64_t rows = n >= 2 ? A(a, 1)->i : kAreaRows;
  if (rows < 1) rows = 1;
  if (!make_widget(vm, WK_Area, as_str(*p)->s, var_field_id(slot), 0, rows, 0, 0, out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
// 伏せ字の入力欄。中身は ui.field と同じ持ち方で、出すときだけ * になる
static NativeStatus u_password(VM& vm, Value* a, int n, Value& out) {
  // show を渡した形は、その間だけ**そのまま出す**（伏せない）
  bool masked = !(n >= 3 && A(a, 2)->b);
  return make_widget(vm, WK_Field, as_str(*A(a, 1))->s, as_str(*A(a, 0))->s, -1, masked ? 1 : 0,
                     0, 0, out) ? N_Ok : N_Panic;
}
static NativeStatus u_password_ref(VM& vm, Value* a, int n, Value& out) {
  Value* p = val_deref(&a[0]);
  int slot = var_slot(vm, p);
  if (slot < 0) {
    vm.panic(vm.L("入力欄に ref で渡せるのは、一番外側の var だけです",
                  "ui.password(ref ...) takes a top-level var"));
    return N_Panic;
  }
  if (!(p->k == V_Obj && p->o->kind == O_Str)) {
    vm.panic(vm.L("入力欄に ref で渡せるのは string の var です",
                  "ui.password(ref ...) takes a string var"));
    return N_Panic;
  }
  bool masked = !(n >= 2 && A(a, 1)->b);
  if (!make_widget(vm, WK_Field, as_str(*p)->s, var_field_id(slot), 0, masked ? 1 : 0, 0, 0, out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
// 入切のラジオ。checkbox と同じ受け取り方で、見た目が丸になる
static NativeStatus u_radio(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Radio, as_str(*A(a, 0))->s, as_str(*A(a, 1))->s,
                     A(a, 2)->b ? 1 : 0, 1, 0, 0, out) ? N_Ok : N_Panic;
}
static NativeStatus u_radio_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Radio, as_str(*A(a, 0))->s, Str(), A(a, 2)->b ? 1 : 0, 1, 0, 0, out,
                     A(a, 1)) ? N_Ok : N_Panic;
}
static const int kListRows = 4;   // ui.listbox で行数を省いたとき

// 並べる文字を持つ部品（選ぶ・一覧・タブ）。中身の置き場を文字の並びとして借りる
static NativeStatus make_options(VM& vm, int kind, Value* a, int id_at, int opts_at, int idx_at,
                                 int64_t rows, Value& out, bool fn) {
  Value* opts = A(a, opts_at);
  Str id;
  if (!fn) id = as_str(*A(a, id_at))->s;
  return make_widget(vm, kind, Str(), id, A(a, idx_at)->i, rows, 0, opts, out,
                     fn ? A(a, id_at) : 0) ? N_Ok : N_Panic;
}
static NativeStatus u_combo(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_options(vm, WK_Combo, a, 0, 1, 2, 0, out, false);
}
static NativeStatus u_combo_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_options(vm, WK_Combo, a, 0, 1, 2, 0, out, true);
}
static NativeStatus u_list(VM& vm, Value* a, int n, Value& out) {
  int64_t rows = n >= 4 ? A(a, 3)->i : kListRows;
  return make_options(vm, WK_List, a, 0, 1, 2, rows < 1 ? 1 : rows, out, false);
}
static NativeStatus u_list_fn(VM& vm, Value* a, int n, Value& out) {
  int64_t rows = n >= 4 ? A(a, 3)->i : kListRows;
  return make_options(vm, WK_List, a, 0, 1, 2, rows < 1 ? 1 : rows, out, true);
}
// いくつも選べる一覧。番号の並び（list<int>）で「いま選ばれているもの」を渡す
static bool make_multi(VM& vm, const Str& id, Value* opts, Value* chosen, int64_t rows,
                       Value& out, Value* action) {
  if (!make_widget(vm, WK_List, Str(), id, -1, rows, 0, opts, out, action)) return false;
  InstObj* o = as_inst(out);
  widget_put(o, WF_Flags, mk_int(w_field(out, WF_Flags) | WFL_Multi));
  widget_put(o, WF_Sel, val_retain(*chosen));
  return true;
}
static NativeStatus u_list_multi(VM& vm, Value* a, int n, Value& out) {
  int64_t rows = n >= 4 ? A(a, 3)->i : kListRows;
  return make_multi(vm, as_str(*A(a, 0))->s, A(a, 1), A(a, 2), rows < 1 ? 1 : rows, out, 0)
             ? N_Ok : N_Panic;
}
static NativeStatus u_list_multi_fn(VM& vm, Value* a, int n, Value& out) {
  int64_t rows = n >= 4 ? A(a, 3)->i : kListRows;
  return make_multi(vm, Str(), A(a, 1), A(a, 2), rows < 1 ? 1 : rows, out, A(a, 0))
             ? N_Ok : N_Panic;
}
static NativeStatus u_tabs(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_options(vm, WK_Tabs, a, 0, 1, 2, 0, out, false);
}
static NativeStatus u_tabs_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_options(vm, WK_Tabs, a, 0, 1, 2, 0, out, true);
}
// 数の入力欄。上と下の限りから外には出られない
static NativeStatus make_number(VM& vm, Value* a, Value& out, bool fn) {
  int64_t lo = A(a, 2)->i, hi = A(a, 3)->i;
  if (hi < lo) {
    vm.panic(vm.L("数の入力欄は、下より上を大きくします", "number needs hi >= lo"));
    return N_Panic;
  }
  int64_t val = A(a, 1)->i;
  if (val < lo) val = lo;
  if (val > hi) val = hi;
  return make_widget(vm, WK_Number, Str(), fn ? Str() : as_str(*A(a, 0))->s, val, lo, hi, 0, out,
                     fn ? A(a, 0) : 0) ? N_Ok : N_Panic;
}
static NativeStatus u_number(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_number(vm, a, out, false);
}
static NativeStatus u_number_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_number(vm, a, out, true);
}
// 小数を持つ部品（つまみ・引いて変える欄）を作る。値も限りも WF_Fa/Fb/Fc に置き、
// WFL_Float を立てておく。WF_B は「出す小数の桁」で、-1 は限りの広さから決める
static bool make_float_widget(VM& vm, int kind, const Str& id, double val, double lo, double hi,
                              Value& out, Value* action) {
  if (!(hi > lo)) hi = lo;
  if (val < lo) val = lo;
  if (val > hi) val = hi;
  if (!make_widget(vm, kind, Str(), id, 0, 0, 0, 0, out, action)) return false;
  InstObj* o = as_inst(out);
  widget_put(o, WF_Flags, mk_int(WFL_Float));
  widget_put(o, WF_Fa, mk_float(val));
  widget_put(o, WF_Fb, mk_float(lo));
  widget_put(o, WF_Fc, mk_float(hi));
  return true;
}

static NativeStatus u_slider_f(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_float_widget(vm, WK_Slider, as_str(*A(a, 0))->s, A(a, 1)->f, A(a, 2)->f,
                           A(a, 3)->f, out, 0) ? N_Ok : N_Panic;
}
static NativeStatus u_slider_f_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_float_widget(vm, WK_Slider, Str(), A(a, 1)->f, A(a, 2)->f, A(a, 3)->f, out,
                           A(a, 0)) ? N_Ok : N_Panic;
}
// 引いて変える欄。数の入力欄と同じ形（名札・関数・ref）で受ける
static NativeStatus u_drag(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t lo = A(a, 2)->i, hi = A(a, 3)->i;
  if (hi < lo) {
    vm.panic(vm.L("引いて変える欄は、下より上を大きくします", "drag needs hi >= lo"));
    return N_Panic;
  }
  int64_t val = clamp_i(A(a, 1)->i, lo, hi);
  return make_widget(vm, WK_Drag, Str(), as_str(*A(a, 0))->s, val, lo, hi, 0, out)
             ? N_Ok : N_Panic;
}
static NativeStatus u_drag_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t lo = A(a, 2)->i, hi = A(a, 3)->i;
  if (hi < lo) {
    vm.panic(vm.L("引いて変える欄は、下より上を大きくします", "drag needs hi >= lo"));
    return N_Panic;
  }
  int64_t val = clamp_i(A(a, 1)->i, lo, hi);
  return make_widget(vm, WK_Drag, Str(), Str(), val, lo, hi, 0, out, A(a, 0)) ? N_Ok : N_Panic;
}
static NativeStatus u_drag_f(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_float_widget(vm, WK_Drag, as_str(*A(a, 0))->s, A(a, 1)->f, A(a, 2)->f, A(a, 3)->f,
                           out, 0) ? N_Ok : N_Panic;
}
static NativeStatus u_drag_f_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_float_widget(vm, WK_Drag, Str(), A(a, 1)->f, A(a, 2)->f, A(a, 3)->f, out,
                           A(a, 0)) ? N_Ok : N_Panic;
}

// --- ref で受ける形 -------------------------------------------------------
// 変数を ref で渡すと、動いたときに**その変数が直に書き換わる**。
// 名札も update() も ui.value() も要らない。値を持つのは今までどおり書く人で、
// 処理系は「どの var か」だけを覚える（ui.field(ref ...) と同じ仕組み）
static bool ref_slot(VM& vm, Value* a, int at, const char* what, int* slot, Value** got) {
  Value* p = val_deref(&a[at]);
  *slot = var_slot(vm, p);
  if (*slot < 0) {
    vm.panic(vm.L(Str(what) + " に ref で渡せるのは、一番外側の var だけです",
                  Str(what) + " takes a top-level var by ref"));
    return false;
  }
  *got = p;
  return true;
}

static NativeStatus u_checkbox_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 1, "ui.checkbox", &slot, &p)) return N_Panic;
  // 持たせるのは**いまの入切**。押されたら place_checkbox がひっくり返す
  if (!make_widget(vm, WK_Checkbox, as_str(*A(a, 0))->s, Str(), p->b ? 1 : 0, 0, 0, 0, out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
// ラジオ。押されたら「自分の数」を書き戻す。選ばれて見えるのは、いまの数と同じとき
static NativeStatus u_radio_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 1, "ui.radio", &slot, &p)) return N_Panic;
  int64_t mine = A(a, 2)->i;
  if (!make_widget(vm, WK_Radio, as_str(*A(a, 0))->s, Str(), p->i == mine ? 1 : 0, mine, 0, 0,
                   out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
static NativeStatus u_slider_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 0, "ui.slider", &slot, &p)) return N_Panic;
  if (!make_widget(vm, WK_Slider, Str(), Str(), p->i, A(a, 1)->i, A(a, 2)->i, 0, out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
static NativeStatus u_number_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 0, "ui.number", &slot, &p)) return N_Panic;
  int64_t lo = A(a, 1)->i, hi = A(a, 2)->i;
  if (hi < lo) {
    vm.panic(vm.L("数の入力欄は、下より上を大きくします", "number needs hi >= lo"));
    return N_Panic;
  }
  int64_t val = p->i < lo ? lo : (p->i > hi ? hi : p->i);
  if (!make_widget(vm, WK_Number, Str(), Str(), val, lo, hi, 0, out)) return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
// 小数を ref で受ける形（つまみ・引いて変える欄）
static NativeStatus make_float_ref(VM& vm, int kind, const char* what, Value* a, Value& out) {
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 0, what, &slot, &p)) return N_Panic;
  if (!make_float_widget(vm, kind, Str(), p->f, A(a, 1)->f, A(a, 2)->f, out, 0)) return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
static NativeStatus u_slider_f_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_float_ref(vm, WK_Slider, "ui.slider", a, out);
}
static NativeStatus u_drag_f_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_float_ref(vm, WK_Drag, "ui.drag", a, out);
}
static NativeStatus u_drag_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 0, "ui.drag", &slot, &p)) return N_Panic;
  int64_t lo = A(a, 1)->i, hi = A(a, 2)->i;
  if (hi < lo) {
    vm.panic(vm.L("引いて変える欄は、下より上を大きくします", "drag needs hi >= lo"));
    return N_Panic;
  }
  if (!make_widget(vm, WK_Drag, Str(), Str(), clamp_i(p->i, lo, hi), lo, hi, 0, out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
// いくつも選べる一覧を ref で受ける形。押されるたびに、その var の並びが入れ替わる
static NativeStatus u_list_multi_ref(VM& vm, Value* a, int n, Value& out) {
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 0, "ui.listbox", &slot, &p)) return N_Panic;
  int64_t rows = n >= 3 ? A(a, 2)->i : kListRows;
  if (!make_multi(vm, Str(), A(a, 1), p, rows < 1 ? 1 : rows, out, 0)) return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
// 並べたものから選ぶ形（選ぶ・一覧・タブ）。番号を ref で受ける
static NativeStatus make_options_ref(VM& vm, int kind, Value* a, int64_t rows, Value& out) {
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 0, "ui.combo / ui.listbox / ui.tabs", &slot, &p)) return N_Panic;
  if (!make_widget(vm, kind, Str(), Str(), p->i, rows, 0, A(a, 1), out)) return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}
static NativeStatus u_combo_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_options_ref(vm, WK_Combo, a, 0, out);
}
static NativeStatus u_list_ref(VM& vm, Value* a, int n, Value& out) {
  int64_t rows = n >= 3 ? A(a, 2)->i : kListRows;
  return make_options_ref(vm, WK_List, a, rows < 1 ? 1 : rows, out);
}
static NativeStatus u_tabs_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_options_ref(vm, WK_Tabs, a, 0, out);
}

static NativeStatus u_space(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Space, Str(), Str(), A(a, 0)->i, 0, 0, 0, out) ? N_Ok : N_Panic;
}
// 並んでいる向きに場所を取る空き。数を渡せばその画素ぶん、省くと余りをぜんぶ取る
static NativeStatus u_spacer(VM& vm, Value* a, int n, Value& out) {
  int64_t size = n >= 1 ? A(a, 0)->i : 0;
  if (size < 0) size = 0;
  return make_widget(vm, WK_Spacer, Str(), Str(), size, n >= 1 ? 0 : 1, 0, 0, out)
             ? N_Ok : N_Panic;
}
static NativeStatus u_col(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Column, Str(), Str(), 0, 0, 0, A(a, 0), out) ? N_Ok : N_Panic;
}
// 格子。cols 列に、左上から順に詰める
static NativeStatus u_grid(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t cols = A(a, 0)->i;
  if (cols < 1) {
    vm.panic(vm.L("格子の列の数は 1 以上にします", "grid column count must be 1 or more"));
    return N_Panic;
  }
  return make_widget(vm, WK_Grid, Str(), Str(), cols, 0, 0, A(a, 1), out) ? N_Ok : N_Panic;
}
static NativeStatus u_row(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Row, Str(), Str(), 0, 0, 0, A(a, 0), out) ? N_Ok : N_Panic;
}
// 中身をまとめて、置ける幅の真ん中に置く（ui.col(...).align("center") と同じ）
static NativeStatus u_center(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!make_widget(vm, WK_Column, Str(), Str(), 0, 0, 0, A(a, 0), out)) return N_Panic;
  InstObj* o = as_inst(out);
  val_release(o->fields[WF_Align]);
  o->fields[WF_Align] = mk_int(WA_Center);
  return N_Ok;
}
// 色の入力。色そのものを持つのは書く人（ref か名札）
static NativeStatus u_color(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Color, Str(), as_str(*A(a, 0))->s, (int64_t)to_color(A(a, 1)->i),
                     0, 0, 0, out) ? N_Ok : N_Panic;
}
static NativeStatus u_color_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Color, Str(), Str(), (int64_t)to_color(A(a, 1)->i), 0, 0, 0, out,
                     A(a, 0)) ? N_Ok : N_Panic;
}
static NativeStatus u_color_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 0, "ui.color", &slot, &p)) return N_Panic;
  if (!make_widget(vm, WK_Color, Str(), Str(), (int64_t)to_color(p->i), 0, 0, 0, out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}

// 折りたためる木。開いているかどうかは書く人が持つ（ref か名札）
static NativeStatus u_tree(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Tree, as_str(*A(a, 0))->s, as_str(*A(a, 1))->s,
                     A(a, 2)->b ? 1 : 0, 0, 0, A(a, 3), out) ? N_Ok : N_Panic;
}
static NativeStatus u_tree_fn(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Tree, as_str(*A(a, 0))->s, Str(), A(a, 2)->b ? 1 : 0, 0, 0,
                     A(a, 3), out, A(a, 1)) ? N_Ok : N_Panic;
}
static NativeStatus u_tree_ref(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int slot = 0;
  Value* p = 0;
  if (!ref_slot(vm, a, 1, "ui.tree", &slot, &p)) return N_Panic;
  if (!make_widget(vm, WK_Tree, as_str(*A(a, 0))->s, Str(), p->b ? 1 : 0, 0, 0, A(a, 2), out))
    return N_Panic;
  set_var_slot(out, slot);
  return N_Ok;
}

// 重ね置き。中身を同じところに重ねる（絵の上に字を出す、など）
static NativeStatus u_stack(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Stack, Str(), Str(), 0, 0, 0, A(a, 0), out) ? N_Ok : N_Panic;
}
// 巻物。高さを決めて（.height）、はみ出したぶんを送って見る
static NativeStatus u_scroll(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Scroll, Str(), Str(), 0, 0, 0, A(a, 0), out) ? N_Ok : N_Panic;
}
static NativeStatus u_scroll_id(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Scroll, Str(), as_str(*A(a, 0))->s, 0, 0, 0, A(a, 1), out)
             ? N_Ok : N_Panic;
}
// 区切り線。置ける幅いっぱいに引く
static NativeStatus u_divider(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  return make_widget(vm, WK_Divider, Str(), Str(), 0, 0, 0, 0, out) ? N_Ok : N_Panic;
}

// --- 出す -----------------------------------------------------------------
static NativeStatus show_at(VM& vm, Value* a, int x, int y, Value& out) {
  if (!need_open(vm)) return N_Panic;
  Value* view = A(a, 0);
  g_lang_ja = (vm.lang() == LANG_JA);
  menu_hit();          // メニューが出ていれば、押しはそちらが先に受ける
  color_hit();         // 色を選ぶ板も同じ（出ていなければ何もしない）
  g_hit_id.clear();
  g_hit_text.clear();
  g_hit_val = 0;
  g_hit_any = false;
  g_edited = false;
  g_vm = &vm;          // ref で受けた入力欄の書き戻しに使う
  clear_hit_action();
  // 覚えておく。窓の縁を引いている間、この部品を新しい大きさで置き直す（ui_live_redraw）
  val_release(g_last_view);
  g_last_view = val_retain(*view);
  g_has_view = true;
  g_last_x = x;
  g_last_y = y;
  g_tip_text.clear();
  g_point_x = g_point_y = -1;   // 絵の上に乗っていれば、置くときに入る
  g_row_axis = false;   // いちばん外は縦並びとみなす（ui.spacer の向き）
  g_click_seen = g_mpress[0];
  g_focus_next = g_focus;
  if (g_click_seen) g_focus_next.clear();   // どこも押されなければ焦点は外れる

  // 使える場所は、置き始めたところから見て、面の中に残っているぶん。
  // 高さも同じように数えるので、いちばん外側でも取り分（fr）が使える
  int avail = g_w - x * 2;
  if (avail < 1) avail = g_w > x ? g_w - x : 1;
  int avail_h = g_h - y * 2;
  if (avail_h < 1) avail_h = g_h > y ? g_h - y : 1;
  place(*view, x, y, avail, avail_h, true);
  g_focus = g_focus_next;
  // 押された入力欄に焦点が来なかったら、覚えておいた位置は捨てる
  if (g_want_caret >= 0 && !(g_focus == g_want_id)) {
    g_want_caret = -1;
    g_want_len = 0;
    g_want_id.clear();
  }
  // 押しは受け取った部品が使い切る。こうすると、同じ回にもう一度 ui.show() しても
  // 二度は効かない（ui.run が、状態を変えたあとすぐ描き直すのに要る）
  if (g_hit_any) { g_mpress[0] = false; g_mpress[2] = false; }
  color_draw();        // 色の板・説明・一覧は、部品の上に描く
  tip_draw();
  menu_draw();
  out = mk_str(g_hit_id);
  return N_Ok;
}
// 窓の縁を引いている間、移植層から呼ばれる（platform.h の set_redraw）。
// その間 OS はプログラムを止めているので、Shark の view() は呼び直せない。
// でも引いている間は状態も変わらないから、**最後に見せた部品を置き直せば同じ絵**になる。
// 文字の折り返しも取り分（fr）も、新しい大きさで掛かり直す
static bool ui_live_redraw(int w, int h) {
  if (!g_open || !g_has_view) return false;
  resize_surface(w, h);
  if (g_w != w || g_h != h) return false;   // 作り直せなかった（メモリの上限など）
  for (int yy = 0; yy < g_h; yy++) span(0, yy, g_w, g_bg);   // ui.clear() と同じ
  g_replay = true;
  int x0 = g_last_x, y0 = g_last_y;
  int avail = g_w - x0 * 2;
  if (avail < 1) avail = g_w > x0 ? g_w - x0 : 1;
  int avail_h = g_h - y0 * 2;
  if (avail_h < 1) avail_h = g_h > y0 ? g_h - y0 : 1;
  place(g_last_view, x0, y0, avail, avail_h, true);
  g_replay = false;
  const PlatformScreen* s = platform().screen;
  if (g_visible && s) s->present(g_px, g_w, g_h);
  return true;
}

static NativeStatus u_show(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return show_at(vm, a, 4, 4, out);
}
static NativeStatus u_show_at(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return show_at(vm, a, (int)A(a, 1)->i, (int)A(a, 2)->i, out);
}
static NativeStatus u_value(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_int(g_hit_val);
  return N_Ok;
}
static NativeStatus u_chosen(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = (g_hit_list.k == V_Obj && g_hit_list.o->kind == O_List) ? val_retain(g_hit_list)
                                                               : mk_list();
  return N_Ok;
}
static NativeStatus u_float_value(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_float(g_hit_valf);
  return N_Ok;
}
static NativeStatus u_text_value(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_str(g_hit_text);
  return N_Ok;
}
// この回、ref で受けた部品が var を書き換えたか
static NativeStatus u_edited(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_bool(g_edited);
  return N_Ok;
}
// この回、関数を持った部品が押されたか
static NativeStatus u_has_action(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_bool(g_hit_action.k == V_Obj);
  return N_Ok;
}
// その関数。無ければ「何もしない関数」を返す（前奏の __noop）
static NativeStatus u_action(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (g_hit_action.k == V_Obj) { out = val_retain(g_hit_action); return N_Ok; }
  int fi = -1;
  if (vm.prog)
    for (int i = 0; i < vm.prog->funcs.size(); i++)
      if (!vm.prog->funcs[i]->owner && vm.prog->funcs[i]->name == "__noop") { fi = i; break; }
  out = mk_func(fi);
  return N_Ok;
}

static NativeStatus u_theme(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  g_bg = to_color(A(a, 0)->i);
  g_fg = to_color(A(a, 1)->i);
  g_accent = to_color(A(a, 2)->i);
  out = mk_void();
  return N_Ok;
}

// --- 切り貼りと、右で押したときのメニュー ---------------------------------
static NativeStatus u_clipboard(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  Str s;
  if (!clip_get(&s)) s.clear();
  out = mk_str(s);
  return N_Ok;
}
static NativeStatus u_set_clipboard(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const PlatformScreen* p = platform().screen;
  if (!p || !p->clipboard_set) { out = mk_bool(false); return N_Ok; }
  clip_set(as_str(*A(a, 0))->s);
  out = mk_bool(true);
  return N_Ok;
}
// いま選んでいる文字
static NativeStatus u_selected(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  int st = 0, ln = 0;
  sel_get(g_input_seed, &st, &ln);
  out = mk_str(ln > 0 ? sub_chars(g_input_seed, st, ln) : Str());
  return N_Ok;
}
// この場所にメニューを出す。選ばれるか、外を押されるまで出たまま
static NativeStatus u_menu(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  ListObj* items = as_list(*A(a, 2));
  Vec<Str> v;
  for (int i = 0; i < items->v.size(); i++) v.push(as_str(items->v[i])->s);
  menu_open_at((int)A(a, 0)->i, (int)A(a, 1)->i, v, Str());
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_menu_pick(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  // 入力欄が自分で出したメニューの結果は、そちらで使うので返さない
  out = mk_int(g_menu_owner.size() > 0 ? -1 : g_menu_pick);
  return N_Ok;
}
static NativeStatus u_menu_close(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  g_menu_on = false;
  g_menu_items.clear();
  g_menu_keys.clear();
  g_menu_owner.clear();
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_menu_open(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_bool(g_menu_on);
  return N_Ok;
}

// ------------------------------------------------------------------ 登録
void register_ui(Registry& r) {
  TypeTable& t = r.types();
  Type* tv = t.t_void();
  Type* ti = t.t_int();
  Type* tf = t.t_float();
  Type* tb = t.t_bool();
  Type* ts = t.t_string();
  Type* tby = t.t_bytes();
  Type* tli = t.list_of(ti);

  r.add("ui.open", u_open, tv, ti, ti);
  r.add("ui.open", u_open, tv, ts, ti, ti);
  r.add("ui.open", u_open, tv, ts, ti, ti, tb);
  r.add("ui.close", u_close, tv);
  r.add("ui.width", u_width, ti);
  r.add("ui.height", u_height, ti);
  r.add("ui.visible", u_visible, tb);
  r.add("ui.scale", u_scale, ti);
  r.add("ui.pixel_ratio", u_pixel_ratio, tf);
  r.add("ui.present", u_present, tv);
  r.add("ui.frame", u_frame, tv);
  r.add("ui.frame", u_frame, tv, ti);

  r.add("ui.rgb", u_rgb, ti, ti, ti, ti);
  r.add("ui.rgba", u_rgba, ti, ti, ti, ti, ti);
  r.add("ui.alpha", u_alpha, ti, ti);
  r.add("ui.red", u_red, ti, ti);
  r.add("ui.green", u_green, ti, ti);
  r.add("ui.blue", u_blue, ti, ti);
  r.add("ui.mix", u_mix, ti, ti, ti, tf);

  r.add("ui.clear", u_clear, tv, ti);
  r.add("ui.clear", u_clear, tv);
  r.add("ui.set", u_set, tv, ti, ti, ti);
  r.add("ui.get", u_get, ti, ti, ti);
  r.add("ui.hline", u_hline, tv, ti, ti, ti, ti);
  r.add("ui.vline", u_vline, tv, ti, ti, ti, ti);
  r.add("ui.line", u_line, tv, ti, ti, ti, ti, ti);
  r.add("ui.rect", u_rect, tv, ti, ti, ti, ti, ti);
  r.add("ui.fill_rect", u_fill_rect, tv, ti, ti, ti, ti, ti);
  r.add("ui.circle", u_circle, tv, ti, ti, ti, ti);
  r.add("ui.fill_circle", u_fill_circle, tv, ti, ti, ti, ti);
  r.add("ui.clip", u_clip, tv, ti, ti, ti, ti);
  r.add("ui.clip_off", u_clip_off, tv);
  r.add("ui.blit", u_blit, tv, ti, ti, ti, tli);

  // 絵（Canvas）。tcv は型検査のときに本物のクラスに差し替わる（ui_bind_canvas_class）
  Type* tcv = canvas_stub(r.types());
  Type* tocv = r.types().optional_of(tcv);
  r.add("ui.canvas", u_canvas, tcv, ti, ti);
  r.add("ui.canvas", u_canvas, tcv, ti, ti, ti);
  r.add("ui.load_png", u_load_png, tocv, tby);
  r.add("ui.draw", u_draw, tv, tcv, ti, ti);
  r.add("ui.draw", u_draw, tv, tcv, ti, ti, ti, ti);

  // 三角形と奥行き（3D の土台）
  r.add("ui.tri", u_tri, tv, ti, ti, ti, ti, ti, ti, ti, ti, ti, ti);
  r.add("ui.depth", u_depth, tv, tb);
  r.add("ui.clear_depth", u_clear_depth, tv);

  r.add("ui.text", u_text, tv, ti, ti, ts, ti);
  r.add("ui.text", u_text_scaled, tv, ti, ti, ts, ti, ti);
  r.add("ui.text_width", u_text_width, ti, ts);
  r.add("ui.text_width", u_text_width, ti, ts, ti);
  r.add("ui.text_height", u_text_height, ti);
  r.add("ui.text_height", u_text_height, ti, ti);

  r.add("ui.poll", u_poll, tb);
  r.add("ui.quit", u_quit, tv);
  r.add("ui.cursor", u_cursor, tv, ts);
  r.add("ui.key", u_key, tb, ts);
  r.add("ui.pressed", u_pressed, tb, ts);
  r.add("ui.released", u_released, tb, ts);
  r.add("ui.typed", u_typed, ts);
  r.add("ui.mouse_x", u_mouse_x, ti);
  r.add("ui.mouse_y", u_mouse_y, ti);
  r.add("ui.mouse", u_mouse, tb, ti);
  r.add("ui.clicked", u_clicked, tb, ti);
  r.add("ui.wheel", u_wheel, ti);
  r.add("ui.wheel_x", u_wheel_x, ti);
  r.add("ui.input", u_input, ts, ti, ti, ti, ts);
  r.add("ui.input_off", u_input_off, tv);
  r.add("ui.marked", u_marked, ts);
  r.add("ui.has_ime", u_has_ime, tb);
  r.add("ui.selected", u_selected, ts);
  r.add("ui.clipboard", u_clipboard, ts);
  r.add("ui.set_clipboard", u_set_clipboard, tb, ts);
  r.add("ui.menu", u_menu, tv, ti, ti, t.list_of(ts));
  r.add("ui.menu_pick", u_menu_pick, ti);
  r.add("ui.menu_close", u_menu_close, tv);
  r.add("ui.menu_open", u_menu_open, tb);

  r.add("ui.pick_file", u_pick_file, t.optional_of(ts));
  r.add("ui.pick_file", u_pick_file, t.optional_of(ts), ts);
  r.add("ui.pick_save", u_pick_save, t.optional_of(ts));
  r.add("ui.pick_save", u_pick_save, t.optional_of(ts), ts);
  r.add("ui.pick_save", u_pick_save, t.optional_of(ts), ts, ts);
  r.add("ui.to_png", u_to_png, tby);

  r.add("ui.font", u_font_size, tb, ti);
  r.add("ui.font", u_font_path, tb, ts, ti);
  r.add("ui.font", u_font_bytes, tb, tby, ti);
  r.add("ui.font_builtin", u_font_builtin, tv);
  r.add("ui.font_name", u_font_name, ts);

  // 宣言的な層（Widget を1つ返す。入れ子は ui.col / ui.row / ui.grid に配列で渡す）。
  // Widget の本物のクラスは型検査のときに作られるので、ここでは仮の型で登録しておく
  Type* tw = widget_stub(t);
  Type* tlw = t.list_of(tw);
  r.add("ui.label", u_label, tw, ts);
  r.add("ui.button", u_button, tw, ts, ts);
  r.add("ui.checkbox", u_checkbox, tw, ts, ts, tb);
  r.add("ui.slider", u_slider, tw, ts, ti, ti, ti);
  r.add("ui.field", u_field, tw, ts, ts);
  // ref で受ける形。覚えるのは借用ではなく「どの var か」なので、
  // 型検査は一番外側の var だけを通す（check.cpp の E0307）
  r.mark_ref0_var(r.add("ui.field", u_field_ref, tw, ts));
  // 複数行の入力欄。受け取り方は ui.field と同じ2つ（ref と名札）
  r.add("ui.textarea", u_textarea, tw, ts, ts);
  r.add("ui.textarea", u_textarea, tw, ts, ts, ti);
  r.mark_ref0_var(r.add("ui.textarea", u_textarea_ref, tw, ts));
  r.mark_ref0_var(r.add("ui.textarea", u_textarea_ref, tw, ts, ti));
  // 伏せ字の入力欄
  r.add("ui.password", u_password, tw, ts, ts);
  r.add("ui.password", u_password, tw, ts, ts, tb);
  r.mark_ref0_var(r.add("ui.password", u_password_ref, tw, ts));
  r.mark_ref0_var(r.add("ui.password", u_password_ref, tw, ts, tb));
  r.add("ui.space", u_space, tw, ti);
  r.add("ui.spacer", u_spacer, tw);
  r.add("ui.spacer", u_spacer, tw, ti);
  r.add("ui.col", u_col, tw, tlw);
  r.add("ui.row", u_row, tw, tlw);
  r.add("ui.grid", u_grid, tw, ti, tlw);
  r.add("ui.center", u_center, tw, tlw);
  r.add("ui.color", u_color, tw, ts, ti);
  r.add("ui.tree", u_tree, tw, ts, ts, tb, tlw);
  r.add("ui.stack", u_stack, tw, tlw);
  r.add("ui.scroll", u_scroll, tw, tlw);
  r.add("ui.scroll", u_scroll_id, tw, ts, tlw);
  r.add("ui.divider", u_divider, tw);
  Vec<Type*> no_params;
  Type* tact = t.func_type(no_params, tv);   // func() -> void
  r.add("ui.button", u_button_fn, tw, ts, tact);
  r.add("ui.button", u_button_w, tw, tw, ts);
  r.add("ui.button", u_button_w_fn, tw, tw, tact);
  r.add("ui.tree", u_tree_fn, tw, ts, tact, tb, tlw);
  r.add("ui.color", u_color_fn, tw, tact, ti);
  r.mark_ref_var(r.add("ui.color", u_color_ref, tw, ti), 0);
  r.mark_ref_var(r.add("ui.tree", u_tree_ref, tw, ts, tb, tlw), 1);
  r.add("ui.image", u_image, tw, tcv);
  r.add("ui.image", u_image_id, tw, ts, tcv);
  r.add("ui.image", u_image_fn, tw, tact, tcv);
  r.add("ui.point_x", u_point_x, ti);
  r.add("ui.point_y", u_point_y, ti);
  r.add("ui.checkbox", u_checkbox_fn, tw, ts, tact, tb);
  r.add("ui.slider", u_slider_fn, tw, tact, ti, ti, ti);
  // 並べたものから選ぶ部品。並べる文字は list<string> で渡す
  Type* tls = t.list_of(ts);
  r.add("ui.radio", u_radio, tw, ts, ts, tb);
  r.add("ui.radio", u_radio_fn, tw, ts, tact, tb);
  r.add("ui.combo", u_combo, tw, ts, tls, ti);
  r.add("ui.combo", u_combo_fn, tw, tact, tls, ti);
  r.add("ui.listbox", u_list, tw, ts, tls, ti);
  r.add("ui.listbox", u_list, tw, ts, tls, ti, ti);
  r.add("ui.listbox", u_list_fn, tw, tact, tls, ti);
  r.add("ui.listbox", u_list_fn, tw, tact, tls, ti, ti);
  // いくつも選べる形（選ばれている番号を list<int> で渡す）
  r.add("ui.listbox", u_list_multi, tw, ts, tls, tli);
  r.add("ui.listbox", u_list_multi, tw, ts, tls, tli, ti);
  r.add("ui.listbox", u_list_multi_fn, tw, tact, tls, tli);
  r.add("ui.listbox", u_list_multi_fn, tw, tact, tls, tli, ti);
  r.add("ui.tabs", u_tabs, tw, ts, tls, ti);
  r.add("ui.tabs", u_tabs_fn, tw, tact, tls, ti);
  r.add("ui.number", u_number, tw, ts, ti, ti, ti);
  r.add("ui.number", u_number_fn, tw, tact, ti, ti, ti);
  // 小数のつまみと、引いて変える欄
  r.add("ui.slider", u_slider_f, tw, ts, tf, tf, tf);
  r.add("ui.slider", u_slider_f_fn, tw, tact, tf, tf, tf);
  r.add("ui.drag", u_drag, tw, ts, ti, ti, ti);
  r.add("ui.drag", u_drag_fn, tw, tact, ti, ti, ti);
  r.add("ui.drag", u_drag_f, tw, ts, tf, tf, tf);
  r.add("ui.drag", u_drag_f_fn, tw, tact, tf, tf, tf);
  // 変数を ref で渡す形。動いたらその変数が直に書き換わるので、
  // 名札も update() も ui.value() も要らない
  r.mark_ref_var(r.add("ui.checkbox", u_checkbox_ref, tw, ts, tb), 1);
  r.mark_ref_var(r.add("ui.radio", u_radio_ref, tw, ts, ti, ti), 1);
  r.mark_ref_var(r.add("ui.slider", u_slider_ref, tw, ti, ti, ti), 0);
  r.mark_ref_var(r.add("ui.number", u_number_ref, tw, ti, ti, ti), 0);
  r.mark_ref_var(r.add("ui.slider", u_slider_f_ref, tw, tf, tf, tf), 0);
  r.mark_ref_var(r.add("ui.drag", u_drag_ref, tw, ti, ti, ti), 0);
  r.mark_ref_var(r.add("ui.drag", u_drag_f_ref, tw, tf, tf, tf), 0);
  r.mark_ref_var(r.add("ui.combo", u_combo_ref, tw, ti, tls), 0);
  r.mark_ref_var(r.add("ui.listbox", u_list_ref, tw, ti, tls), 0);
  r.mark_ref_var(r.add("ui.listbox", u_list_ref, tw, ti, tls, ti), 0);
  r.mark_ref_var(r.add("ui.listbox", u_list_multi_ref, tw, tli, tls), 0);
  r.mark_ref_var(r.add("ui.listbox", u_list_multi_ref, tw, tli, tls, ti), 0);
  r.mark_ref_var(r.add("ui.tabs", u_tabs_ref, tw, ti, tls), 0);
  r.add("ui.edited", u_edited, tb);
  r.add("ui.has_action", u_has_action, tb);
  r.add("ui.action", u_action, tact);
  r.add("ui.show", u_show, ts, tw);
  r.add("ui.show", u_show_at, ts, tw, ti, ti);
  r.add("ui.value", u_value, ti);
  r.add("ui.float_value", u_float_value, tf);
  r.add("ui.chosen", u_chosen, tli);
  r.add("ui.text_value", u_text_value, ts);
  r.add("ui.theme", u_theme, tv, ti, ti, ti);
  r.enable_module("std.ui");
}

}  // namespace shark

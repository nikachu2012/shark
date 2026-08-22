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

namespace shark {

// 字形を FreeType で出す口（入っていなければ中身は空）
#include "font_ft.inc"

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

static void reset_input() {
  for (int i = 0; i < SKEY_Max; i++) { g_key[i] = false; g_press[i] = false; g_rel[i] = false; g_hit[i] = false; }
  for (int i = 0; i < 3; i++) { g_mb[i] = false; g_mpress[i] = false; g_mrel[i] = false; }
  g_typed.clear();
  g_mx = g_my = 0;
}

static void ui_reset_widgets();   // 宣言的な層の覚えごとを消す（下で定義）

static void input_stop();   // 下で定義

static void drop_surface() {
  input_stop();
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
// 画面を開いたまま終わっても、端末が元に戻るようにしておく
void ui_shutdown() {
  drop_surface();
#if defined(SHARK_FREETYPE)
  ft::unload();   // 読んだフォントと、字形の覚え書きも返す
#endif
}

static bool need_open(VM& vm) {
  if (g_open) return true;
  vm.panic(vm.L("先に ui.open(横, 縦) で面を用意します",
                "call ui.open(width, height) first"));
  return false;
}

// ------------------------------------------------------------------ 描く土台
static inline uint32_t to_color(int64_t v) { return (uint32_t)(v & 0xffffff); }

static inline void put(int x, int y, uint32_t c) {
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
  for (int i = x0; i <= x1; i++) row[i] = c;
}

// ------------------------------------------------------------------ 面
static NativeStatus u_open(VM& vm, Value* a, int n, Value& out) {
  Str title;
  int64_t w, h;
  if (n >= 3) { title = as_str(*A(a, 0))->s; w = A(a, 1)->i; h = A(a, 2)->i; }
  else { title = Str("Shark"); w = A(a, 0)->i; h = A(a, 1)->i; }
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
  // 画面が無い機種、端末でないところ（出力を | や > で受けているとき）では
  // false が返る。そのときは見えない面に描くだけになる
  g_visible = s && s->open(title.c_str(), g_w, g_h);
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
static NativeStatus u_visible(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n; out = mk_bool(g_visible); return N_Ok;
}
static NativeStatus u_present(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  if (g_visible && platform().screen) platform().screen->present(g_px, g_w, g_h);
  out = mk_void();
  return N_Ok;
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
  for (int sh = 16; sh >= 0; sh -= 8) {
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
  for (int y = g_cy0; y <= g_cy1; y++) span(g_cx0, y, g_cx1 - g_cx0 + 1, c);
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_set(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!need_open(vm)) return N_Panic;
  put((int)A(a, 0)->i, (int)A(a, 1)->i, to_color(A(a, 2)->i));
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
  for (int i = 0; i < src->v.size(); i++) {
    put(x + i % w, y + i / w, to_color(src->v[i].i));
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
// 内蔵の 5×7 か、FreeType で読んだフォントか（ui.font() で選ぶ）。
// 何もしなければ内蔵のまま。内蔵は ASCII しか持たないが、**どの機種でも同じに出る**
// （spec/library/ui.md）
// 1行の高さ（字の上端から下端まで）。部品の高さもこれで決まる
static int line_h(int scale) {
#if defined(SHARK_FREETYPE)
  if (ft::active()) return ft::line_height(ft::size() * scale);
#endif
  return kCellH * scale;
}
// 改行したときに下げる幅。字の高さそのままだと詰まって見えるので少し空ける。
// 内蔵の 5×7 は枠（6×8）に空きを含んでいるので、そのまま
static int line_pitch(int scale) {
#if defined(SHARK_FREETYPE)
  if (ft::active()) return line_h(scale) * 5 / 4;
#endif
  return line_h(scale);
}
// その文字のぶん、次の字までどれだけ進むか
static int advance_of(int cp, int scale) {
#if defined(SHARK_FREETYPE)
  if (ft::active()) {
    const ft::Glyph* g = ft::glyph(cp, ft::size() * scale);
    return g ? g->adv : 0;
  }
#endif
  (void)cp;
  return kCellW * scale;
}

#if defined(SHARK_FREETYPE)
// 濃さ a（0〜255）で、下地と混ぜて1点打つ。FreeType の字は縁がなめらかなので混ぜる
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
#endif

// 1文字描く。(x, y) はその文字の枠の左上
static void draw_cp(int x, int y, int cp, int scale, uint32_t c) {
#if defined(SHARK_FREETYPE)
  if (ft::active()) {
    int px = ft::size() * scale;
    const ft::Glyph* g = ft::glyph(cp, px);
    if (!g) return;
    int base = y + ft::ascender(px);   // 基準線。字形はここから上下に置かれる
    for (int gy = 0; gy < g->h; gy++)
      for (int gx = 0; gx < g->w; gx++)
        blend_at(x + g->left + gx, base - g->top + gy, c,
                 g->bits ? g->bits[gy * g->w + gx] : 0);
    return;
  }
#endif
  draw_glyph(cp, x, y, scale, c);
}

static void put_text(int x, int y, const Str& s, int scale, uint32_t c) {
  if (scale < 1) scale = 1;
  int lh = line_pitch(scale);
  int cx = x, cy = y;
  int at = 0;
  while (at < s.size()) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    at += adv;
    if (cp == '\n') { cx = x; cy += lh; continue; }
    draw_cp(cx, cy, cp, scale, c);
    cx += advance_of(cp, scale);
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
static int text_px_width(const Str& s, int scale) {
  if (scale < 1) scale = 1;
  int at = 0, cur = 0, best = 0;
  while (at < s.size()) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    at += adv;
    if (cp == '\n') { cur = 0; continue; }
    cur += advance_of(cp, scale);
    if (cur > best) best = cur;
  }
  return best;
}

static NativeStatus draw_text(VM& vm, Value* a, int scale, Value& out) {
  if (!need_open(vm)) return N_Panic;
  put_text((int)A(a, 0)->i, (int)A(a, 1)->i, as_str(*A(a, 2))->s, scale, to_color(A(a, 3)->i));
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
  out = mk_int(text_px_width(as_str(*A(a, 0))->s, scale));
  return N_Ok;
}
static NativeStatus u_text_height(VM& vm, Value* a, int n, Value& out) {
  (void)vm;
  out = mk_int(line_h(n >= 1 ? (int)A(a, 0)->i : 1));
  return N_Ok;
}

// ------------------------------------------------------------------ 入力
struct KeyName { const char* name; int code; };
static const KeyName kKeyNames[] = {
    {"left", SKEY_Left}, {"right", SKEY_Right}, {"up", SKEY_Up}, {"down", SKEY_Down},
    {"enter", SKEY_Enter}, {"esc", SKEY_Escape}, {"tab", SKEY_Tab}, {"back", SKEY_Back},
    {"delete", SKEY_Delete}, {"home", SKEY_Home}, {"end", SKEY_End},
    {"pageup", SKEY_PageUp}, {"pagedown", SKEY_PageDown},
    {"shift", SKEY_Shift}, {"ctrl", SKEY_Ctrl}, {"alt", SKEY_Alt},
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
static bool g_dragging = false;         // なぞって選んでいる最中
static int g_drag_anchor = 0;           // なぞり始めた文字
static bool g_lang_ja = true;           // 内蔵メニューの言い方

// 右で押したときのメニュー
static bool g_menu_on = false;
static int g_menu_x = 0, g_menu_y = 0;
static Vec<Str> g_menu_items;
static int g_menu_pick = -1;            // この回に選ばれた番号
static Str g_menu_owner;                // 入力欄が出したメニューなら、その名札





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
  if (has_ime()) platform().screen->text_input(false, 0, 0, 0, 0);
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

static void input_frame(int x, int y, int h, const Str& value, Str* conf, Str* marked) {
  g_input_want = true;
  marked->clear();
  if (has_ime()) {
    const PlatformScreen* sc = platform().screen;
    // 呼んだ側が中身を変えていたら、渡し直す
    bool reseed = !g_input_on || !(value == g_input_seed);
    sc->text_input(true, reseed ? value.c_str() : 0, x, y, h);
    g_input_on = true;
    Str c;
    if (sc->text_state(&c, marked)) {
      *conf = c;
      g_input_seed = c;
      g_marked = *marked;
      return;
    }
    *conf = value;
    g_input_seed = value;
    return;
  }
  // 変換を持たない出し先（端末など）。ここで数えて動かす。
  // 端末では、変換はその端末のソフトがやって、確定した文字だけが ui.typed() に届く
  Str next = value;
  int n = utf8_len(next);
  if (g_caret > n) g_caret = n;
  if (g_anchor > n) g_anchor = n;
  bool shift = g_key[SKEY_Shift];
  if (g_press[SKEY_Left] && g_caret > 0) { g_caret--; if (!shift) g_anchor = g_caret; }
  if (g_press[SKEY_Right] && g_caret < n) { g_caret++; if (!shift) g_anchor = g_caret; }
  if (g_press[SKEY_Home]) { g_caret = 0; if (!shift) g_anchor = 0; }
  if (g_press[SKEY_End]) { g_caret = n; if (!shift) g_anchor = n; }
  int st = 0, ln = 0;
  sel_get(next, &st, &ln);
  if (g_press[SKEY_Back]) {
    if (ln > 0) next = sel_replace(next, Str());
    else if (st > 0) { sel_set(next, st - 1, 1); next = sel_replace(next, Str()); }
  } else if (g_press[SKEY_Delete]) {
    if (ln > 0) next = sel_replace(next, Str());
    else if (st < n) { sel_set(next, st, 1); next = sel_replace(next, Str()); }
  }
  if (g_typed.size() > 0) next = sel_replace(next, g_typed);
  *conf = next;
  g_input_seed = next;
  g_marked.clear();
}

// 入力欄が受け取った打鍵を、その回のぶんとして使い切る。
// 同じ回にもう一度描くとき、同じ字がもう一度入らないようにする
static void take_input() {
  g_typed.clear();
  g_press[SKEY_Back] = false;
  g_press[SKEY_Delete] = false;
  g_press[SKEY_Left] = false;
  g_press[SKEY_Right] = false;
  g_press[SKEY_Home] = false;
  g_press[SKEY_End] = false;
}

static NativeStatus u_poll(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  // 前の刻みの「今このとき」を消す
  for (int i = 0; i < SKEY_Max; i++) { g_press[i] = false; g_rel[i] = false; g_hit[i] = false; }
  for (int i = 0; i < 3; i++) { g_mpress[i] = false; g_mrel[i] = false; }
  g_typed.clear();

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
      }
    }
    // 離した合図が来ない機種（端末）では、押された刻みだけ押されているとみなす
    if (!s->has_key_up) {
      for (int i = 0; i < SKEY_Max; i++)
        if (g_key[i] && !g_hit[i]) { g_key[i] = false; g_rel[i] = true; }
    }
  }
  // 前の回に誰も文字入力を求めなかったら、受け付けを止める
  if (!g_input_want) input_stop();
  g_input_want = false;

  out = mk_bool(!g_quit);
  return N_Ok;
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
static NativeStatus u_to_png(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  if (!need_open(vm)) return N_Panic;
  Str png;
  const char sig[8] = {(char)0x89, 'P', 'N', 'G', '\r', '\n', (char)0x1a, '\n'};
  png.append(sig, 8);

  Str ihdr;
  be32(ihdr, (uint32_t)g_w);
  be32(ihdr, (uint32_t)g_h);
  ihdr.push(8);   // 1色 8 ビット
  ihdr.push(2);   // 赤緑青（透過なし）
  ihdr.push(0); ihdr.push(0); ihdr.push(0);
  chunk(png, "IHDR", ihdr);

  // 各行の頭に「前の行との差の取り方」を置く。0 はそのまま
  Str raw;
  raw.reserve((g_w * 3 + 1) * g_h);
  for (int y = 0; y < g_h; y++) {
    raw.push(0);
    const uint32_t* row = g_px + (size_t)y * (size_t)g_w;
    for (int x = 0; x < g_w; x++) {
      raw.push((char)((row[x] >> 16) & 0xff));
      raw.push((char)((row[x] >> 8) & 0xff));
      raw.push((char)(row[x] & 0xff));
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

  out = mk_bytes(png);
  return N_Ok;
}

// ------------------------------------------------------------------ フォント
// 内蔵の字形は ASCII しか持たない。日本語などを出したいときは、
// 機種のフォントを読む（FreeType が要る → README「日本語の字を出す」）。
//
// 読むかどうかは**書く人が決める**。何もしなければ内蔵のままで、
// そのぶん、どの機種でも同じ大きさ・同じ形で出る（spec/library/ui.md）。

#if defined(SHARK_FREETYPE)

// 機種にありそうなフォント。上から順に見て、最初に開けたものを使う
static const char* const kFontPaths[] = {
#if defined(__APPLE__)
    // W4 が本文の太さ（Regular くらい）。W3 は細く見えるので後ろに置く
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/AppleSDGothicNeo.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
#elif defined(_WIN32)
    "C:\\Windows\\Fonts\\meiryo.ttc",
    "C:\\Windows\\Fonts\\YuGothM.ttc",
    "C:\\Windows\\Fonts\\msgothic.ttc",
    "C:\\Windows\\Fonts\\arial.ttf",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
    "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
    "/usr/share/fonts/ipa-gothic/ipag.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
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

static bool load_font_path(const char* path, int px) {
  char* data = 0;
  size_t n = 0;
  if (!read_font_file(path, &data, &n)) return false;
  bool ok = ft::load(data, n, px, Str(path));
  free(data);
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

// ui.font(大きさ) — 機種のフォントを使う
static NativeStatus u_font_size(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int px = (int)A(a, 0)->i;
#if defined(SHARK_FREETYPE)
  if (ft::active() && ft::resize(px)) { out = mk_bool(true); return N_Ok; }
#endif
  out = mk_bool(load_font_auto(px));
  return N_Ok;
}
// ui.font(場所, 大きさ)
static NativeStatus u_font_path(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_bool(load_font_path(as_str(*A(a, 0))->s.c_str(), (int)A(a, 1)->i));
  return N_Ok;
}
// ui.font(中身, 大きさ) — 自分で読んだものを渡す
static NativeStatus u_font_bytes(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
#if defined(SHARK_FREETYPE)
  const Str& d = as_str(*A(a, 0))->s;
  out = mk_bool(ft::load(d.data(), (size_t)d.size(), (int)A(a, 1)->i, Str("(bytes)")));
#else
  (void)a;
  out = mk_bool(false);
#endif
  return N_Ok;
}
static NativeStatus u_font_builtin(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
#if defined(SHARK_FREETYPE)
  ft::unload();
#endif
  out = mk_void();
  return N_Ok;
}
static NativeStatus u_font_name(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
#if defined(SHARK_FREETYPE)
  if (ft::active()) { out = mk_str(ft::name()); return N_Ok; }
#endif
  out = mk_str(Str());
  return N_Ok;
}

// ------------------------------------------------------------ 宣言的な層
// 「今どうあるべきか」を **Widget の配列にして返す**（spec/library/ui.md）。
// 呼び出しにブロックを続ける記法は言語に無いので、入れ子は配列で表す。
//
//   func view(count: int) -> list<Widget> {
//     return [ui.label(f"{count} 回"), ui.button("押す", "inc")];
//   }
//   var hit = ui.show(view(count));     // 描いて、押されたものの名札が返る
//
// 部品は毎回作り直され、状態は持たない。押されたかどうかだけを名札で返し、
// 値は呼んだ側が持つ。だから「今の状態」と「画面」がずれない。
enum WidgetKind {
  WK_Label = 0, WK_Button, WK_Checkbox, WK_Slider, WK_Field, WK_Space, WK_Column, WK_Row,
  WK_Divider
};
enum WidgetField {
  WF_Kind = 0, WF_Text, WF_Id, WF_A, WF_B, WF_C, WF_Kids,
  WF_Fg, WF_Bg, WF_Pad, WF_Wid, WF_Hei, WF_WidFr, WF_HeiFr, WF_Align, WF_Act, WF_Count
};
enum WidgetAlign { WA_Left = 0, WA_Center, WA_Right };

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

static NativeStatus widget_with(VM& vm, Value* a, int field, int64_t value, Value& out) {
  InstObj* o = widget_copy(vm, a, out);
  if (!o) return N_Panic;
  widget_put(o, field, mk_int(value));
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
static int64_t w_field(const Value& v, int i) {
  InstObj* o = as_inst(v);
  return i < o->fields.size() ? o->fields[i].i : 0;
}
// 入力欄が書き戻す先の var の番号（-1 なら名札で受ける形）
static int w_var(const Value& v) { return w_kind(v) == WK_Field ? (int)w_a(v) : -1; }
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
static uint32_t w_fg(const Value& v) {
  int64_t c = w_field(v, WF_Fg);
  return c < 0 ? g_fg : (uint32_t)c;
}

// --- 見た目の既定値（spec/library/ui.md）---------------------------------
// 部品の寸法は、**そのときの字の高さ**から決める。
// フォントを変えても、HiDPI で字を大きくしても、見た目の釣り合いが崩れない。
// 内蔵の 5×7（高さ 8）のときに、下の数がそれぞれ 4・6・5・3・96・120 になる
static int ui_unit() {
  int h = kCellH;   // 内蔵の 5×7 のとき
#if defined(SHARK_FREETYPE)
  if (ft::active()) h = ft::size();   // フォントの大きさ（行送りではなく字の大きさ）
#endif
  return h < 6 ? 6 : h;
}
static int gap_y() { return ui_unit() / 2; }          // 縦に並べたときの間
static int gap_x() { return ui_unit() * 3 / 4; }      // 横に並べたときの間
static int pad_x() { return ui_unit() * 5 / 8; }      // ボタンの内側の余白（横）
static int pad_y() { return ui_unit() * 3 / 8; }      // 同じく縦
static int slide_w() { return ui_unit() * 12; }       // つまみの長さ
static int field_pad_x() {                            // 入力欄の内側の余白
  int n = ui_unit() / 3;
  return n < 3 ? 3 : n;
}
static int field_pad_y() {
  int n = ui_unit() / 6;
  return n < 2 ? 2 : n;
}
static int field_w() { return ui_unit() * 15; }       // 入力欄の長さ
// チェックの四角
static int box_w() {
  int h = ui_unit() - 2;
  return h < 8 ? 8 : h;
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
static Str g_hit_text;
static bool g_click_seen = false;   // この回にどこかが押されたか（焦点を外すのに使う）
static Str g_focus_next;
static bool g_edited = false;      // この回、ref で受けた部品が変数を書き換えたか
// ui.show() を呼んだ処理系。ref で受けた入力欄の書き戻しに使う（下の write_var）
static VM* g_vm = 0;

static void clear_hit_action() {
  val_release(g_hit_action);
  g_hit_action = mk_void();
}

// 押されたことを覚える。名札と、持っていれば関数も
static void hit(const Value& v, int64_t val) {
  g_hit_id = w_id(v);
  g_hit_val = val;
  g_hit_any = true;
  clear_hit_action();
  if (Value* a = w_act(v)) g_hit_action = val_retain(*a);
}

static void ui_reset_widgets() {
  clear_hit_action();
  g_hit_any = false;
  g_edited = false;
  g_vm = 0;
  g_menu_on = false;
  g_menu_items.clear();
  g_menu_owner.clear();
  g_menu_pick = -1;
  g_caret = g_anchor = g_scroll = g_drag_anchor = 0;
  g_dragging = false;
  g_focus.clear();
  g_hit_id.clear();
  g_hit_text.clear();
  g_hit_val = 0;
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

static Box measure(const Value& v);

// 中身そのものの大きさ（余白も指定も入れない）
static Box intrinsic(const Value& v) {
  Box b;
  b.w = 0;
  b.h = 0;
  switch (w_kind(v)) {
    case WK_Label:
      b.w = text_px_width(w_text(v), 1);
      b.h = line_h(1) + (text_lines(w_text(v)) - 1) * line_pitch(1);
      break;
    case WK_Button:
      b.w = text_px_width(w_text(v), 1) + pad_x() * 2;
      b.h = line_h(1) + pad_y() * 2;
      break;
    case WK_Checkbox:
      b.w = box_w() + 4 + text_px_width(w_text(v), 1);
      b.h = box_w();
      break;
    case WK_Slider: b.w = slide_w(); b.h = line_h(1) + 2; break;
    case WK_Field: b.w = field_w(); b.h = line_h(1) + field_pad_y() * 2; break;
    case WK_Divider: b.w = 0; b.h = ui_unit() / 2 + 1; break;   // 幅は置くときに決まる
    case WK_Space: b.h = (int)w_a(v); break;
    case WK_Column: {
      ListObj* k = w_kids(v);
      for (int i = 0; i < k->v.size(); i++) {
        Box c = measure(k->v[i]);
        if (c.w > b.w) b.w = c.w;
        b.h += c.h;
        if (i + 1 < k->v.size()) b.h += gap_y();
      }
      break;
    }
    case WK_Row: {
      ListObj* k = w_kids(v);
      for (int i = 0; i < k->v.size(); i++) {
        Box c = measure(k->v[i]);
        b.w += c.w;
        if (c.h > b.h) b.h = c.h;
        if (i + 1 < k->v.size()) b.w += gap_x();
      }
      break;
    }
    default: break;
  }
  return b;
}

// 外から見た大きさ。内側の余白（padding）と、決め打ちの幅・高さを入れる
static Box measure(const Value& v) {
  Box b = intrinsic(v);
  int p = w_pad(v);
  b.w += p * 2;
  b.h += p * 2;
  if (w_wid(v) > 0) b.w = w_wid(v);
  if (w_hei(v) > 0) b.h = w_hei(v);
  return b;
}

// --- 取り分（fr）を配る ---------------------------------------------------
// 中に、その向きの取り分を書いた子がいるか（ui.row / ui.column だけ）
static bool kids_want_fr(const Value& v, bool horiz) {
  int k = w_kind(v);
  if (k != WK_Row && k != WK_Column) return false;
  ListObj* kids = w_kids(v);
  for (int i = 0; i < kids->v.size(); i++) if (w_fr(kids->v[i], horiz) > 0) return true;
  return false;
}


// 縦か横に並べたときの、子ひとりひとりの大きさ。
// 取り分を書いていない子は中身の大きさのまま。**余った場所**を、書いた数の比で分ける。
//
//   ui.row([a.width(1.0), b.width(2.0)])   余りを 1:2 で分ける
//   ui.row([a, b.width(float.infinity())]) a は中身の大きさ、b が残り全部
//
// float.infinity()（余りぜんぶ）が混じっている並びでは、ふつうの取り分は
// 中身の大きさに戻り、余りは infinity を書いた子どうしで等分する
static void axis_sizes(ListObj* k, bool horiz, int avail, int gap, Vec<int>& out) {
  int n = k->v.size();
  out.clear();
  bool fill = false;
  for (int i = 0; i < n; i++) if (fr_is_fill(w_fr(k->v[i], horiz))) fill = true;

  double wsum = 0;
  int fixed = n > 1 ? gap * (n - 1) : 0;
  for (int i = 0; i < n; i++) {
    Box b = measure(k->v[i]);
    out.push(horiz ? b.w : b.h);
    double f = w_fr(k->v[i], horiz);
    if (f > 0 && (!fill || fr_is_fill(f))) wsum += fill ? 1.0 : f;
    else fixed += out[i];              // 大きさの決まっている子。ここは配らない
  }
  if (wsum <= 0) return;               // 取り分を書いた子がいない

  double left = avail - fixed;
  if (left < 0) left = 0;
  double acc = 0;
  int done = 0;
  for (int i = 0; i < n; i++) {
    double f = w_fr(k->v[i], horiz);
    if (!(f > 0 && (!fill || fr_is_fill(f)))) continue;
    acc += fill ? 1.0 : f;
    // ここまでに配る量から数えることで、丸めの誤差がたまらない（合計は必ず left）
    int upto = (int)(left * acc / wsum + 0.5);
    out[i] = upto - done;
    done = upto;
  }
}

// --- 描いて、触られたかを見る ---------------------------------------------
// 親がくれる場所（avail_w × avail_h）の中に置く。取り分（fr）を書いた向きは、
// 親がそこに配ったぶんがそのまま渡ってくる（下の axis_sizes）
static void place(const Value& v, int x, int y, int avail_w, int avail_h);

static void place_button(const Value& v, int x, int y, const Box& b) {
  bool over = inside(x, y, b.w, b.h);
  int64_t bg = w_field(v, WF_Bg);
  uint32_t face = bg >= 0 ? (uint32_t)bg : blend(g_bg, g_accent, over ? 0.55 : 0.3);
  if (bg >= 0 && over) face = blend(face, g_fg, 0.2);
  uint32_t edge = blend(g_bg, g_accent, over ? 1.0 : 0.7);
  for (int i = 0; i < b.h; i++) span(x, y + i, b.w, face);
  span(x + 1, y, b.w - 2, edge);
  span(x + 1, y + b.h - 1, b.w - 2, edge);
  for (int i = 1; i < b.h - 1; i++) { put(x, y + i, edge); put(x + b.w - 1, y + i, edge); }
  // 決め打ちで広げたときは、ラベルを真ん中に置く
  int tw = text_px_width(w_text(v), 1);
  put_text(x + (b.w - tw) / 2, y + (b.h - line_h(1)) / 2, w_text(v), 1, w_fg(v));
  if (over && g_mpress[0]) hit(v, 1);
}

static void place_checkbox(const Value& v, int x, int y, const Box& b) {
  bool on = w_a(v) != 0;
  bool over = inside(x, y, b.w, b.h);
  uint32_t edge = blend(g_bg, g_accent, over ? 1.0 : 0.7);
  int bw = box_w();
  for (int i = 0; i < bw; i++) span(x, y + i, bw, blend(g_bg, g_accent, over ? 0.35 : 0.2));
  span(x, y, bw, edge);
  span(x, y + bw - 1, bw, edge);
  for (int i = 0; i < bw; i++) { put(x, y + i, edge); put(x + bw - 1, y + i, edge); }
  if (on) {   // 中の印。四角の大きさに合わせて引く
    int n = bw / 4;
    if (n < 2) n = 2;
    for (int i = 0; i < n; i++) put(x + bw / 4 + i, y + bw / 2 + i, g_fg);
    for (int i = 0; i < n + 1; i++) put(x + bw / 4 + n + i, y + bw / 2 + n - i, g_fg);
  }
  put_text(x + bw + 4, y + (bw - line_h(1)) / 2, w_text(v), 1, w_fg(v));
  if (over && g_mpress[0]) hit(v, on ? 0 : 1);
}

static void place_slider(const Value& v, int x, int y, const Box& b) {
  int64_t lo = w_b(v), hi = w_c(v), val = w_a(v);
  if (hi <= lo) hi = lo + 1;
  if (val < lo) val = lo;
  if (val > hi) val = hi;
  int track_y = y + b.h / 2 - 1;
  int tw = b.w, knob = box_w() / 2 + 1;
  span(x, track_y, tw, blend(g_bg, g_fg, 0.35));
  span(x, track_y + 1, tw, blend(g_bg, g_fg, 0.35));
  int usable = tw - knob;
  int kx = x + (int)((val - lo) * usable / (hi - lo));
  for (int i = 0; i < b.h; i++) span(kx, y + i, knob, g_accent);
  // 押されている間は、そのつど今の位置から値を出す
  if (g_mb[0] && inside(x - 3, y - 3, tw + 6, b.h + 6)) {
    int rel = g_mx - x - knob / 2;
    if (rel < 0) rel = 0;
    if (rel > usable) rel = usable;
    int64_t nv = lo + (int64_t)rel * (hi - lo) / usable;
    hit(v, nv);
  }
}


// --- 右で押したときのメニュー ---------------------------------------------
// 出すのはこちら（面に描く）。どの出し先でも同じに出る
static void menu_open_at(int x, int y, const Vec<Str>& items, const Str& owner) {
  g_menu_on = items.size() > 0;
  g_menu_x = x;
  g_menu_y = y;
  g_menu_items = items;
  g_menu_owner = owner;
}
static int menu_item_h() { return line_h(1) + pad_y() * 2; }
static int menu_w() {
  int w = 0;
  for (int i = 0; i < g_menu_items.size(); i++) {
    int t = text_px_width(g_menu_items[i], 1);
    if (t > w) w = t;
  }
  return w + pad_x() * 4;
}
// ui.show の頭で呼ぶ。メニューが出ているあいだの押しは、**下の部品には渡さない**
static void menu_hit() {
  g_menu_pick = -1;
  if (!g_menu_on) return;
  int w = menu_w(), ih = menu_item_h(), h = ih * g_menu_items.size();
  if (g_mpress[0]) {
    if (inside(g_menu_x, g_menu_y, w, h)) {
      int i = (g_my - g_menu_y) / ih;
      if (i >= 0 && i < g_menu_items.size()) g_menu_pick = i;
    }
    g_menu_on = false;
    g_mpress[0] = false;   // 選んだ／閉じた押しは、ここで飲み込む
    g_mb[0] = false;
  } else if (g_mpress[2]) {
    g_menu_on = false;     // 右で押し直したら、いったん閉じる
  }
}
// ui.show の終わりで呼ぶ。いちばん上に描く
static void menu_draw() {
  if (!g_menu_on) return;
  int w = menu_w(), ih = menu_item_h(), h = ih * g_menu_items.size();
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
  for (int i = 0; i < g_menu_items.size(); i++) {
    int iy = y + i * ih;
    if (inside(x, iy, w, ih))
      for (int k = 1; k < ih - 1; k++) span(x + 1, iy + k, w - 2, blend(g_bg, g_accent, 0.45));
    put_text(x + pad_x() * 2, iy + pad_y(), g_menu_items[i], 1, g_fg);
  }
}

// マウスの位置が、何文字目にあたるか（from 文字目から右へ数える）
static int char_at_x(const Str& s, int from, int x0, int mx) {
  int n = utf8_len(s);
  int at = utf8_offset(s, from), cx = x0, i = from;
  while (i < n) {
    int cp = 0;
    int adv = utf8_decode(s, at, &cp);
    if (adv <= 0) break;
    int w = advance_of(cp, 1);
    if (mx < cx + w / 2) break;
    cx += w;
    at += adv;
    i++;
  }
  return i;
}

// 入力欄の中で、右で押したときに出すもの
static void field_menu(int x, int y, const Str& id) {
  Vec<Str> items;
  items.push(Str(g_lang_ja ? "コピー" : "Copy"));
  items.push(Str(g_lang_ja ? "切り取り" : "Cut"));
  items.push(Str(g_lang_ja ? "貼り付け" : "Paste"));
  items.push(Str(g_lang_ja ? "すべて選ぶ" : "Select All"));
  menu_open_at(x, y, items, id);
}

static void place_field(const Value& v, int x, int y, const Box& b) {
  Str text = w_text(v);
  Str id = w_id(v);
  bool over = inside(x, y, b.w, b.h);
  bool focused = g_focus.size() > 0 && g_focus == id;
  int tx = x + field_pad_x(), ty = y + field_pad_y();
  int room = b.w - field_pad_x() * 2;

  // --- 押された・なぞられた ---
  if (over && g_mpress[0]) {
    g_focus_next = id;
    if (!focused) { g_scroll = 0; g_anchor = g_caret = utf8_len(text); }
    int i = char_at_x(text, g_scroll, tx, g_mx);
    sel_set(text, i, 0);
    g_drag_anchor = i;
    g_dragging = true;
  } else if (over && g_mpress[2]) {   // 右で押したら、切り貼りのメニュー
    g_focus_next = id;
    field_menu(g_mx, g_my, id);
  }
  if (g_dragging && focused && g_mb[0]) {
    int j = char_at_x(text, g_scroll, tx, g_mx);
    int a = g_drag_anchor < j ? g_drag_anchor : j;
    int c = g_drag_anchor < j ? j : g_drag_anchor;
    sel_set(text, a, c - a);
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
  }

  // --- 文字を受け取る ---
  Str marked;
  if (focused) {
    bool was_composing = g_marked.size() > 0;
    Str conf;
    input_frame(tx, ty, b.h - field_pad_y() * 2, text, &conf, &marked);
    if (g_press[SKEY_Enter] && !was_composing) g_focus_next.clear();
    text = conf;
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
  int caret = st + ln;
  if (!focused) g_scroll = 0;
  else {
    int n = utf8_len(text);
    if (g_scroll > n) g_scroll = 0;
    if (caret < g_scroll) g_scroll = caret;
    while (g_scroll < caret &&
           text_px_width(sub_chars(text, g_scroll, caret - g_scroll), 1) + 2 > room)
      g_scroll++;
  }
  int scroll = focused ? g_scroll : 0;

  // --- 描く ---
  uint32_t edge = blend(g_bg, focused ? g_accent : g_fg, focused ? 1.0 : 0.45);
  int64_t fbg = w_field(v, WF_Bg);
  for (int i = 0; i < b.h; i++)
    span(x, y + i, b.w, fbg >= 0 ? (uint32_t)fbg : blend(g_bg, g_fg, 0.08));
  span(x, y, b.w, edge);
  span(x, y + b.h - 1, b.w, edge);
  for (int i = 0; i < b.h; i++) { put(x, y + i, edge); put(x + b.w - 1, y + i, edge); }

  // 選んでいるところに帯を敷く
  if (focused && ln > 0) {
    int a = st < scroll ? scroll : st;
    int ax = tx + text_px_width(sub_chars(text, scroll, a - scroll), 1);
    int bx = tx + text_px_width(sub_chars(text, scroll, st + ln - scroll), 1);
    if (bx > x + b.w - field_pad_x()) bx = x + b.w - field_pad_x();
    if (bx > ax)
      for (int i = 2; i < b.h - 2; i++) span(ax, y + i, bx - ax, blend(g_bg, g_accent, 0.5));
  }

  Str shown = sub_chars(text, scroll, utf8_len(text) - scroll);
  if (focused && marked.size() > 0) shown += marked;
  put_text(tx, ty, shown, 1, w_fg(v));

  if (focused) {
    int cx = tx + text_px_width(sub_chars(text, scroll, caret - scroll), 1);
    if (marked.size() > 0) {   // 変換中のところに下線
      int mw = text_px_width(marked, 1);
      span(cx, y + b.h - field_pad_y(), mw, g_accent);
      cx += mw;
    }
    if (ln == 0 || marked.size() > 0)
      for (int i = 2; i < b.h - 2; i++) put(cx, y + i, g_accent);
  }
}

// 横に並べたとき、背の低い部品を真ん中に置く（つまみと文字が並んだときに揃う）
static int yy_of_row(const Value& row, int cy, int ch, int h) {
  (void)row;
  return ch > h ? cy + (ch - h) / 2 : cy;
}

static void place(const Value& v, int x, int y, int avail_w, int avail_h) {
  Box b = measure(v);
  int w = b.w, h = b.h;
  // 取り分（fr）を書いた向きは、親が配ってくれたぶんいっぱいに広がる
  if (w_fr(v, true) > 0 && avail_w > 0) w = avail_w;
  if (w_fr(v, false) > 0 && avail_h > 0) h = avail_h;
  // 取り分を書いた子がいる入れ物も、置ける場所いっぱいに広がる。
  // 中身の大きさのままだと、子に配る余りがそもそも無いため
  if (w_wid(v) <= 0 && w_fr(v, true) <= 0 && avail_w > w && kids_want_fr(v, true)) w = avail_w;
  if (w_hei(v) <= 0 && w_fr(v, false) <= 0 && avail_h > h && kids_want_fr(v, false)) h = avail_h;
  // 区切り線は、置ける幅いっぱいに引く
  if (w_kind(v) == WK_Divider && w_wid(v) <= 0) w = avail_w;

  // 寄せ方は「親がくれた幅の中で、自分をどこに置くか」
  int al = w_align(v);
  if (avail_w > w) {
    if (al == WA_Center) x += (avail_w - w) / 2;
    else if (al == WA_Right) x += avail_w - w;
  }

  int64_t bg = w_field(v, WF_Bg);
  if (bg >= 0 && w_kind(v) != WK_Button && w_kind(v) != WK_Field)
    for (int i = 0; i < h; i++) span(x, y + i, w, (uint32_t)bg);

  int p = w_pad(v);
  int cx = x + p, cy = y + p, cw = w - p * 2, ch = h - p * 2;
  Box cb;
  cb.w = cw;
  cb.h = ch;

  switch (w_kind(v)) {
    case WK_Label: put_text(cx, cy, w_text(v), 1, w_fg(v)); break;
    case WK_Button: place_button(v, cx, cy, cb); break;
    case WK_Checkbox: place_checkbox(v, cx, cy, cb); break;
    case WK_Slider: place_slider(v, cx, cy, cb); break;
    case WK_Field: place_field(v, cx, cy, cb); break;
    case WK_Space: break;
    case WK_Divider: {
      uint32_t c = w_field(v, WF_Fg) >= 0 ? w_fg(v) : blend(g_bg, g_fg, 0.3);
      span(cx, cy + ch / 2, cw, c);
      break;
    }
    case WK_Column: {
      ListObj* k = w_kids(v);
      Vec<int> hs;
      axis_sizes(k, false, ch, gap_y(), hs);   // 縦に並べる。余りは高さの取り分へ
      int yy = cy;
      for (int i = 0; i < k->v.size(); i++) {
        place(k->v[i], cx, yy, cw, hs[i]);
        yy += hs[i] + gap_y();
      }
      break;
    }
    case WK_Row: {
      ListObj* k = w_kids(v);
      Vec<int> ws;
      axis_sizes(k, true, cw, gap_x(), ws);    // 横に並べる。余りは幅の取り分へ
      int xx = cx;
      for (int i = 0; i < k->v.size(); i++) {
        // 高さの取り分を書いた子は、並びの高さいっぱいに伸びる
        int hh = w_fr(k->v[i], false) > 0 ? ch : measure(k->v[i]).h;
        place(k->v[i], xx, yy_of_row(v, cy, ch, hh), ws[i], hh);
        xx += ws[i] + gap_x();
      }
      break;
    }
    default: break;
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
  return make_widget(vm, WK_Field, as_str(*p)->s, var_field_id(slot), slot, 0, 0, 0, out)
             ? N_Ok : N_Panic;
}
static NativeStatus u_space(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Space, Str(), Str(), A(a, 0)->i, 0, 0, 0, out) ? N_Ok : N_Panic;
}
static NativeStatus u_column(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Column, Str(), Str(), 0, 0, 0, A(a, 0), out) ? N_Ok : N_Panic;
}
static NativeStatus u_row(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  return make_widget(vm, WK_Row, Str(), Str(), 0, 0, 0, A(a, 0), out) ? N_Ok : N_Panic;
}
// 中身をまとめて、置ける幅の真ん中に置く（ui.column(...).align("center") と同じ）
static NativeStatus u_center(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!make_widget(vm, WK_Column, Str(), Str(), 0, 0, 0, A(a, 0), out)) return N_Panic;
  InstObj* o = as_inst(out);
  val_release(o->fields[WF_Align]);
  o->fields[WF_Align] = mk_int(WA_Center);
  return N_Ok;
}
// 区切り線。置ける幅いっぱいに引く
static NativeStatus u_divider(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  return make_widget(vm, WK_Divider, Str(), Str(), 0, 0, 0, 0, out) ? N_Ok : N_Panic;
}

// --- 出す -----------------------------------------------------------------
static NativeStatus show_at(VM& vm, Value* a, int x, int y, Value& out) {
  if (!need_open(vm)) return N_Panic;
  ListObj* view = as_list(*A(a, 0));
  g_lang_ja = (vm.lang() == LANG_JA);
  menu_hit();          // メニューが出ていれば、押しはそちらが先に受ける
  g_hit_id.clear();
  g_hit_text.clear();
  g_hit_val = 0;
  g_hit_any = false;
  g_edited = false;
  g_vm = &vm;          // ref で受けた入力欄の書き戻しに使う
  clear_hit_action();
  g_click_seen = g_mpress[0];
  g_focus_next = g_focus;
  if (g_click_seen) g_focus_next.clear();   // どこも押されなければ焦点は外れる

  // 使える場所は、置き始めたところから見て、面の中に残っているぶん。
  // 高さも同じように数えるので、いちばん外側でも取り分（fr）が使える
  int avail = g_w - x * 2;
  if (avail < 1) avail = g_w > x ? g_w - x : 1;
  int avail_h = g_h - y * 2;
  if (avail_h < 1) avail_h = g_h > y ? g_h - y : 1;
  Vec<int> hs;
  axis_sizes(view, false, avail_h, gap_y(), hs);   // 縦に並べる
  int cy = y;
  for (int i = 0; i < view->v.size(); i++) {
    place(view->v[i], x, cy, avail, hs[i]);
    cy += hs[i] + gap_y();
  }
  g_focus = g_focus_next;
  // 押しは受け取った部品が使い切る。こうすると、同じ回にもう一度 ui.show() しても
  // 二度は効かない（ui.run が、状態を変えたあとすぐ描き直すのに要る）
  if (g_hit_any) { g_mpress[0] = false; g_mpress[2] = false; }
  menu_draw();         // いちばん上に描く
  out = mk_str(g_hit_id);
  return N_Ok;
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
  r.add("ui.close", u_close, tv);
  r.add("ui.width", u_width, ti);
  r.add("ui.height", u_height, ti);
  r.add("ui.visible", u_visible, tb);
  r.add("ui.scale", u_scale, ti);
  r.add("ui.present", u_present, tv);

  r.add("ui.rgb", u_rgb, ti, ti, ti, ti);
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

  r.add("ui.text", u_text, tv, ti, ti, ts, ti);
  r.add("ui.text", u_text_scaled, tv, ti, ti, ts, ti, ti);
  r.add("ui.text_width", u_text_width, ti, ts);
  r.add("ui.text_width", u_text_width, ti, ts, ti);
  r.add("ui.text_height", u_text_height, ti);
  r.add("ui.text_height", u_text_height, ti, ti);

  r.add("ui.poll", u_poll, tb);
  r.add("ui.quit", u_quit, tv);
  r.add("ui.key", u_key, tb, ts);
  r.add("ui.pressed", u_pressed, tb, ts);
  r.add("ui.released", u_released, tb, ts);
  r.add("ui.typed", u_typed, ts);
  r.add("ui.mouse_x", u_mouse_x, ti);
  r.add("ui.mouse_y", u_mouse_y, ti);
  r.add("ui.mouse", u_mouse, tb, ti);
  r.add("ui.clicked", u_clicked, tb, ti);
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

  r.add("ui.to_png", u_to_png, tby);

  r.add("ui.font", u_font_size, tb, ti);
  r.add("ui.font", u_font_path, tb, ts, ti);
  r.add("ui.font", u_font_bytes, tb, tby, ti);
  r.add("ui.font_builtin", u_font_builtin, tv);
  r.add("ui.font_name", u_font_name, ts);

  // 宣言的な層（Widget を配列にして返す）。
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
  r.add("ui.space", u_space, tw, ti);
  r.add("ui.column", u_column, tw, tlw);
  r.add("ui.row", u_row, tw, tlw);
  r.add("ui.center", u_center, tw, tlw);
  r.add("ui.divider", u_divider, tw);
  Vec<Type*> no_params;
  Type* tact = t.func_type(no_params, tv);   // func() -> void
  r.add("ui.button", u_button_fn, tw, ts, tact);
  r.add("ui.checkbox", u_checkbox_fn, tw, ts, tact, tb);
  r.add("ui.slider", u_slider_fn, tw, tact, ti, ti, ti);
  r.add("ui.edited", u_edited, tb);
  r.add("ui.has_action", u_has_action, tb);
  r.add("ui.action", u_action, tact);
  r.add("ui.show", u_show, ts, tlw);
  r.add("ui.show", u_show_at, ts, tlw, ti, ti);
  r.add("ui.value", u_value, ti);
  r.add("ui.text_value", u_text_value, ts);
  r.add("ui.theme", u_theme, tv, ti, ti, ti);
  r.enable_module("std.ui");
}

}  // namespace shark

// font_ft.inc — FreeType で字を描く（core/lib/ui.cpp から読む）
//
// **ここだけ外部ライブラリに頼る。**日本語などの字形を自前で抱えるのは重すぎるため
// （spec/library/ui.md の「文字」）。
//
//   ・入っていなければ、この中身は丸ごと消える（内蔵の 5×7 だけになる）
//   ・使うかどうかは書く人が決める。ui.font() を呼ぶまでは内蔵の字形のまま
//   ・字形は「濃さ」で返ってくるので、下地と混ぜて描く（縁がなめらかになる）
//
// 作り方は README の「日本語の字を出す」。
#if defined(SHARK_FREETYPE)

#include <ft2build.h>
#include FT_FREETYPE_H

namespace ft {

struct Glyph {
  int cp, px;              // 鍵。cp が -1 なら空き
  short w, h, left, top;   // 濃さの並びの大きさと、置く場所（left/top は基準線から）
  short adv;               // 次の字までの幅
  unsigned char* bits;     // w×h の濃さ（0〜255）
};

static FT_Library g_lib = 0;
static FT_Face g_face = 0;
// 字の中身。FreeType は読み終わるまでここを見続けるので、こちらで持つ。
// これは処理系の持ちものであって、プログラムのメモリではない。
// 上限（--memory）に数えないよう malloc を使う（日本語のフォントは数 MB ある）
static unsigned char* g_data = 0;
static size_t g_data_n = 0;
static Str g_name;      // どこから読んだか
static int g_px = 14;   // いまの大きさ
static int g_set_px = 0;

// 字形の覚え書き。開番地法。いっぱいになったら丸ごと捨てて作り直す
static const int kCap = 1024;
static Glyph* g_cache = 0;
static int g_count = 0;

// 覚え書きは処理系の持ちもので、プログラムのメモリではない。
// 上限（--memory）に数えないよう、malloc をそのまま使う
static void cache_clear() {
  if (!g_cache) return;
  for (int i = 0; i < kCap; i++)
    if (g_cache[i].cp >= 0 && g_cache[i].bits) free(g_cache[i].bits);
  for (int i = 0; i < kCap; i++) { g_cache[i].cp = -1; g_cache[i].bits = 0; }
  g_count = 0;
}

static bool cache_ready() {
  if (g_cache) return true;
  g_cache = (Glyph*)malloc(sizeof(Glyph) * kCap);
  if (!g_cache) return false;
  for (int i = 0; i < kCap; i++) { g_cache[i].cp = -1; g_cache[i].bits = 0; }
  g_count = 0;
  return true;
}

static unsigned slot_of(int cp, int px) {
  unsigned h = (unsigned)cp * 2654435761u + (unsigned)px * 40503u;
  return h % (unsigned)kCap;
}

static void set_size(int px) {
  if (px == g_set_px) return;
  FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px);
  g_set_px = px;
}

static void unload() {
  cache_clear();
  if (g_cache) { free(g_cache); g_cache = 0; }
  if (g_face) { FT_Done_Face(g_face); g_face = 0; }
  if (g_data) { free(g_data); g_data = 0; g_data_n = 0; }
  g_name.clear();
  g_set_px = 0;
}

static bool active() { return g_face != 0; }
static const Str& name() { return g_name; }
static int size() { return g_px; }

// 中身（TTF / OTF / TTC）を渡して開く。開けなければ false。
// data はここで写し取るので、呼んだ側は持ち続けなくてよい
static bool load(const char* data, size_t n, int px, const Str& from) {
  if (!data || n == 0) return false;
  if (px < 4) px = 4;
  if (px > 400) px = 400;
  if (!g_lib && FT_Init_FreeType(&g_lib) != 0) return false;
  if (!cache_ready()) return false;
  unsigned char* copy = (unsigned char*)malloc(n);
  if (!copy) return false;
  sk_memcpy(copy, data, n);
  FT_Face face = 0;
  if (FT_New_Memory_Face(g_lib, (const FT_Byte*)copy, (FT_Long)n, 0, &face) != 0) {
    free(copy);
    return false;
  }
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) {
    FT_Done_Face(face);
    free(copy);
    return false;
  }
  cache_clear();
  if (g_face) FT_Done_Face(g_face);
  if (g_data) free(g_data);
  g_face = face;
  g_data = copy;
  g_data_n = n;
  g_name = from;
  g_px = px;
  g_set_px = px;
  return true;
}

// 大きさだけ変える
static bool resize(int px) {
  if (!g_face) return false;
  if (px < 4) px = 4;
  if (px > 400) px = 400;
  if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px) != 0) return false;
  g_px = px;
  g_set_px = px;
  return true;
}

static const Glyph* glyph(int cp, int px) {
  if (!g_face || !g_cache) return 0;
  unsigned at = slot_of(cp, px);
  for (int probe = 0; probe < kCap; probe++) {
    Glyph& e = g_cache[(at + (unsigned)probe) % (unsigned)kCap];
    if (e.cp == cp && e.px == px) return &e;
    if (e.cp >= 0) continue;
    // 空き。ここに作る
    if (g_count > kCap * 3 / 4) { cache_clear(); return glyph(cp, px); }
    set_size(px);
    if (FT_Load_Char(g_face, (FT_ULong)cp, FT_LOAD_RENDER) != 0) return 0;
    FT_GlyphSlot s = g_face->glyph;
    e.cp = cp;
    e.px = px;
    e.w = (short)s->bitmap.width;
    e.h = (short)s->bitmap.rows;
    e.left = (short)s->bitmap_left;
    e.top = (short)s->bitmap_top;
    e.adv = (short)(s->advance.x >> 6);
    e.bits = 0;
    int n = (int)e.w * (int)e.h;
    if (n > 0) {
      e.bits = (unsigned char*)malloc((size_t)n);
      if (!e.bits) { e.cp = -1; return 0; }
      for (int y = 0; y < e.h; y++)
        for (int x = 0; x < e.w; x++)
          e.bits[y * e.w + x] = s->bitmap.buffer[y * s->bitmap.pitch + x];
    }
    g_count++;
    return &e;
  }
  return 0;
}

// 1行の高さ。**字の上端から下端まで**（ascender − descender）で数える。
//
// FreeType の metrics.height は「次の行までの送り」で、行間のあきを含む。
// ヒラギノなら 24 画素の字で 36 もある。これを1行の高さとして使うと、
// 基準線から下に 11 画素ぶん**字の下にあきができる**。使わない。
static int line_height(int px) {
  if (!g_face) return px;
  set_size(px);
  int h = (int)((g_face->size->metrics.ascender - g_face->size->metrics.descender) >> 6);
  return h > 0 ? h : px;
}
static int ascender(int px) {
  if (!g_face) return px;
  set_size(px);
  return (int)(g_face->size->metrics.ascender >> 6);
}

}  // namespace ft

#endif  // SHARK_FREETYPE

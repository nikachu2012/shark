// font_ft.inc — FreeType で字を描く（core/lib/ui.cpp から読む）
//
// **ここだけ外部ライブラリに頼る。**日本語などの字形を自前で抱えるのは重すぎるため
// （spec/library/ui.md の「文字」）。
//
//   ・入っていなければ、この中身は丸ごと消える（内蔵の 5×7 だけになる）
//   ・使うかどうかは書く人が決める。ui.font() を呼ぶまでは内蔵の字形のまま
//   ・字形は「濃さ」で返ってくるので、下地と混ぜて描く（縁がなめらかになる）
//   ・本命のフォントに無い字は、**控えのフォント**から探す（下の「控え」）
//
// 作り方は README の「日本語の字を出す」。
#if defined(SHARK_FREETYPE)

// ヘッダ（ft2build.h / FT_FREETYPE_H）は ui.cpp が名前空間の外で読んでいる。

namespace ft {

struct Glyph {
  int cp, px;              // 鍵。cp が -1 なら空き
  short w, h, left, top;   // 濃さの並びの大きさと、置く場所（left/top は基準線から）
  short adv;               // 次の字までの幅
  unsigned char* bits;     // w×h の濃さ（0〜255）
};

// --- 控え（フォールバック）------------------------------------------------
//
// 1つのフォントに世界中の字は入っていない。同梱の Noto Sans JP も、
// 日本語と英数字は持っているが、絵文字やハングルは持っていない。
// そこで**本命に無い字が来たら、控えのフォントから探す**。
//
//   ・控えは**要るときに1つずつ**読む。はじめから全部読むと数十 MB になる
//   ・どの字をどのフォントが持っているかは FT_Get_Char_Index で尋ねる
//   ・どれも持っていなければ、本命の .notdef（□）を出す。
//     「無い」ことが見て分かるほうがよいので、消してしまわない
//
// 候補の場所を知っているのは ui.cpp（コアは自分でファイルを開かない）。
// そこから set_fallback_source() で口を1つ渡してもらう。
//
// **読んだ控えは、抱えたままにする。**「この字を持っていなかったから捨てる」に
// すると、あとで来た別の字をそのフォントが持っていても、もう見に行けなくなる
// （読む順は前に進むだけなので）。そのかわり数に上限を置く。
// フォントは1本 10 MB を超えることもあり（Windows の絵文字は 12 MB）、
// 際限なく足すとメモリが膨らむ。控えは4本まで
static const int kMaxFaces = 5;   // 本命1つ ＋ 控え4つ

struct FaceRec {
  FT_Face face;
  unsigned char* data;   // 中身。FreeType は読み終わるまでここを見続ける
  size_t data_n;
  Str name;              // どこから読んだか
  int set_px;            // このフェイスに入れてある大きさ
};

static FT_Library g_lib = 0;
// 0 番が本命、1 番から控え。
// 中身は処理系の持ちものであって、プログラムのメモリではない。
// 上限（--memory）に数えないよう malloc を使う（日本語のフォントは数 MB ある）
static FaceRec g_faces[kMaxFaces];
static int g_nfaces = 0;
static int g_px = 14;   // いま頼まれている大きさ

// 控えの候補を渡してもらう口（ui.cpp が埋める）。
// index 番目の候補があれば true。読めなかったときは data に 0 を入れて true
// （「その候補は飛ばして次へ」の意味）。候補が尽きたら false
static bool (*g_more)(int index, char** data, size_t* n, Str* name) = 0;
static int g_more_at = 0;   // 次に試す候補

static void set_fallback_source(bool (*fn)(int, char**, size_t*, Str*)) { g_more = fn; }

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

// 大きさはフェイスごとに入れる（控えは本命と同じ大きさにそろえる）
static void set_size(int i, int px) {
  if (g_faces[i].set_px == px) return;
  FT_Set_Pixel_Sizes(g_faces[i].face, 0, (FT_UInt)px);
  g_faces[i].set_px = px;
}

static void drop_face(int i) {
  if (g_faces[i].face) FT_Done_Face(g_faces[i].face);
  if (g_faces[i].data) free(g_faces[i].data);
  g_faces[i].face = 0;
  g_faces[i].data = 0;
  g_faces[i].data_n = 0;
  g_faces[i].name.clear();
  g_faces[i].set_px = 0;
}

static void unload() {
  cache_clear();
  if (g_cache) { free(g_cache); g_cache = 0; }
  for (int i = 0; i < g_nfaces; i++) drop_face(i);
  g_nfaces = 0;
  g_more_at = 0;
}

static bool active() { return g_nfaces > 0; }
static const Str& name() { return g_faces[0].name; }
static int size() { return g_px; }

// 中身（TTF / OTF / TTC）をうしろに足す。
// data は malloc で取ったもので、**足せたらこちらが持つ**（写しは取らない。
// 日本語のフォントは数 MB あり、写すと一時的に倍いる）。
// 足せなければ false を返すので、そのときは呼んだ側が返す
static bool add_face_owned(unsigned char* data, size_t n, const Str& from, int px) {
  if (g_nfaces >= kMaxFaces || !data || n == 0) return false;
  FT_Face face = 0;
  if (FT_New_Memory_Face(g_lib, (const FT_Byte*)data, (FT_Long)n, 0, &face) != 0) return false;
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px) != 0) {
    FT_Done_Face(face);
    return false;
  }
  FaceRec& r = g_faces[g_nfaces];
  r.face = face;
  r.data = data;
  r.data_n = n;
  r.name = from;
  r.set_px = px;
  g_nfaces++;
  return true;
}

// 本命を差し替える。控えはいったん捨てて、また要るときに読み直す
static bool load(const char* data, size_t n, int px, const Str& from) {
  if (!data || n == 0) return false;
  if (px < 4) px = 4;
  if (px > 400) px = 400;
  if (!g_lib && FT_Init_FreeType(&g_lib) != 0) return false;
  if (!cache_ready()) return false;

  // 先に開けるかどうかを見てから、いまのものを捨てる
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
  for (int i = 0; i < g_nfaces; i++) drop_face(i);
  g_nfaces = 0;
  g_more_at = 0;

  FaceRec& r = g_faces[0];
  r.face = face;
  r.data = copy;
  r.data_n = n;
  r.name = from;
  r.set_px = px;
  g_nfaces = 1;
  g_px = px;
  return true;
}

// 大きさだけ変える
static bool resize(int px) {
  if (g_nfaces == 0) return false;
  if (px < 4) px = 4;
  if (px > 400) px = 400;
  if (FT_Set_Pixel_Sizes(g_faces[0].face, 0, (FT_UInt)px) != 0) return false;
  g_faces[0].set_px = px;
  g_px = px;
  return true;
}

// 控えを1つ増やす。もう候補が無ければ false
static bool add_one_fallback() {
  if (!g_more || g_nfaces >= kMaxFaces) return false;
  for (;;) {
    char* data = 0;
    size_t n = 0;
    Str from;
    if (!g_more(g_more_at, &data, &n, &from)) return false;   // 候補が尽きた
    g_more_at++;
    if (!data || n == 0) continue;   // 読めなかった。次の候補へ
    bool dup = false;
    for (int i = 0; i < g_nfaces; i++)
      if (g_faces[i].name == from) dup = true;   // もう持っているものは足さない
    if (dup || !add_face_owned((unsigned char*)data, n, from, g_px)) {
      free(data);   // 足せなかったぶんは、こちらで返す
      continue;
    }
    return true;
  }
}

// この字を持っているフェイスを選ぶ。どれも持っていなければ 0（本命の □）
static int face_for(int cp) {
  for (int i = 0; i < g_nfaces; i++)
    if (FT_Get_Char_Index(g_faces[i].face, (FT_ULong)cp) != 0) return i;
  while (add_one_fallback()) {
    int i = g_nfaces - 1;
    if (FT_Get_Char_Index(g_faces[i].face, (FT_ULong)cp) != 0) return i;
  }
  return 0;
}

// 字形の濃さを写し取る。フォントによって並べ方が違う
static bool copy_bits(const FT_Bitmap& b, Glyph* e) {
  int n = (int)e->w * (int)e->h;
  if (n <= 0) { e->bits = 0; return true; }
  e->bits = (unsigned char*)malloc((size_t)n);
  if (!e->bits) return false;
  if (b.pixel_mode == FT_PIXEL_MODE_GRAY) {
    for (int y = 0; y < e->h; y++)
      for (int x = 0; x < e->w; x++)
        e->bits[y * e->w + x] = b.buffer[y * b.pitch + x];
  } else if (b.pixel_mode == FT_PIXEL_MODE_MONO) {
    // 1画素が1ビット。古いビットマップのフォントはこの形
    for (int y = 0; y < e->h; y++)
      for (int x = 0; x < e->w; x++) {
        unsigned char byte = b.buffer[y * b.pitch + (x >> 3)];
        e->bits[y * e->w + x] = (byte & (0x80 >> (x & 7))) ? 255 : 0;
      }
  } else if (b.pixel_mode == FT_PIXEL_MODE_BGRA) {
    // 色つきの絵文字。ここは濃さしか持てないので、形（不透明さ）だけ使う
    for (int y = 0; y < e->h; y++)
      for (int x = 0; x < e->w; x++)
        e->bits[y * e->w + x] = b.buffer[y * b.pitch + x * 4 + 3];
  } else {
    free(e->bits);
    e->bits = 0;
    return false;
  }
  return true;
}

static const Glyph* glyph(int cp, int px) {
  if (g_nfaces == 0 || !g_cache) return 0;
  unsigned at = slot_of(cp, px);
  for (int probe = 0; probe < kCap; probe++) {
    Glyph& e = g_cache[(at + (unsigned)probe) % (unsigned)kCap];
    if (e.cp == cp && e.px == px) return &e;
    if (e.cp >= 0) continue;
    // 空き。ここに作る
    if (g_count > kCap * 3 / 4) { cache_clear(); return glyph(cp, px); }
    int fi = face_for(cp);
    set_size(fi, px);
    if (FT_Load_Char(g_faces[fi].face, (FT_ULong)cp, FT_LOAD_RENDER) != 0) return 0;
    FT_GlyphSlot s = g_faces[fi].face->glyph;
    e.cp = cp;
    e.px = px;
    e.w = (short)s->bitmap.width;
    e.h = (short)s->bitmap.rows;
    e.left = (short)s->bitmap_left;
    e.top = (short)s->bitmap_top;
    e.adv = (short)(s->advance.x >> 6);
    e.bits = 0;
    if (!copy_bits(s->bitmap, &e)) { e.cp = -1; return 0; }
    g_count++;
    return &e;
  }
  return 0;
}

// 1行の高さ。**字の上端から下端まで**（ascender − descender）で数える。
// 高さは**本命だけ**で決める。控えごとに変わると、行の高さが字で揺れてしまう。
//
// FreeType の metrics.height は「次の行までの送り」で、行間のあきを含む。
// ヒラギノなら 24 画素の字で 36 もある。これを1行の高さとして使うと、
// 基準線から下に 11 画素ぶん**字の下にあきができる**。使わない。
static int line_height(int px) {
  if (g_nfaces == 0) return px;
  set_size(0, px);
  int h = (int)((g_faces[0].face->size->metrics.ascender -
                 g_faces[0].face->size->metrics.descender) >> 6);
  return h > 0 ? h : px;
}
static int ascender(int px) {
  if (g_nfaces == 0) return px;
  set_size(0, px);
  return (int)(g_faces[0].face->size->metrics.ascender >> 6);
}

}  // namespace ft

#endif  // SHARK_FREETYPE

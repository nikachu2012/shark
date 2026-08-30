// png.inc — PNG を読む（std.ui の ui.load_png）
//
// 外の library には頼らない決まり（spec/README.md）なので、
// 展開（inflate, RFC 1951）もここで書いている。書き出しは ui.cpp の側。
//
// 読めるのは**編み込み無し**の PNG（interlace 0）。色の持ち方は
// 白黒・白黒＋透明・色つき・色つき＋透明・パレットのどれでもよく、
// 深さは 1/2/4/8/16 ビット。16 ビットは 8 ビットに落とす。
//
// 出来上がりは 0xTTRRGGBB の並び。**いちばん上のバイトは「透け具合」**で、
// 0 が不透明（ui.cpp の色の決まりと同じ）。PNG のアルファは裏返して入れる。

namespace png {

// ---------------------------------------------------------------- ビットを読む
// deflate は下のビットから詰める
struct Bits {
  const unsigned char* d;
  int n, pos, cnt;
  uint32_t acc;
  bool bad;
  Bits(const unsigned char* p, int len) : d(p), n(len), pos(0), cnt(0), acc(0), bad(false) {}
  int get(int need) {
    while (cnt < need) {
      if (pos >= n) { bad = true; return 0; }
      acc |= (uint32_t)d[pos++] << cnt;
      cnt += 8;
    }
    int v = (int)(acc & ((1u << need) - 1));
    acc >>= need;
    cnt -= need;
    return v;
  }
  void align() { acc = 0; cnt = 0; }   // 次のバイトの切れ目まで捨てる
};

// ------------------------------------------------------------ ハフマンの木
// 正準ハフマン。符号長の並びだけから組み立てる（RFC 1951 の 3.2.2）
struct Huff {
  short count[16];
  short sym[288];
  bool build(const unsigned char* lens, int n) {
    for (int i = 0; i < 16; i++) count[i] = 0;
    for (int i = 0; i < n; i++) count[lens[i]]++;
    int left = 1;
    for (int len = 1; len < 16; len++) {
      left <<= 1;
      left -= count[len];
      if (left < 0) return false;   // 符号が多すぎる。壊れている
    }
    short offs[16];
    offs[0] = 0; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + count[len]);
    for (int i = 0; i < n; i++) if (lens[i]) sym[offs[lens[i]]++] = (short)i;
    return true;
  }
  // 読めなければ -1
  int decode(Bits& b) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
      code |= b.get(1);
      if (b.bad) return -1;
      int c = count[len];
      if (code - c < first) return sym[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    return -1;
  }
};

static const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577};
static const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                     7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// 決め打ちの木（RFC 1951 の 3.2.6）。使うときに1度だけ組み立てる
static void fixed_trees(Huff& lit, Huff& dist) {
  unsigned char l[288];
  for (int i = 0; i < 144; i++) l[i] = 8;
  for (int i = 144; i < 256; i++) l[i] = 9;
  for (int i = 256; i < 280; i++) l[i] = 7;
  for (int i = 280; i < 288; i++) l[i] = 8;
  lit.build(l, 288);
  unsigned char d[30];
  for (int i = 0; i < 30; i++) d[i] = 5;
  dist.build(d, 30);
}

// 1つの塊を展開して out の後ろに足す
static bool block(Bits& b, const Huff& lit, const Huff& dist, Str& out, int limit) {
  for (;;) {
    int s = lit.decode(b);
    if (s < 0) return false;
    if (s < 256) {
      if (out.size() >= limit) return false;
      out.push((char)s);
      continue;
    }
    if (s == 256) return true;   // 塊の終わり
    s -= 257;
    if (s >= 29) return false;
    int len = kLenBase[s] + b.get(kLenExtra[s]);
    int ds = dist.decode(b);
    if (ds < 0 || ds >= 30) return false;
    int off = kDistBase[ds] + b.get(kDistExtra[ds]);
    if (b.bad || off > out.size()) return false;
    if (out.size() + len > limit) return false;
    int from = out.size() - off;
    for (int i = 0; i < len; i++) out.push(out[from + i]);   // 重なってもよい
  }
}

// zlib の包み（RFC 1950）を外して展開する
static bool inflate(const unsigned char* src, int n, Str& out, int limit) {
  if (n < 2) return false;
  // zlib の頭。下位4ビットが 8（deflate）で、辞書つきは扱わない
  if ((src[0] & 0x0f) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return false;
  Bits b(src + 2, n - 2);
  Huff fl, fd;
  bool fixed_ready = false;
  for (;;) {
    int last = b.get(1);
    int type = b.get(2);
    if (b.bad) return false;
    if (type == 0) {                     // そのまま入っている
      b.align();
      if (b.pos + 4 > b.n) return false;
      int len = b.d[b.pos] | (b.d[b.pos + 1] << 8);
      b.pos += 4;                        // 長さと、その裏返し
      if (b.pos + len > b.n || out.size() + len > limit) return false;
      out.append((const char*)b.d + b.pos, len);
      b.pos += len;
    } else if (type == 1) {              // 決め打ちの木
      if (!fixed_ready) { fixed_trees(fl, fd); fixed_ready = true; }
      if (!block(b, fl, fd, out, limit)) return false;
    } else if (type == 2) {              // その場で決めた木
      int nlen = b.get(5) + 257, ndist = b.get(5) + 1, ncode = b.get(4) + 4;
      if (b.bad || nlen > 286 || ndist > 30) return false;
      static const unsigned char kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                               11, 4, 12, 3, 13, 2, 14, 1, 15};
      unsigned char cl[19];
      for (int i = 0; i < 19; i++) cl[i] = 0;
      for (int i = 0; i < ncode; i++) cl[kOrder[i]] = (unsigned char)b.get(3);
      if (b.bad) return false;
      Huff clh;
      if (!clh.build(cl, 19)) return false;
      // 符号長そのものを、その木で展開する
      unsigned char lens[286 + 30];
      int i = 0;
      while (i < nlen + ndist) {
        int s = clh.decode(b);
        if (s < 0) return false;
        if (s < 16) { lens[i++] = (unsigned char)s; continue; }
        int rep = 0;
        unsigned char v = 0;
        if (s == 16) {
          if (i == 0) return false;
          v = lens[i - 1];
          rep = 3 + b.get(2);
        } else if (s == 17) {
          rep = 3 + b.get(3);
        } else {
          rep = 11 + b.get(7);
        }
        if (b.bad || i + rep > nlen + ndist) return false;
        while (rep--) lens[i++] = v;
      }
      Huff lh, dh;
      if (!lh.build(lens, nlen)) return false;
      if (!dh.build(lens + nlen, ndist)) return false;
      if (!block(b, lh, dh, out, limit)) return false;
    } else {
      return false;
    }
    if (last) return true;
  }
}

// ---------------------------------------------------------------- PNG 本体
static uint32_t be32(const unsigned char* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// 濾し方を戻す（PNG の 9章）。cur と prev はどちらも stride バイト
static void unfilter(int type, unsigned char* cur, const unsigned char* prev, int stride, int bpp) {
  switch (type) {
    case 0: break;
    case 1:
      for (int i = bpp; i < stride; i++) cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
      break;
    case 2:
      if (prev) for (int i = 0; i < stride; i++) cur[i] = (unsigned char)(cur[i] + prev[i]);
      break;
    case 3:
      for (int i = 0; i < stride; i++) {
        int left = i >= bpp ? cur[i - bpp] : 0;
        int up = prev ? prev[i] : 0;
        cur[i] = (unsigned char)(cur[i] + ((left + up) >> 1));
      }
      break;
    case 4:
      for (int i = 0; i < stride; i++) {
        int a = i >= bpp ? cur[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
        int p = a + b - c;
        int pa = p > a ? p - a : a - p;
        int pb = p > b ? p - b : b - p;
        int pc = p > c ? p - c : c - p;
        int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
        cur[i] = (unsigned char)(cur[i] + pr);
      }
      break;
    default: break;
  }
}

// 深さ depth のビットを i 番目だけ取り出す（1/2/4/8/16 ビット）
static int sample(const unsigned char* row, int i, int depth) {
  if (depth == 8) return row[i];
  if (depth == 16) return row[i * 2];           // 上の 8 ビットだけ使う
  int per = 8 / depth;                          // 1バイトに何個入るか
  int v = row[i / per];
  int shift = 8 - depth * (i % per + 1);
  return (v >> shift) & ((1 << depth) - 1);
}

// 読めたら true。px には w*h 個の uint32_t（0xTTRRGGBB。上のバイトは透け具合）が
// そのまま並ぶ。Canvas の画素の入れ物にそのまま渡せる形にしてある
static bool load(const unsigned char* d, int n, int* out_w, int* out_h, Str& px) {
  static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (n < 8 + 25) return false;
  for (int i = 0; i < 8; i++) if (d[i] != kSig[i]) return false;

  int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
  bool have_ihdr = false;
  Str idat;
  unsigned char pal[256 * 3];
  int pal_n = 0;
  unsigned char pal_a[256];
  for (int i = 0; i < 256; i++) pal_a[i] = 255;
  bool have_trns = false;
  int trns_gray = -1, trns_r = -1, trns_g = -1, trns_b = -1;

  int p = 8;
  while (p + 8 <= n) {
    uint32_t len = be32(d + p);
    if (len > 0x7fffffffu || p + 8 + (int)len + 4 > n) return false;
    const unsigned char* tag = d + p + 4;
    const unsigned char* body = d + p + 8;
    if (tag[0] == 'I' && tag[1] == 'H' && tag[2] == 'D' && tag[3] == 'R') {
      if (len < 13) return false;
      w = (int)be32(body);
      h = (int)be32(body + 4);
      depth = body[8];
      ctype = body[9];
      interlace = body[12];
      have_ihdr = true;
      if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
      if (interlace != 0) return false;   // 編み込みは読まない
      if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return false;
      if (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 4 && ctype != 6) return false;
      // パレットは 8 ビットまで、白黒＋透明と色つきは 8 か 16 ビット
      if (ctype == 3 && depth == 16) return false;
      if ((ctype == 2 || ctype == 4 || ctype == 6) && depth != 8 && depth != 16) return false;
    } else if (tag[0] == 'P' && tag[1] == 'L' && tag[2] == 'T' && tag[3] == 'E') {
      if (len % 3 != 0 || len > 256 * 3) return false;
      pal_n = (int)len / 3;
      for (uint32_t i = 0; i < len; i++) pal[i] = body[i];
    } else if (tag[0] == 't' && tag[1] == 'R' && tag[2] == 'N' && tag[3] == 'S') {
      have_trns = true;
      if (ctype == 3) {
        for (uint32_t i = 0; i < len && i < 256; i++) pal_a[i] = body[i];
      } else if (ctype == 0 && len >= 2) {
        trns_gray = (body[0] << 8) | body[1];
      } else if (ctype == 2 && len >= 6) {
        trns_r = (body[0] << 8) | body[1];
        trns_g = (body[2] << 8) | body[3];
        trns_b = (body[4] << 8) | body[5];
      }
    } else if (tag[0] == 'I' && tag[1] == 'D' && tag[2] == 'A' && tag[3] == 'T') {
      idat.append((const char*)body, (int)len);
    } else if (tag[0] == 'I' && tag[1] == 'E' && tag[2] == 'N' && tag[3] == 'D') {
      break;
    }
    p += 8 + (int)len + 4;   // 長さ・名前・中身・確かめの数
  }
  if (!have_ihdr || idat.empty()) return false;
  if (ctype == 3 && pal_n == 0) return false;

  int channels = (ctype == 0) ? 1 : (ctype == 2) ? 3 : (ctype == 3) ? 1 : (ctype == 4) ? 2 : 4;
  // 1行のバイト数（濾し方の1バイトは別）
  int stride = (w * channels * depth + 7) / 8;
  int bpp = (channels * depth + 7) / 8;
  if (bpp < 1) bpp = 1;
  // 展開したあとの大きさは、ここより増えない
  int limit = (stride + 1) * h;
  if (limit <= 0) return false;

  Str raw;
  raw.reserve(limit);
  if (!inflate((const unsigned char*)idat.data(), idat.size(), raw, limit)) return false;
  if (raw.size() < limit) return false;

  size_t total = (size_t)w * (size_t)h;
  if (sk_mem_limit() != 0 && total * sizeof(uint32_t) > sk_mem_limit()) return false;
  px.clear();
  px.reserve((int)(total * sizeof(uint32_t)));
  for (size_t i = 0; i < total * sizeof(uint32_t); i++) px.push('\0');
  uint32_t* dst = (uint32_t*)&px[0];

  if (raw.size() <= 0) return false;
  unsigned char* rows = (unsigned char*)&raw[0];
  const unsigned char* prev = 0;
  int mx = (1 << depth) - 1;   // その深さの最大値（白黒を 0〜255 に伸ばすのに使う）
  for (int y = 0; y < h; y++) {
    unsigned char* line = rows + (size_t)y * (size_t)(stride + 1);
    int ft = line[0];
    unsigned char* cur = line + 1;
    unfilter(ft, cur, prev, stride, bpp);
    for (int x = 0; x < w; x++) {
      int r = 0, g = 0, b = 0, al = 255;
      if (ctype == 0) {
        int v = sample(cur, x, depth);
        if (have_trns && trns_gray >= 0 && v == (depth == 16 ? trns_gray >> 8 : trns_gray)) al = 0;
        r = g = b = (depth == 16) ? v : v * 255 / mx;
      } else if (ctype == 2) {
        int st = depth == 16 ? 2 : 1;
        r = cur[(x * 3 + 0) * st];
        g = cur[(x * 3 + 1) * st];
        b = cur[(x * 3 + 2) * st];
        if (have_trns && trns_r >= 0 && r == (trns_r >> (depth == 16 ? 8 : 0)) &&
            g == (trns_g >> (depth == 16 ? 8 : 0)) && b == (trns_b >> (depth == 16 ? 8 : 0)))
          al = 0;
      } else if (ctype == 3) {
        int v = sample(cur, x, depth);
        if (v >= pal_n) v = 0;
        r = pal[v * 3 + 0]; g = pal[v * 3 + 1]; b = pal[v * 3 + 2];
        al = pal_a[v];
      } else if (ctype == 4) {
        int st = depth == 16 ? 2 : 1;
        r = g = b = cur[(x * 2 + 0) * st];
        al = cur[(x * 2 + 1) * st];
      } else {
        int st = depth == 16 ? 2 : 1;
        r = cur[(x * 4 + 0) * st];
        g = cur[(x * 4 + 1) * st];
        b = cur[(x * 4 + 2) * st];
        al = cur[(x * 4 + 3) * st];
      }
      // PNG は「不透明のぶん」、こちらは「透け具合」。裏返して入れる
      dst[y * w + x] = ((uint32_t)(255 - al) << 24) | ((uint32_t)r << 16) |
                       ((uint32_t)g << 8) | (uint32_t)b;
    }
    prev = cur;
  }
  *out_w = w;
  *out_h = h;
  return true;
}

}  // namespace png

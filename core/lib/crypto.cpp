// crypto.cpp — std.crypto（spec/library/crypto.md）
//
// ハッシュは自前で持つ。外部ライブラリには依存しない（spec/runtime/platform.md）。
// 乱数のもとだけは機種に頼るので、移植層（PlatformRandom）から借りる。
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

typedef unsigned char u8;

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

// ハッシュはどれも「まとめて1回」で計算する。中身の違いは、この形に揃える
typedef void (*HashFn)(const u8* data, size_t len, u8* out);

// ------------------------------------------------------------------ SHA-256
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static void sha256_block(uint32_t* h, const u8* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], x = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = x + s1 + ch + kSha256K[i] + w[i];
    uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = s0 + maj;
    x = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += x;
}

// 最後の半端な分に 0x80 と長さ（ビット数）を足して締める。SHA-1 とも共通の形
static int pad_be64(const u8* d, size_t n, size_t done, u8* tail) {
  size_t rest = n - done;
  for (size_t i = 0; i < rest; i++) tail[i] = d[done + i];
  tail[rest] = 0x80;
  int blocks = (rest + 1 <= 56) ? 1 : 2;
  for (size_t i = rest + 1; i < (size_t)blocks * 64 - 8; i++) tail[i] = 0;
  uint64_t bits = (uint64_t)n * 8;
  for (int i = 0; i < 8; i++) tail[(size_t)blocks * 64 - 1 - i] = (u8)(bits >> (i * 8));
  return blocks;
}

static void sha256_raw(const u8* d, size_t n, u8* out) {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) sha256_block(h, d + i);
  u8 tail[128];
  int blocks = pad_be64(d, n, i, tail);
  for (int b = 0; b < blocks; b++) sha256_block(h, tail + b * 64);
  for (int k = 0; k < 8; k++) {
    out[k * 4] = (u8)(h[k] >> 24);
    out[k * 4 + 1] = (u8)(h[k] >> 16);
    out[k * 4 + 2] = (u8)(h[k] >> 8);
    out[k * 4 + 3] = (u8)h[k];
  }
}

// ------------------------------------------------------------------ SHA-512
static uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

static void sha512_block(uint64_t* h, const u8* p) {
  uint64_t w[80];
  for (int i = 0; i < 16; i++) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v = (v << 8) | (uint64_t)p[i * 8 + k];
    w[i] = v;
  }
  for (int i = 16; i < 80; i++) {
    uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint64_t e = h[4], f = h[5], g = h[6], x = h[7];
  for (int i = 0; i < 80; i++) {
    uint64_t s1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
    uint64_t ch = (e & f) ^ (~e & g);
    uint64_t t1 = x + s1 + ch + kSha512K[i] + w[i];
    uint64_t s0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
    uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint64_t t2 = s0 + maj;
    x = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += x;
}

static void sha512_raw(const u8* d, size_t n, u8* out) {
  uint64_t h[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
                   0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                   0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
  size_t i = 0;
  for (; i + 128 <= n; i += 128) sha512_block(h, d + i);
  // 締め方は SHA-256 と同じだが、まとまりが 128 バイトで、長さの欄が 16 バイト
  u8 tail[256];
  size_t rest = n - i;
  for (size_t k = 0; k < rest; k++) tail[k] = d[i + k];
  tail[rest] = 0x80;
  int blocks = (rest + 1 <= 112) ? 1 : 2;
  size_t total = (size_t)blocks * 128;
  for (size_t k = rest + 1; k < total - 8; k++) tail[k] = 0;
  uint64_t bits = (uint64_t)n * 8;
  for (int k = 0; k < 8; k++) tail[total - 1 - k] = (u8)(bits >> (k * 8));
  for (int b = 0; b < blocks; b++) sha512_block(h, tail + b * 128);
  for (int k = 0; k < 8; k++)
    for (int j = 0; j < 8; j++) out[k * 8 + j] = (u8)(h[k] >> ((7 - j) * 8));
}

// ------------------------------------------------------------------ SHA-1
// もう安全ではない。古いデータと突き合わせるためだけに置いてある
static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t* h, const u8* p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 80; i++)
    w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
    else { f = b ^ c ^ d; k = 0xca62c1d6u; }
    uint32_t t = rol32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol32(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1_raw(const u8* d, size_t n, u8* out) {
  uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) sha1_block(h, d + i);
  u8 tail[128];
  int blocks = pad_be64(d, n, i, tail);
  for (int b = 0; b < blocks; b++) sha1_block(h, tail + b * 64);
  for (int k = 0; k < 5; k++) {
    out[k * 4] = (u8)(h[k] >> 24);
    out[k * 4 + 1] = (u8)(h[k] >> 16);
    out[k * 4 + 2] = (u8)(h[k] >> 8);
    out[k * 4 + 3] = (u8)h[k];
  }
}

// ------------------------------------------------------------------ MD5
// SHA-1 と同じく、もう安全ではない。長さの欄も並びも小さい方から入る
static const uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
static const int kMd5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t* h, const u8* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; i++)
    m[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
           ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
    else { f = c ^ (b | ~d); g = (7 * i) % 16; }
    uint32_t t = d;
    d = c;
    c = b;
    b = b + rol32(a + f + kMd5K[i] + m[g], kMd5S[i]);
    a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

static void md5_raw(const u8* d, size_t n, u8* out) {
  uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
  size_t i = 0;
  for (; i + 64 <= n; i += 64) md5_block(h, d + i);
  u8 tail[128];
  size_t rest = n - i;
  for (size_t k = 0; k < rest; k++) tail[k] = d[i + k];
  tail[rest] = 0x80;
  int blocks = (rest + 1 <= 56) ? 1 : 2;
  size_t total = (size_t)blocks * 64;
  for (size_t k = rest + 1; k < total - 8; k++) tail[k] = 0;
  uint64_t bits = (uint64_t)n * 8;
  for (int k = 0; k < 8; k++) tail[total - 8 + k] = (u8)(bits >> (k * 8));
  for (int b = 0; b < blocks; b++) md5_block(h, tail + b * 64);
  for (int k = 0; k < 4; k++)
    for (int j = 0; j < 4; j++) out[k * 4 + j] = (u8)(h[k] >> (j * 8));
}

// ------------------------------------------------------------------ HMAC
// 鍵を混ぜて2回ハッシュする（RFC 2104）。中身はどのハッシュでも同じ形
static Str hmac_digest(HashFn fn, int dlen, int blen, const Str& key, const Str& msg) {
  u8 k[128];
  for (int i = 0; i < blen; i++) k[i] = 0;
  if (key.size() > blen) fn((const u8*)key.data(), (size_t)key.size(), k);
  else for (int i = 0; i < key.size(); i++) k[i] = (u8)key[i];

  Str inner;
  inner.reserve(blen + msg.size());
  for (int i = 0; i < blen; i++) inner.push((char)(k[i] ^ 0x36));
  inner.append(msg);
  u8 ih[64];
  fn((const u8*)inner.data(), (size_t)inner.size(), ih);

  Str outer;
  outer.reserve(blen + dlen);
  for (int i = 0; i < blen; i++) outer.push((char)(k[i] ^ 0x5c));
  outer.append((const char*)ih, dlen);
  u8 oh[64];
  fn((const u8*)outer.data(), (size_t)outer.size(), oh);
  return Str((const char*)oh, dlen);
}

// ------------------------------------------------------------------ 乱数のもと
// 真の乱数（機器の雑音）を先に試し、取れなければ暗号学的に安全な乱数に落ちる。
// どちらから取れたかは source() が返す（spec/library/crypto.md）
enum RandSource { RS_UNKNOWN = 0, RS_TRUE, RS_SECURE, RS_NONE };
static RandSource g_src = RS_UNKNOWN;   // 直前に取れた出どころ。source() が返す

static bool fill_random(u8* buf, int n) {
  const PlatformRandom* r = platform().random;
  if (r && r->true_bytes && r->true_bytes(buf, n)) { g_src = RS_TRUE; return true; }
  if (r && r->secure_bytes && r->secure_bytes(buf, n)) { g_src = RS_SECURE; return true; }
  g_src = RS_NONE;
  return false;
}
// 乱数を1つ取る。もとが無ければ実行時エラー（黙って弱い乱数を返さない）
static bool draw64(VM& vm, uint64_t* out) {
  u8 b[8];
  if (!fill_random(b, 8)) {
    vm.panic(Str("この処理系は乱数のもとを持っていません"));
    return false;
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | (uint64_t)b[i];
  *out = v;
  return true;
}

// 16 進の行き来は型の側にある（bytes.to_hex / string.from_hex）。
// ここに残すのは uuid4 が使う数字の表だけ
static const char* kHexDigits = "0123456789abcdef";

// ------------------------------------------------------------------ 呼ばれる関数
#define C_HASH(name, fn, dlen)                                                 \
  static NativeStatus name(VM& vm, Value* a, int n, Value& out) {              \
    (void)vm; (void)n;                                                         \
    const Str& s = S(a, 0);                                                    \
    u8 d[dlen];                                                                \
    fn((const u8*)s.data(), (size_t)s.size(), d);                              \
    out = mk_bytes(Str((const char*)d, dlen));                                 \
    return N_Ok;                                                               \
  }
C_HASH(c_sha256, sha256_raw, 32)
C_HASH(c_sha512, sha512_raw, 64)
C_HASH(c_sha1, sha1_raw, 20)
C_HASH(c_md5, md5_raw, 16)

static NativeStatus c_hmac_sha256(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_bytes(hmac_digest(sha256_raw, 32, 64, S(a, 0), S(a, 1)));
  return N_Ok;
}
static NativeStatus c_hmac_sha512(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_bytes(hmac_digest(sha512_raw, 64, 128, S(a, 0), S(a, 1)));
  return N_Ok;
}

// 中身が同じでも違っても、掛かる時間を変えない比べ方。
// 先頭から何バイト合っていたかを、時間の差から読み取られないようにする
static NativeStatus c_equal(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  const Str& x = S(a, 0);
  const Str& y = S(a, 1);
  if (x.size() != y.size()) { out = mk_bool(false); return N_Ok; }
  u8 diff = 0;
  for (int i = 0; i < x.size(); i++) diff |= (u8)(x[i] ^ y[i]);
  out = mk_bool(diff == 0);
  return N_Ok;
}

static NativeStatus c_random_bytes(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t want = A(a, 0)->i;
  if (want < 0) { vm.panic(Str("random_bytes の長さに負の数は渡せません")); return N_Panic; }
  if (want > (1 << 20)) {
    vm.panic(Str("random_bytes で一度に取れるのは 1048576 バイトまでです"));
    return N_Panic;
  }
  Str r;
  r.reserve((int)want);
  u8 buf[256];
  for (int64_t got = 0; got < want; got += 256) {
    int m = (int)(want - got < 256 ? want - got : 256);
    if (!fill_random(buf, m)) {
      vm.panic(Str("この処理系は乱数のもとを持っていません"));
      return N_Panic;
    }
    r.append((const char*)buf, m);
  }
  out = mk_bytes(r);
  return N_Ok;
}
static NativeStatus c_random_int(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t lo = A(a, 0)->i, hi = A(a, 1)->i;
  if (hi < lo) { vm.panic(Str("random_int の範囲が逆です")); return N_Panic; }
  uint64_t span = (uint64_t)(hi - lo) + 1;
  uint64_t v = 0;
  if (span == 0) {   // lo と hi が int の端どうし。64 ビットぜんぶが範囲になる
    if (!draw64(vm, &v)) return N_Panic;
    out = mk_int((int64_t)v);
    return N_Ok;
  }
  // span で割り切れるところまでで打ち切り、外れたら引き直す。
  // 余りをそのまま使うと、小さい値だけが出やすくなるため
  uint64_t zone = (~(uint64_t)0 / span) * span;
  int tries = 0;
  do {
    if (!draw64(vm, &v)) return N_Panic;
    // まともな乱数なら、外れ続ける見込みはない。64 回続いたら、もとが壊れている。
    // ここで引き直し続けると、呼んだ側に戻らなくなる（spec/runtime/platform.md）
    if (++tries > 64) {
      vm.panic(Str("乱数のもとが同じ値ばかり返しています"));
      return N_Panic;
    }
  } while (v >= zone);
  out = mk_int(lo + (int64_t)(v % span));
  return N_Ok;
}
// RFC 4122 の版 4。16 バイトの乱数のうち、版と種別の欄だけ決まった形にする
static NativeStatus c_uuid4(VM& vm, Value* a, int n, Value& out) {
  (void)a; (void)n;
  u8 b[16];
  if (!fill_random(b, 16)) {
    vm.panic(Str("この処理系は乱数のもとを持っていません"));
    return N_Panic;
  }
  b[6] = (u8)((b[6] & 0x0f) | 0x40);
  b[8] = (u8)((b[8] & 0x3f) | 0x80);
  Str r;
  r.reserve(36);
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) r.push('-');
    r.push(kHexDigits[b[i] >> 4]);
    r.push(kHexDigits[b[i] & 15]);
  }
  out = mk_str(r);
  return N_Ok;
}
// 覚えておいた結果ではなく、そのつど本当に取ってみて答える。
// 雑音が尽きれば同じ機種でも出どころは変わるので、古い答えを使い回さない
static NativeStatus c_source(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  u8 probe[8];
  fill_random(probe, 8);
  out = mk_str(g_src == RS_TRUE ? "true" : (g_src == RS_SECURE ? "secure" : "none"));
  return N_Ok;
}

void register_crypto(Registry& r) {
  TypeTable& t = r.types();
  Type* ts = t.t_string();
  Type* ti = t.t_int();
  Type* tb = t.t_bool();
  Type* tby = t.t_bytes();
  // string もバイト列として受ける（中の並びは UTF-8 のまま）
  r.add("crypto.sha256", c_sha256, tby, tby);
  r.add("crypto.sha256", c_sha256, tby, ts);
  r.add("crypto.sha512", c_sha512, tby, tby);
  r.add("crypto.sha512", c_sha512, tby, ts);
  r.add("crypto.sha1", c_sha1, tby, tby);
  r.add("crypto.sha1", c_sha1, tby, ts);
  r.add("crypto.md5", c_md5, tby, tby);
  r.add("crypto.md5", c_md5, tby, ts);
  r.add("crypto.hmac_sha256", c_hmac_sha256, tby, tby, tby);
  r.add("crypto.hmac_sha256", c_hmac_sha256, tby, tby, ts);
  r.add("crypto.hmac_sha256", c_hmac_sha256, tby, ts, tby);
  r.add("crypto.hmac_sha256", c_hmac_sha256, tby, ts, ts);
  r.add("crypto.hmac_sha512", c_hmac_sha512, tby, tby, tby);
  r.add("crypto.hmac_sha512", c_hmac_sha512, tby, tby, ts);
  r.add("crypto.hmac_sha512", c_hmac_sha512, tby, ts, tby);
  r.add("crypto.hmac_sha512", c_hmac_sha512, tby, ts, ts);
  r.add("crypto.equal", c_equal, tb, tby, tby);
  r.add("crypto.equal", c_equal, tb, ts, ts);
  r.add("crypto.random_bytes", c_random_bytes, tby, ti);
  r.add("crypto.random_int", c_random_int, ti, ti, ti);
  r.add("crypto.uuid4", c_uuid4, ts);
  r.add("crypto.source", c_source, ts);
  r.enable_module("std.crypto");
}

}  // namespace shark

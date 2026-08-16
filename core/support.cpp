#include "support.h"
#include "platform/platform.h"

// 数の書き出しにだけ <stdio.h> の snprintf を使う。入出力はしない
// （どの機種にもある範囲。結果は IEEE 754 で決まるので環境差は出ない）
#include <stdio.h>

namespace shark {

static const Platform* g_platform = 0;

const Platform& platform() {
  // 差し替えられていないときに使うもの。作る相手に合わせて選ぶ
  if (!g_platform) {
#if defined(__EMSCRIPTEN__)
    g_platform = platform_web();
#else
    g_platform = platform_desktop();
#endif
  }
  return *g_platform;
}
void platform_set(const Platform* p) { g_platform = p; }

// 確保した塊の先頭に「大きさ」と「どの区分か」を置いておく。
// 16 バイトにしてあるのは、返す番地の並びを崩さないため。
// 区分を覚えておくのは、解放が実行中でなくても正しく引くため。
static const size_t kMemHeader = 16;

size_t sk_mem_used_v = 0;
size_t sk_mem_run_used_v = 0;
size_t sk_mem_limit_v = 0;
int sk_mem_phase_v = 0;

void sk_mem_set_limit(size_t bytes) { sk_mem_limit_v = bytes; }

int sk_mem_set_phase(int phase) {
  int prev = sk_mem_phase_v;
  sk_mem_phase_v = phase;
  return prev;
}

void* sk_alloc(size_t n) {
  size_t total = n + kMemHeader;
  char* base = (char*)platform().alloc(total);
  if (!base) sk_fatal("メモリが足りません");
  ((size_t*)base)[0] = total;
  ((size_t*)base)[1] = (size_t)sk_mem_phase_v;
  sk_mem_used_v += total;
  if (sk_mem_phase_v == 1) sk_mem_run_used_v += total;
  return base + kMemHeader;
}

void* sk_realloc(void* p, size_t n) {
  if (!p) return sk_alloc(n);
  char* base = (char*)p - kMemHeader;
  size_t old = ((size_t*)base)[0];
  size_t tag = ((size_t*)base)[1];   // 区分は最初に確保したときのまま
  size_t total = n + kMemHeader;
  char* nb = (char*)platform().realloc(base, total);
  if (!nb) sk_fatal("メモリが足りません");
  ((size_t*)nb)[0] = total;
  ((size_t*)nb)[1] = tag;
  sk_mem_used_v = sk_mem_used_v - old + total;
  if (tag == 1) sk_mem_run_used_v = sk_mem_run_used_v - old + total;
  return nb + kMemHeader;
}

void sk_free(void* p) {
  if (!p) return;
  char* base = (char*)p - kMemHeader;
  size_t total = ((size_t*)base)[0];
  size_t tag = ((size_t*)base)[1];
  sk_mem_used_v -= total;
  if (tag == 1) sk_mem_run_used_v -= total;
  platform().free(base);
}

void sk_fatal(const char* msg) { platform().fatal(msg); }

Str str_from_int(int64_t v) {
  char buf[24];
  int i = 24;
  bool neg = v < 0;
  uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
  if (u == 0) buf[--i] = '0';
  while (u) { buf[--i] = (char)('0' + (int)(u % 10)); u /= 10; }
  if (neg) buf[--i] = '-';
  return Str(buf + i, 24 - i);
}

Str str_from_uint_base(uint64_t v, int base, bool upper) {
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  char buf[70];
  int i = 70;
  if (v == 0) buf[--i] = '0';
  while (v) { buf[--i] = digits[v % (uint64_t)base]; v /= (uint64_t)base; }
  return Str(buf + i, 70 - i);
}

// 1 桁から増やしていき、元に戻る一番短い表現を選ぶ（環境差を出さないため自前で行う）
//
// 桁数が決まったら、ふつうの大きさのうちは小数の形（60.0）で出す。
// %g に任せると 60.0 が 6e+01 になってしまう（有効桁が 1 桁のため）。
Str str_from_float(double v) {
  if (v != v) return Str("nan");
  if (v > 1.7976931348623157e308) return Str("inf");
  if (v < -1.7976931348623157e308) return Str("-inf");
  char buf[64];
  int prec = 17;
  for (int p = 1; p <= 17; p++) {
    snprintf(buf, sizeof buf, "%.*e", p - 1, v);   // 有効数字 p 桁
    double back = 0;
    if (str_to_float(Str(buf), &back) && back == v) { prec = p; break; }
  }

  // "1.5e+02" の指数の部分を読む
  int exp = 0, sign = 1;
  for (int i = 0; buf[i]; i++) {
    if (buf[i] != 'e') continue;
    int j = i + 1;
    if (buf[j] == '+' || buf[j] == '-') { sign = buf[j] == '-' ? -1 : 1; j++; }
    for (; buf[j] >= '0' && buf[j] <= '9'; j++) exp = exp * 10 + (buf[j] - '0');
    exp *= sign;
    break;
  }

  // 大きすぎず小さすぎなければ小数の形にする。桁は「有効桁 - 1 - 指数」で足りる
  if (exp >= -4 && exp <= 15) {
    int decimals = prec - 1 - exp;
    if (decimals < 1) decimals = 1;      // 60 → "60.0"（float と分かるように）
    if (decimals > 30) decimals = 30;
    char fixed[64];
    snprintf(fixed, sizeof fixed, "%.*f", decimals, v);
    double back = 0;
    if (str_to_float(Str(fixed), &back) && back == v) return Str(fixed);
  }

  // それ以外は指数の形。余分な 0 を落とすために %g で出し直す
  snprintf(buf, sizeof buf, "%.*g", prec, v);
  return Str(buf);
}

bool str_to_int(const Str& s, int64_t* out) {
  int i = 0, n = s.size();
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
  if (i >= n) return false;
  uint64_t acc = 0;
  int digits = 0;
  for (; i < n; i++) {
    char c = s[i];
    if (c < '0' || c > '9') break;
    uint64_t d = (uint64_t)(c - '0');
    if (acc > (0x7fffffffffffffffull - d) / 10) return false;  // あふれ
    acc = acc * 10 + d;
    digits++;
  }
  if (digits == 0) return false;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i != n) return false;
  *out = neg ? -(int64_t)acc : (int64_t)acc;
  return true;
}

// 10 のべき乗のうち、double でぴったり表せるもの（10^22 まで）
static const double kPow10[] = {
  1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// 数字は 64 ビット整数にためてから、10 のべき乗で寄せる。
// 「ぴったり表せる者どうしの1回の計算」に持ち込めるときは、それが最も正確
// （2^53 までの整数と 10^22 までのべき乗は、どちらも誤差なく持てる）。
bool str_to_float(const Str& s, double* out) {
  int i = 0, n = s.size();
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
  uint64_t mant = 0;
  int digits = 0, frac = 0, dropped = 0;
  for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
    digits++;
    if (mant < 1844674407370955161ull) mant = mant * 10 + (uint64_t)(s[i] - '0');
    else dropped++;                       // 入りきらない桁は指数に回す
  }
  if (i < n && s[i] == '.') {
    i++;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
      digits++;
      if (mant < 1844674407370955161ull) { mant = mant * 10 + (uint64_t)(s[i] - '0'); frac++; }
    }
  }
  if (digits == 0) return false;
  int exp = 0;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    bool eneg = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) { eneg = s[i] == '-'; i++; }
    int ed = 0, ev = 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) { ev = ev * 10 + (s[i] - '0'); ed++; if (ev > 100000) ev = 100000; }
    if (ed == 0) return false;
    exp = eneg ? -ev : ev;
  }
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i != n) return false;

  int e = exp + dropped - frac;          // 値は mant * 10^e
  double r;
  if (mant <= (1ull << 53) && e >= -22 && e <= 22) {
    r = e >= 0 ? (double)mant * kPow10[e] : (double)mant / kPow10[-e];
  } else {
    // 大きい・小さいときは 10^22 ずつ寄せる。一度に寄せると、途中で inf や 0 になってしまう
    r = (double)mant;
    int k = e;
    while (k > 0) {
      int step = k > 22 ? 22 : k;
      r *= kPow10[step];
      k -= step;
      if (!(r <= 1.7976931348623157e308)) break;   // inf まで行ったら、そこで終わり
    }
    while (k < 0 && r != 0.0) {
      int step = -k > 22 ? 22 : -k;
      r /= kPow10[step];
      k += step;
    }
  }
  *out = neg ? -r : r;
  return true;
}

}  // namespace shark

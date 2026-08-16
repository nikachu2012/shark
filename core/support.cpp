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

// 17 桁から短くしていき、元に戻る一番短い表現を選ぶ（環境差を出さないため自前で行う）
Str str_from_float(double v) {
  if (v != v) return Str("nan");
  if (v > 1.7976931348623157e308) return Str("inf");
  if (v < -1.7976931348623157e308) return Str("-inf");
  char buf[64];
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(buf, sizeof buf, "%.*g", prec, v);
    double back = 0;
    Str s(buf);
    if (str_to_float(s, &back) && back == v) break;
  }
  Str s(buf);
  // 指数も小数点も無ければ ".0" を足して float と分かるようにする
  bool has = false;
  for (int i = 0; i < s.size(); i++) if (s[i] == '.' || s[i] == 'e' || s[i] == 'n' || s[i] == 'i') has = true;
  if (!has) s += ".0";
  return s;
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

bool str_to_float(const Str& s, double* out) {
  int i = 0, n = s.size();
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  int start = i;
  bool neg = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
  double mant = 0;
  int digits = 0;
  for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) { mant = mant * 10 + (s[i] - '0'); digits++; }
  int frac = 0;
  if (i < n && s[i] == '.') {
    i++;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++) { mant = mant * 10 + (s[i] - '0'); digits++; frac++; }
  }
  if (digits == 0) { (void)start; return false; }
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
  int e = exp - frac;
  double r = mant;
  // 10 のべき乗を掛け算・割り算で寄せる（丸めは最近接偶数のまま）
  double p = 10.0;
  int k = e < 0 ? -e : e;
  double scale = 1.0;
  while (k) {
    if (k & 1) scale *= p;
    p *= p;
    k >>= 1;
  }
  if (e < 0) r /= scale; else r *= scale;
  *out = neg ? -r : r;
  return true;
}

}  // namespace shark

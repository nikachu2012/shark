// support.h — 最小限の器（Vec / Str / Map）
//
// 例外も RTTI も使わない（spec/skeleton.md）。C++ 標準ライブラリへの依存は、
// どの機種にもある範囲だけに絞る（spec/runtime/platform.md）。
#ifndef SHARK_SUPPORT_H
#define SHARK_SUPPORT_H

#include <new>
#include <stddef.h>
#include <stdint.h>

namespace shark {

// 確保・解放は移植層に投げる（platform/platform.h）
void* sk_alloc(size_t n);
void* sk_realloc(void* p, size_t n);
void  sk_free(void* p);
void  sk_fatal(const char* msg);

// --- メモリの上限 ---------------------------------------------------------
// 上限が見るのは「実行中のプログラムが使う量」だけ。
// 読み込みで作るもの（構文木・バイトコード・型の表）は数えない。
// 確保そのものは断らない（断ると、その場で書き換えのしようがなくなるため）。
// 仮想マシンは命令の切れ目でこれを見て、実行時エラーとして止める。
extern size_t sk_mem_used_v;      // 処理系が確保した量ぜんぶ（後始末の検査に使う）
extern size_t sk_mem_run_used_v;  // そのうち、実行中のプログラムのぶん
extern size_t sk_mem_limit_v;
extern int    sk_mem_phase_v;     // 0 = それ以外、1 = 実行中

void sk_mem_set_limit(size_t bytes);   // 0 は「上限なし」
// いまの区分を切り替える。前の区分を返すので、戻すときに使う
int  sk_mem_set_phase(int phase);

inline size_t sk_mem_used() { return sk_mem_used_v; }
inline size_t sk_mem_run_used() { return sk_mem_run_used_v; }
inline size_t sk_mem_limit() { return sk_mem_limit_v; }
inline bool   sk_mem_over() { return sk_mem_limit_v != 0 && sk_mem_run_used_v > sk_mem_limit_v; }

// 実行中だけ区分を切り替える（step() と start() で使う）
struct MemRunScope {
  int prev;
  MemRunScope() : prev(sk_mem_set_phase(1)) {}
  ~MemRunScope() { sk_mem_set_phase(prev); }
};

inline void sk_memcpy(void* d, const void* s, size_t n) {
  unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s;
  for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}
inline size_t sk_strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

// ---------------------------------------------------------------- Vec<T>
template <class T>
class Vec {
 public:
  Vec() : d_(0), n_(0), c_(0) {}
  Vec(const Vec& o) : d_(0), n_(0), c_(0) { reserve(o.n_); for (int i = 0; i < o.n_; i++) push(o.d_[i]); }
  Vec& operator=(const Vec& o) {
    if (this == &o) return *this;
    clear(); reserve(o.n_); for (int i = 0; i < o.n_; i++) push(o.d_[i]); return *this;
  }
  ~Vec() { clear(); sk_free(d_); }

  int size() const { return n_; }
  int capacity() const { return c_; }
  bool empty() const { return n_ == 0; }
  T& operator[](int i) { return d_[i]; }
  const T& operator[](int i) const { return d_[i]; }
  T& back() { return d_[n_ - 1]; }
  const T& back() const { return d_[n_ - 1]; }
  T* data() { return d_; }
  const T* data() const { return d_; }

  void reserve(int c) {
    if (c <= c_) return;
    int nc = c_ ? c_ * 2 : 4;
    if (nc < c) nc = c;
    T* nd = (T*)sk_alloc(sizeof(T) * (size_t)nc);
    for (int i = 0; i < n_; i++) { new (&nd[i]) T(d_[i]); d_[i].~T(); }
    sk_free(d_); d_ = nd; c_ = nc;
  }
  void push(const T& v) { reserve(n_ + 1); new (&d_[n_]) T(v); n_++; }
  void pop() { n_--; d_[n_].~T(); }
  void insert(int i, const T& v) {
    reserve(n_ + 1); new (&d_[n_]) T(v); n_++;
    for (int k = n_ - 1; k > i; k--) { T t = d_[k]; d_[k] = d_[k - 1]; d_[k - 1] = t; }
  }
  void remove(int i) { for (int k = i; k + 1 < n_; k++) d_[k] = d_[k + 1]; pop(); }
  void clear() { for (int i = 0; i < n_; i++) d_[i].~T(); n_ = 0; }
  void resize(int n, const T& fill) { while (n_ > n) pop(); while (n_ < n) push(fill); }
  void swap_with(Vec& o) {
    T* d = d_; int n = n_, c = c_;
    d_ = o.d_; n_ = o.n_; c_ = o.c_;
    o.d_ = d; o.n_ = n; o.c_ = c;
  }

 private:
  T* d_; int n_; int c_;
};

// ---------------------------------------------------------------- Str
// UTF-8 のバイト列を持つ。末尾に 0 を置くので c_str() が使える。
class Str {
 public:
  Str() : d_(0), n_(0), c_(0) {}
  Str(const char* s) : d_(0), n_(0), c_(0) { append(s, (int)sk_strlen(s)); }
  Str(const char* s, int n) : d_(0), n_(0), c_(0) { append(s, n); }
  Str(const Str& o) : d_(0), n_(0), c_(0) { append(o.d_, o.n_); }
  Str& operator=(const Str& o) { if (this != &o) { n_ = 0; append(o.d_, o.n_); } return *this; }
  ~Str() { sk_free(d_); }

  int size() const { return n_; }
  bool empty() const { return n_ == 0; }
  const char* c_str() const { return d_ ? d_ : ""; }
  const char* data() const { return d_ ? d_ : ""; }
  char operator[](int i) const { return d_[i]; }
  char& operator[](int i) { return d_[i]; }

  void clear() { n_ = 0; if (d_) d_[0] = 0; }
  void reserve(int c) {
    if (c + 1 <= c_) return;
    int nc = c_ ? c_ * 2 : 16;
    if (nc < c + 1) nc = c + 1;
    d_ = (char*)sk_realloc(d_, (size_t)nc); c_ = nc;
  }
  void append(const char* s, int n) { if (n <= 0) return; reserve(n_ + n); sk_memcpy(d_ + n_, s, (size_t)n); n_ += n; d_[n_] = 0; }
  void append(const Str& o) { append(o.d_, o.n_); }
  void append(const char* s) { append(s, (int)sk_strlen(s)); }
  void push(char c) { reserve(n_ + 1); d_[n_++] = c; d_[n_] = 0; }
  Str sub(int start, int len) const { if (start < 0) start = 0; if (start + len > n_) len = n_ - start; if (len < 0) len = 0; return Str(d_ + start, len); }

  Str& operator+=(const Str& o) { append(o); return *this; }
  Str& operator+=(const char* s) { append(s); return *this; }
  Str& operator+=(char c) { push(c); return *this; }

  bool operator==(const Str& o) const {
    if (n_ != o.n_) return false;
    for (int i = 0; i < n_; i++) if (d_[i] != o.d_[i]) return false;
    return true;
  }
  bool operator!=(const Str& o) const { return !(*this == o); }
  bool operator==(const char* s) const {
    int i = 0; for (; s[i]; i++) { if (i >= n_ || d_[i] != s[i]) return false; }
    return i == n_;
  }
  bool operator!=(const char* s) const { return !(*this == s); }
  // コードポイント順ではなくバイト順（UTF-8 はバイト順＝コードポイント順）
  int cmp(const Str& o) const {
    int n = n_ < o.n_ ? n_ : o.n_;
    for (int i = 0; i < n; i++) {
      unsigned char a = (unsigned char)d_[i], b = (unsigned char)o.d_[i];
      if (a != b) return a < b ? -1 : 1;
    }
    if (n_ == o.n_) return 0;
    return n_ < o.n_ ? -1 : 1;
  }
  uint64_t hash() const {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n_; i++) { h ^= (unsigned char)d_[i]; h *= 1099511628211ull; }
    return h;
  }

 private:
  char* d_; int n_; int c_;
};

inline Str operator+(const Str& a, const Str& b) { Str r(a); r.append(b); return r; }
inline Str operator+(const Str& a, const char* b) { Str r(a); r.append(b); return r; }

Str str_from_int(int64_t v);
Str str_from_uint_base(uint64_t v, int base, bool upper);
// 整数の値でも "1.0" のように小数点を残す（float であることが読んで分かるように）
Str str_from_float(double v);
bool str_to_int(const Str& s, int64_t* out);
bool str_to_float(const Str& s, double* out);

// ---------------------------------------------------------------- 順序つきの表
// 挿入順を保つ（spec/types/collection.md）。索引は開番地法の表。
template <class K, class V>
class OrderedMap {
 public:
  struct Entry { K key; V val; bool dead; Entry() : dead(false) {} };
  OrderedMap() {}
  int size() const { return count_; }
  const Vec<Entry>& entries() const { return e_; }
  Vec<Entry>& entries() { return e_; }

  V* find(const K& k) {
    for (int i = 0; i < e_.size(); i++) if (!e_[i].dead && e_[i].key == k) return &e_[i].val;
    return 0;
  }
  const V* find(const K& k) const {
    for (int i = 0; i < e_.size(); i++) if (!e_[i].dead && e_[i].key == k) return &e_[i].val;
    return 0;
  }
  void set(const K& k, const V& v) {
    V* p = find(k); if (p) { *p = v; return; }
    Entry en; en.key = k; en.val = v; e_.push(en); count_++;
  }
  bool erase(const K& k) {
    for (int i = 0; i < e_.size(); i++) if (!e_[i].dead && e_[i].key == k) { e_[i].dead = true; count_--; return true; }
    return false;
  }

 private:
  Vec<Entry> e_; int count_ = 0;
};

}  // namespace shark

#endif

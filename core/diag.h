// diag.h — 診断（spec/runtime/diagnostics.md）
//
// コアは文字を整形しない。3点セット（何が起きたか／どこか／どう直すか）を
// 構造化データのまま返す。並べ方は受け取った側が決める。
#ifndef SHARK_DIAG_H
#define SHARK_DIAG_H

#include "support.h"

namespace shark {

enum Lang { LANG_JA = 0, LANG_EN = 1 };
enum Severity { SEV_ERROR = 0, SEV_WARNING = 1 };

struct Span {
  int line;    // 1 から
  int col;     // 1 から（文字単位）
  int len;     // 文字数
  Str label;   // そこに付ける短い説明（空でもよい）
  Span() : line(0), col(0), len(0) {}
  Span(int l, int c, int n) : line(l), col(c), len(n) {}
  Span(int l, int c, int n, const Str& s) : line(l), col(c), len(n), label(s) {}
};

struct Diagnostic {
  Severity severity;
  Str code;      // "E0102"
  Str message;   // 専門用語を避けた1行
  Str file;      // load() に渡された名前
  Vec<Span> spans;
  Vec<Str> help; // 直し方。複数のこともある
  Diagnostic() : severity(SEV_ERROR) {}
};

// 診断をためる場所
class DiagBag {
 public:
  DiagBag() : lang_(LANG_JA), strict_(false) {}
  void set_lang(Lang l) { lang_ = l; }
  Lang lang() const { return lang_; }
  void set_strict(bool s) { strict_ = s; }
  void set_file(const Str& f) { file_ = f; }
  const Str& file() const { return file_; }

  Diagnostic& error(const char* code, const Str& msg);
  Diagnostic& warn(const char* code, const Str& msg);

  int size() const { return items_.size(); }
  const Diagnostic& operator[](int i) const { return items_[i]; }
  Vec<Diagnostic>& items() { return items_; }
  bool has_error() const;
  void clear() { items_.clear(); }

  // 言語で選ぶ
  Str L(const char* ja, const char* en) const { return Str(lang_ == LANG_JA ? ja : en); }
  Str L(const Str& ja, const Str& en) const { return lang_ == LANG_JA ? ja : en; }

 private:
  Vec<Diagnostic> items_;
  Lang lang_;
  bool strict_;
  Str file_;
};

// 番号ごとの詳しい説明（shark explain E0102 で使う）
const char* diag_explain(const char* code, Lang lang);

}  // namespace shark
#endif

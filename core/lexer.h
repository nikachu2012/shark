// lexer.h — 字句解析（spec/syntax.md）
#ifndef SHARK_LEXER_H
#define SHARK_LEXER_H

#include "diag.h"
#include "support.h"

namespace shark {

enum TokKind : uint8_t {
  TK_EOF = 0, TK_Ident, TK_Int, TK_Float, TK_Str, TK_FStr, TK_Bytes,
  // 記号
  TK_LParen, TK_RParen, TK_LBrace, TK_RBrace, TK_LBracket, TK_RBracket,
  TK_Comma, TK_Semi, TK_Colon, TK_Dot, TK_Arrow, TK_Question, TK_QDot, TK_QQ,
  TK_Bang, TK_Plus, TK_Minus, TK_Star, TK_Slash, TK_Percent,
  TK_Assign, TK_PlusAssign, TK_MinusAssign, TK_StarAssign, TK_SlashAssign,
  TK_Eq, TK_Ne, TK_Lt, TK_Le, TK_Gt, TK_Ge, TK_AndAnd, TK_OrOr, TK_Amp, TK_Plus2,
  // ビット演算と冪乗（>> は < > の入れ子と紛れるので、記号としては作らない。
  // 並んだ 2 つの > を構文解析でまとめる）
  TK_Pipe, TK_Caret, TK_Tilde, TK_Shl, TK_Star2,
  TK_AmpAssign, TK_PipeAssign, TK_CaretAssign, TK_ShlAssign,
  // 予約語
  TK_Func, TK_Return, TK_Var, TK_Const, TK_If, TK_Else, TK_While, TK_For, TK_In,
  TK_Break, TK_Continue, TK_Class, TK_This, TK_Super, TK_ThisType,
  TK_Public, TK_Private, TK_Virtual, TK_Override, TK_Ref, TK_Import, TK_As,
  TK_Task, TK_Parallel, TK_Try, TK_Panic, TK_True, TK_False, TK_None,
};

struct Token {
  TokKind kind;
  Str text;     // 識別子、文字列の中身、予約語の綴り
  int64_t ival;
  double dval;
  int line, col, len;
  int offset;   // ソース先頭からのバイト位置（f 文字列の解析に使う）
  Token() : kind(TK_EOF), ival(0), dval(0), line(1), col(1), len(0), offset(0) {}
};

class Lexer {
 public:
  Lexer(const Str& src, DiagBag& diag, int line0 = 1, int col0 = 1);
  // 全部読み切る。誤りがあれば diag に入れ、そこまでを返す
  void run(Vec<Token>* out);

 private:
  void push(Vec<Token>* out, TokKind k, int line, int col, int start);
  bool read_escape(Str* out, bool bytes_mode);
  const Str& s_;
  DiagBag& diag_;
  int i_, line_, col_;
};

const char* tok_name(TokKind k);

}  // namespace shark
#endif

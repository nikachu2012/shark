#include "lexer.h"

namespace shark {

// UTF-8 に直して足す（value.cpp と同じ規則。字句解析は値に依存しない）
static void utf8_encode_into(Str& out, int cp) {
  if (cp < 0x80) out.push((char)cp);
  else if (cp < 0x800) {
    out.push((char)(0xC0 | (cp >> 6)));
    out.push((char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push((char)(0xE0 | (cp >> 12)));
    out.push((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push((char)(0x80 | (cp & 0x3F)));
  } else {
    out.push((char)(0xF0 | (cp >> 18)));
    out.push((char)(0x80 | ((cp >> 12) & 0x3F)));
    out.push((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push((char)(0x80 | (cp & 0x3F)));
  }
}

struct Keyword { const char* w; TokKind k; };
static const Keyword kKeywords[] = {
  {"func", TK_Func}, {"return", TK_Return}, {"var", TK_Var}, {"const", TK_Const},
  {"if", TK_If}, {"else", TK_Else}, {"while", TK_While}, {"for", TK_For}, {"in", TK_In},
  {"break", TK_Break}, {"continue", TK_Continue}, {"class", TK_Class}, {"this", TK_This},
  {"super", TK_Super}, {"This", TK_ThisType}, {"public", TK_Public}, {"private", TK_Private},
  {"virtual", TK_Virtual}, {"override", TK_Override}, {"ref", TK_Ref}, {"import", TK_Import},
  {"as", TK_As}, {"task", TK_Task}, {"parallel", TK_Parallel}, {"try", TK_Try},
  {"panic", TK_Panic}, {"true", TK_True}, {"false", TK_False}, {"none", TK_None},
};

const char* tok_name(TokKind k) {
  switch (k) {
    case TK_EOF: return "ファイルの終わり";
    case TK_Ident: return "名前";
    case TK_Int: return "整数";
    case TK_Float: return "小数";
    case TK_Str: return "文字列";
    case TK_FStr: return "f 文字列";
    case TK_Bytes: return "バイト列";
    case TK_LParen: return "(";
    case TK_RParen: return ")";
    case TK_LBrace: return "{";
    case TK_RBrace: return "}";
    case TK_LBracket: return "[";
    case TK_RBracket: return "]";
    case TK_Comma: return ",";
    case TK_Semi: return ";";
    case TK_Colon: return ":";
    case TK_Dot: return ".";
    case TK_Arrow: return "->";
    case TK_Question: return "?";
    case TK_QDot: return "?.";
    case TK_QQ: return "??";
    case TK_Bang: return "!";
    case TK_Plus: return "+";
    case TK_Minus: return "-";
    case TK_Star: return "*";
    case TK_Slash: return "/";
    case TK_Percent: return "%";
    case TK_Assign: return "=";
    case TK_PlusAssign: return "+=";
    case TK_MinusAssign: return "-=";
    case TK_StarAssign: return "*=";
    case TK_SlashAssign: return "/=";
    case TK_Eq: return "==";
    case TK_Ne: return "!=";
    case TK_Lt: return "<";
    case TK_Le: return "<=";
    case TK_Gt: return ">";
    case TK_Ge: return ">=";
    case TK_AndAnd: return "&&";
    case TK_OrOr: return "||";
    case TK_Amp: return "&";
    case TK_Plus2: return "+";
    default: break;
  }
  for (unsigned i = 0; i < sizeof(kKeywords) / sizeof(kKeywords[0]); i++)
    if (kKeywords[i].k == k) return kKeywords[i].w;
  return "?";
}

static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int hex_val(char c) {
  if (c <= '9') return c - '0';
  if (c <= 'F') return c - 'A' + 10;
  return c - 'a' + 10;
}

Lexer::Lexer(const Str& src, DiagBag& diag, int line0, int col0)
    : s_(src), diag_(diag), i_(0), line_(line0), col_(col0) {}

void Lexer::push(Vec<Token>* out, TokKind k, int line, int col, int start) {
  Token t;
  t.kind = k;
  t.line = line;
  t.col = col;
  t.offset = start;
  t.len = i_ - start;
  out->push(t);
}

bool Lexer::read_escape(Str* out, bool bytes_mode) {
  // s_[i_] は '\\' の次の文字
  char c = s_[i_];
  i_++; col_++;
  switch (c) {
    case 'n': out->push('\n'); return true;
    case 't': out->push('\t'); return true;
    case 'r': out->push('\r'); return true;
    case '0': out->push('\0'); return true;
    case '\\': out->push('\\'); return true;
    case '"': out->push('"'); return true;
    case '\'': out->push('\''); return true;
    case 'x': {
      if (i_ + 1 < s_.size() && is_hex(s_[i_]) && is_hex(s_[i_ + 1])) {
        int v = hex_val(s_[i_]) * 16 + hex_val(s_[i_ + 1]);
        i_ += 2; col_ += 2;
        out->push((char)v);
        return true;
      }
      Diagnostic& d = diag_.error("E0002", diag_.L("\\x の後ろには 16 進 2 桁が要ります",
                                                   "\\x needs two hexadecimal digits"));
      d.spans.push(Span(line_, col_, 2));
      d.help.push(diag_.L("例: \\x41", "example: \\x41"));
      return false;
    }
    case 'u': {
      if (i_ < s_.size() && s_[i_] == '{') {
        i_++; col_++;
        int v = 0, n = 0;
        while (i_ < s_.size() && is_hex(s_[i_])) { v = v * 16 + hex_val(s_[i_]); i_++; col_++; n++; }
        if (n > 0 && i_ < s_.size() && s_[i_] == '}') {
          i_++; col_++;
          if (bytes_mode) { out->push((char)(v & 0xff)); return true; }
          utf8_encode_into(*out, v);
          return true;
        }
      }
      Diagnostic& d = diag_.error("E0002", diag_.L("\\u{...} の書き方が違います", "malformed \\u{...}"));
      d.spans.push(Span(line_, col_, 2));
      d.help.push(diag_.L("例: \\u{3042}", "example: \\u{3042}"));
      return false;
    }
    default: {
      Diagnostic& d = diag_.error("E0002", diag_.L("知らないエスケープです", "unknown escape sequence"));
      d.spans.push(Span(line_, col_ - 1, 2));
      d.help.push(diag_.L("使えるのは \\n \\t \\r \\\\ \\\" \\0 \\xNN \\u{XXXX} です",
                          "allowed: \\n \\t \\r \\\\ \\\" \\0 \\xNN \\u{XXXX}"));
      return false;
    }
  }
}

void Lexer::run(Vec<Token>* out) {
  // BOM は読み飛ばす
  if (s_.size() >= 3 && (unsigned char)s_[0] == 0xEF && (unsigned char)s_[1] == 0xBB &&
      (unsigned char)s_[2] == 0xBF)
    i_ = 3;

  while (i_ < s_.size()) {
    char c = s_[i_];
    // 空白
    if (c == ' ' || c == '\t' || c == '\r') { i_++; col_++; continue; }
    if (c == '\n') { i_++; line_++; col_ = 1; continue; }
    // コメント
    if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
      while (i_ < s_.size() && s_[i_] != '\n') i_++;
      continue;
    }
    if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
      int depth = 0;
      int sl = line_, sc = col_;
      while (i_ < s_.size()) {
        if (s_[i_] == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') { depth++; i_ += 2; col_ += 2; continue; }
        if (s_[i_] == '*' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
          depth--; i_ += 2; col_ += 2;
          if (depth == 0) break;
          continue;
        }
        if (s_[i_] == '\n') { line_++; col_ = 1; } else col_++;
        i_++;
      }
      if (depth != 0) {
        Diagnostic& d = diag_.error("E0002", diag_.L("囲みコメントが閉じていません", "unterminated block comment"));
        d.spans.push(Span(sl, sc, 2));
        d.help.push(diag_.L("*/ で閉じます", "close it with */"));
      }
      continue;
    }

    int line = line_, col = col_, start = i_;

    // 名前と予約語（f" b" は文字列なので除く）
    if (is_alpha(c) && !((c == 'f' || c == 'b') && i_ + 1 < s_.size() && s_[i_ + 1] == '"')) {
      while (i_ < s_.size() && (is_alpha(s_[i_]) || is_digit(s_[i_]))) { i_++; col_++; }
      Str w = s_.sub(start, i_ - start);
      TokKind k = TK_Ident;
      for (unsigned n = 0; n < sizeof(kKeywords) / sizeof(kKeywords[0]); n++)
        if (w == kKeywords[n].w) { k = kKeywords[n].k; break; }
      push(out, k, line, col, start);
      out->back().text = w;
      continue;
    }

    // 数
    if (is_digit(c)) {
      bool is_float = false;
      Str digits;
      if (c == '0' && i_ + 1 < s_.size() && (s_[i_ + 1] == 'x' || s_[i_ + 1] == 'X')) {
        i_ += 2; col_ += 2;
        uint64_t v = 0;
        while (i_ < s_.size() && (is_hex(s_[i_]) || s_[i_] == '_')) {
          if (s_[i_] != '_') v = v * 16 + (uint64_t)hex_val(s_[i_]);
          i_++; col_++;
        }
        push(out, TK_Int, line, col, start);
        out->back().ival = (int64_t)v;
        continue;
      }
      if (c == '0' && i_ + 1 < s_.size() && (s_[i_ + 1] == 'b' || s_[i_ + 1] == 'B')) {
        i_ += 2; col_ += 2;
        uint64_t v = 0;
        while (i_ < s_.size() && (s_[i_] == '0' || s_[i_] == '1' || s_[i_] == '_')) {
          if (s_[i_] != '_') v = v * 2 + (uint64_t)(s_[i_] - '0');
          i_++; col_++;
        }
        push(out, TK_Int, line, col, start);
        out->back().ival = (int64_t)v;
        continue;
      }
      while (i_ < s_.size() && (is_digit(s_[i_]) || s_[i_] == '_')) {
        if (s_[i_] != '_') digits.push(s_[i_]);
        i_++; col_++;
      }
      if (i_ + 1 < s_.size() && s_[i_] == '.' && is_digit(s_[i_ + 1])) {
        is_float = true;
        digits.push('.');
        i_++; col_++;
        while (i_ < s_.size() && (is_digit(s_[i_]) || s_[i_] == '_')) {
          if (s_[i_] != '_') digits.push(s_[i_]);
          i_++; col_++;
        }
      }
      if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
        int save = i_, savec = col_;
        Str exp;
        exp.push('e');
        i_++; col_++;
        if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) { exp.push(s_[i_]); i_++; col_++; }
        if (i_ < s_.size() && is_digit(s_[i_])) {
          while (i_ < s_.size() && is_digit(s_[i_])) { exp.push(s_[i_]); i_++; col_++; }
          digits += exp;
          is_float = true;
        } else { i_ = save; col_ = savec; }
      }
      push(out, is_float ? TK_Float : TK_Int, line, col, start);
      if (is_float) {
        double d = 0;
        str_to_float(digits, &d);
        out->back().dval = d;
      } else {
        int64_t v = 0;
        if (!str_to_int(digits, &v)) {
          Diagnostic& dg = diag_.error("E0002", diag_.L("整数が大きすぎます（int は 64 ビットです）",
                                                        "integer literal is too large for 64-bit int"));
          dg.spans.push(Span(line, col, i_ - start));
          dg.help.push(diag_.L("float として書くこともできます: 例 1.0e19",
                               "write it as a float instead, e.g. 1.0e19"));
        }
        out->back().ival = v;
      }
      continue;
    }

    // 文字列・バイト列・f 文字列
    if (c == '"' || ((c == 'b' || c == 'f') && i_ + 1 < s_.size() && s_[i_ + 1] == '"')) {
      bool bytes_mode = (c == 'b');
      bool fmt_mode = (c == 'f');
      if (bytes_mode || fmt_mode) { i_++; col_++; }
      i_++; col_++;  // 開きの "
      Str body;
      bool closed = false;
      while (i_ < s_.size()) {
        char d = s_[i_];
        if (d == '"') { i_++; col_++; closed = true; break; }
        if (d == '\n') break;
        if (d == '\\') {
          i_++; col_++;
          if (i_ >= s_.size()) break;
          if (fmt_mode) {
            // f 文字列は後で分解するので、エスケープはそのまま残す
            body.push('\\');
            body.push(s_[i_]);
            i_++; col_++;
            continue;
          }
          if (!read_escape(&body, bytes_mode)) break;
          continue;
        }
        body.push(d);
        i_++; col_++;
      }
      if (!closed) {
        Diagnostic& d = diag_.error("E0002", diag_.L("文字列が閉じていません", "unterminated string"));
        d.spans.push(Span(line, col, 1));
        d.help.push(diag_.L("\" で閉じます。改行をまたぐ文字列は書けません",
                            "close it with \". strings cannot span lines"));
      }
      push(out, bytes_mode ? TK_Bytes : (fmt_mode ? TK_FStr : TK_Str), line, col, start);
      out->back().text = body;
      continue;
    }

    // 記号
    struct Sym { const char* s; TokKind k; };
    static const Sym syms[] = {
      {"->", TK_Arrow}, {"??", TK_QQ}, {"?.", TK_QDot}, {"==", TK_Eq}, {"!=", TK_Ne},
      {"<=", TK_Le}, {">=", TK_Ge}, {"&&", TK_AndAnd}, {"||", TK_OrOr},
      {"+=", TK_PlusAssign}, {"-=", TK_MinusAssign}, {"*=", TK_StarAssign}, {"/=", TK_SlashAssign},
      {"(", TK_LParen}, {")", TK_RParen}, {"{", TK_LBrace}, {"}", TK_RBrace},
      {"[", TK_LBracket}, {"]", TK_RBracket}, {",", TK_Comma}, {";", TK_Semi},
      {":", TK_Colon}, {".", TK_Dot}, {"?", TK_Question}, {"!", TK_Bang},
      {"+", TK_Plus}, {"-", TK_Minus}, {"*", TK_Star}, {"/", TK_Slash}, {"%", TK_Percent},
      {"=", TK_Assign}, {"<", TK_Lt}, {">", TK_Gt}, {"&", TK_Amp},
    };
    bool matched = false;
    for (unsigned n = 0; n < sizeof(syms) / sizeof(syms[0]); n++) {
      const char* sy = syms[n].s;
      int len = (int)sk_strlen(sy);
      if (i_ + len > s_.size()) continue;
      bool eq = true;
      for (int k = 0; k < len; k++) if (s_[i_ + k] != sy[k]) { eq = false; break; }
      if (!eq) continue;
      i_ += len; col_ += len;
      push(out, syms[n].k, line, col, start);
      matched = true;
      break;
    }
    if (matched) continue;

    Diagnostic& d = diag_.error("E0002", diag_.L("ここには書けない文字があります", "unexpected character"));
    d.spans.push(Span(line, col, 1));
    d.help.push(diag_.L("識別子に使えるのは英数字と _ だけです（日本語は使えません）",
                        "identifiers may only use ASCII letters, digits and _"));
    i_++; col_++;
  }

  Token t;
  t.kind = TK_EOF;
  t.line = line_;
  t.col = col_;
  t.offset = i_;
  out->push(t);
}

}  // namespace shark

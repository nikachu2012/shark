// fmt_src.cpp — ソースの見た目を整える（shark fmt）
//
// 「書いた人の改行はそのまま、字下げと空きだけを揃える」整え方にしてある。
// 行の折り返しまで決めてしまうと、表に並べた配列や、そろえたコメントといった
// **人が読みやすくするために置いた形**が壊れるため。
//
//   ・字下げは 2 つ。続きの行は、囲みの中なら**囲みに合わせ**、
//     囲みの外なら 4 つ下げる
//   ・囲み（( [ {）が行の終わりにあれば「ぶら下げ」（中身は 2 つ下げ）、
//     行の途中にあれば「そろえ」（中身は囲みのすぐ右に合わせる）。
//     この2つで、いまある書き方をそのまま直せる
//   ・空き行は 2 行まで。行末の空白は落とし、終わりは改行1つ
//   ・コメントは中身に触らない。行の終わりに付けたコメントは、
//     もとの空きぶんを残す（そろえてあるものが崩れない）
//   ・`<` と `>` のまわりも、もとの空きを残す。`list<int>` の囲みなのか
//     大小くらべなのかは、字句だけでは決められないため
//
// 整えたものは**もう一度読み直して**、字句の並びがもとと同じか確かめる。
// 違っていれば整えずにもとを返す（意味を変えないことを、こちらで確かめる）。
#include "fmt_src.h"

#include "diag.h"
#include "lexer.h"

namespace shark {
namespace {

const int kIndent = 2;      // 字下げ1つぶん
const int kContinue = 4;    // 囲みの外で行が続くときの下げぶん

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool utf8_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

// その字句の、ソースにあったままの文字。次の字句の始まりまでを取り、
// 後ろの空白を落とす（コメントも字句として出させているので、間には空白しかない）
Str raw_of(const Str& src, const Vec<Token>& t, int i) {
  int a = t[i].offset;
  int b = (i + 1 < t.size()) ? t[i + 1].offset : src.size();
  if (b > src.size()) b = src.size();
  if (a > b) a = b;
  while (b > a && is_space(src[b - 1])) b--;
  return src.sub(a, b - a);
}

int count_lines(const Str& s) {
  int n = 0;
  for (int i = 0; i < s.size(); i++) if (s[i] == '\n') n++;
  return n;
}
int count_chars(const Str& s) {
  int n = 0;
  for (int i = 0; i < s.size(); i++) if (!utf8_cont(s[i])) n++;
  return n;
}

// 値の終わり。この後ろの ( は呼び出し、- は二項の引き算になる
bool ends_value(TokKind k) {
  switch (k) {
    case TK_Ident: case TK_Int: case TK_Float: case TK_Str: case TK_FStr: case TK_Bytes:
    case TK_RParen: case TK_RBracket: case TK_True: case TK_False: case TK_None:
    case TK_This: case TK_Super: case TK_ThisType: case TK_Bang: case TK_Question:
      return true;
    default:
      return false;
  }
}

// 前後に空きを置く二項の記号
bool binary_op(TokKind k) {
  switch (k) {
    case TK_Plus: case TK_Minus: case TK_Star: case TK_Slash: case TK_Percent:
    case TK_Assign: case TK_PlusAssign: case TK_MinusAssign: case TK_StarAssign:
    case TK_SlashAssign: case TK_Eq: case TK_Ne: case TK_Le: case TK_Ge:
    case TK_AndAnd: case TK_OrOr: case TK_Amp: case TK_Pipe: case TK_Caret:
    case TK_Shl: case TK_Star2: case TK_AmpAssign: case TK_PipeAssign:
    case TK_CaretAssign: case TK_ShlAssign: case TK_Arrow: case TK_QQ: case TK_Plus2:
      return true;
    default:
      return false;
  }
}

// 後ろに空きを置く予約語（if x { … の x の前）
bool word_needs_space(TokKind k) {
  switch (k) {
    case TK_Func: case TK_Return: case TK_Var: case TK_Const: case TK_If: case TK_Else:
    case TK_While: case TK_For: case TK_In: case TK_Class: case TK_Public: case TK_Private:
    case TK_Virtual: case TK_Override: case TK_Ref: case TK_Import: case TK_As:
    case TK_Task: case TK_Parallel: case TK_Try: case TK_Panic:
      return true;
    default:
      return false;
  }
}

// 名前・数・予約語のような「語」。語どうしが並んだら、必ず空きが要る
// （i in、else if、var x、-> int の int など）
bool is_word(TokKind k) {
  if (k == TK_Ident || k == TK_Int || k == TK_Float) return true;
  return k >= TK_Func && k <= TK_None;
}

bool is_open(TokKind k) { return k == TK_LParen || k == TK_LBracket || k == TK_LBrace; }
bool is_close(TokKind k) { return k == TK_RParen || k == TK_RBracket || k == TK_RBrace; }
TokKind closer_of(TokKind k) {
  if (k == TK_LParen) return TK_RParen;
  if (k == TK_LBracket) return TK_RBracket;
  return TK_RBrace;
}

// 同じ行に並ぶ2つの字句のあいだに、空きを置くか。
// gap は、もとのソースでそこに空いていた字の数（`<` などで使う）
// p_prefix は「p が前に付く記号（単項）か」。1つ前の字句で決まるので、呼ぶ側が渡す
bool space_between(TokKind p, TokKind c, int gap, bool p_prefix, bool in_class) {
  // コメントは中身に触らない。前後の空きは、もとのままにしたいので呼ぶ側で見る
  if (p == TK_Comment) return true;
  if (c == TK_Comment) return true;

  if (p == TK_LParen || p == TK_LBracket) return false;   // ( [ のすぐ後ろは詰める
  if (c == TK_RParen || c == TK_RBracket) return false;   // ) ] の手前も詰める
  if (c == TK_Comma || c == TK_Semi) return false;
  // : は「var x: int」では手前を詰め、「class Shark : Fish」では空ける
  if (c == TK_Colon) return in_class;
  if (p == TK_Dot || c == TK_Dot || p == TK_QDot || c == TK_QDot) return false;
  if (c == TK_Question) return false;                     // int? の ?
  if (c == TK_Bang && ends_value(p)) return false;        // うしろに付く ! （断言）
  if (p == TK_Ellipsis || c == TK_Ellipsis) return false;

  // 大小くらべと、型の囲み（list<int>）は字句だけでは見分けられない。
  // もとの空きをそのまま残す
  if (p == TK_Lt || c == TK_Lt || p == TK_Gt || c == TK_Gt) return gap > 0;

  // 呼び出しの ( と、添字の [
  if (c == TK_LParen && (ends_value(p) || p == TK_Bang)) return false;
  if (c == TK_LBracket && ends_value(p)) return false;

  // 前に付く記号（+ - ! ~ & * は、値の後ろでなければ「前に付く」）
  if (p_prefix && (p == TK_Plus || p == TK_Minus || p == TK_Bang || p == TK_Tilde ||
                   p == TK_Amp || p == TK_Star))
    return false;


  if (c == TK_LParen && p == TK_Panic) return false;   // panic("…") は呼び出しの形
  // 中かっこのうしろに呼び出しや添字が続くのは、名前のない関数を呼ぶとき
  if (p == TK_RBrace && (c == TK_LParen || c == TK_LBracket)) return false;
  if (is_word(p) && is_word(c)) return true;           // 語どうしは、必ず空ける
  if (p == TK_RBrace) return true;                     // } のうしろ（} else など）
  if (p == TK_Func && c == TK_LParen) return false;   // 名前のない関数（func() -> void）
  if (binary_op(p) || binary_op(c)) return true;
  if (word_needs_space(p)) return true;
  if (c == TK_LBrace) return true;
  if (p == TK_Comma || p == TK_Semi || p == TK_Colon) return true;
  if (p == TK_RParen || p == TK_RBracket) {
    // )( や )[ は続けて書く。それ以外は前後の決まりに任せる
    return !(c == TK_LParen || c == TK_LBracket);
  }
  return false;
}

}  // namespace

Str format_source(const Str& src, bool* ok) {
  if (ok) *ok = false;
  DiagBag diag;
  Vec<Token> t;
  {
    Lexer lx(src, diag);
    lx.keep_comments(true);
    lx.run(&t);
  }
  if (diag.has_error()) return src;   // 読めないものは触らない
  if (t.size() <= 1) {   // 中身が無い（EOF だけ）
    if (ok) *ok = true;
    return src;
  }

  // 字句ごとの、もとのままの文字と、終わりの行
  Vec<Str> raw;
  Vec<int> end_line;
  for (int i = 0; i < t.size(); i++) {
    raw.push(raw_of(src, t, i));
    end_line.push(t[i].line + count_lines(raw[i]));
  }

  struct Open {
    TokKind kind;
    int cont;    // 続きの行の字下げ
    int close;   // 閉じで始まる行の字下げ
    bool block;  // { が「文のかたまり」か（map の中身なら false）
    bool hang;   // 行の終わりに開いたか（中身をぶら下げる）
  };
  Vec<Open> st;

  Str out;
  int col = 0;          // いま出している行の桁（字の数）
  int line_indent = 0;  // いま出している行の字下げ
  int stmt_indent = 0;  // いまの文が始まった行の字下げ（囲みの中身は、ここから下げる）
  int last = -1;        // 直前に出した字句
  bool in_import = false;   // import 文の中（道の書き方は、もとのまま残す）
  bool in_class = false;    // class の見出しの中（: の手前を空ける）

  for (int i = 0; i + 1 < t.size(); i++) {   // EOF は出さない
    TokKind k = t[i].kind;
    bool head = (last < 0) || t[i].line > end_line[last];

    // 文の始まりかどうか（区切りのあとに来た字句）。字下げの拠りどころになる
    bool starts_stmt = last < 0 || t[last].kind == TK_Semi || t[last].kind == TK_LBrace ||
                       t[last].kind == TK_RBrace || t[last].kind == TK_Comment;

    if (head) {
      // 行を改める。空き行は 1 行まで残す
      if (last >= 0) {
        // 空き行は 2 行まで残す（大きな区切りに 2 行空ける書き方があるため）
        int blanks = t[i].line - end_line[last] - 1;
        out += "\n";
        if (blanks > 0) out += "\n";
        if (blanks > 1) out += "\n";
      }
      // この行の字下げ
      int ind = 0;
      bool closer = is_close(k) && st.size() > 0 && closer_of(st[st.size() - 1].kind) == k;
      if (st.size() > 0) {
        ind = closer ? st[st.size() - 1].close : st[st.size() - 1].cont;
      }
      // 途中で折り返した行は、もう少し下げる。
      // ただし、囲みのすぐ右にそろえてあるところ（そろえ）は、そのまま
      bool aligned = st.size() > 0 && !st[st.size() - 1].hang &&
                     (st[st.size() - 1].kind == TK_LParen ||
                      st[st.size() - 1].kind == TK_LBracket);
      bool new_item = starts_stmt ||
                      (last >= 0 && (t[last].kind == TK_Comma || is_open(t[last].kind)));
      if (!closer && !aligned && !new_item) ind += kContinue;
      for (int s = 0; s < ind; s++) out += " ";
      col = ind;
      line_indent = ind;
      if (starts_stmt) stmt_indent = ind;
    } else if (last >= 0) {
      int prev_end = t[last].offset + raw[last].size();
      int gap = t[i].offset - prev_end;
      if (gap < 0) gap = 0;
      bool sp;
      if (k == TK_Comment) {
        // 行の終わりに付けたコメントは、もとの空きを残す（そろえてあるものが崩れない）
        for (int s = 0; s < (gap > 0 ? gap : 1); s++) out += " ";
        col += gap > 0 ? gap : 1;
        sp = false;
      } else if (in_import) {
        sp = gap > 0;   // import の道（./ や ../）は、もとのまま
      } else {
        // 1つ前の字句が値で終わっていなければ、いま見ている記号は「前に付く」
        bool p_prefix = (last == 0) || !ends_value(t[last - 1].kind);
        sp = space_between(t[last].kind, k, gap, p_prefix, in_class);
        // 中かっこは「文のかたまり」か「map の中身」かで空け方が変わる
        if (t[last].kind == TK_LBrace && k == TK_RBrace) sp = false;   // 空の {}
        else if (t[last].kind == TK_LBrace) sp = st.size() > 0 && st[st.size() - 1].block;
        else if (k == TK_RBrace)
          sp = st.size() == 0 || closer_of(st[st.size() - 1].kind) != TK_RBrace ||
               st[st.size() - 1].block;
      }
      if (sp) { out += " "; col++; }
    }

    if (starts_stmt) {
      in_import = (k == TK_Import);
      in_class = (k == TK_Class);
    }
    if (k == TK_Semi) { in_import = false; in_class = false; }
    if (k == TK_LBrace) in_class = false;

    // 閉じは、開きを1つ取り除く
    if (is_close(k) && st.size() > 0 && closer_of(st[st.size() - 1].kind) == k) st.pop();

    out += raw[i];
    col += count_chars(raw[i]);
    if (count_lines(raw[i]) > 0) {
      // 何行にもわたるコメント。桁は数え直す
      int at = raw[i].size();
      while (at > 0 && raw[i][at - 1] != '\n') at--;
      col = count_chars(raw[i].sub(at, raw[i].size() - at));
    }

    // 開きは、続きの行の下げ方を決めて積む
    if (is_open(k)) {
      Open o;
      o.kind = k;
      // 中かっこが「文のかたまり」か「map の中身」か。値の来るところに開いていれば中身
      o.block = true;
      if (k == TK_LBrace && last >= 0) {
        TokKind b = t[last].kind;
        o.block = !(b == TK_Assign || b == TK_LParen || b == TK_LBracket || b == TK_Comma ||
                    b == TK_Colon || b == TK_Return || b == TK_QQ || binary_op(b));
      }
      // 行の終わりにある囲みは「ぶら下げ」。後ろに付いたコメントは、
      // 終わりのままとみなす
      int nx = i + 1;
      while (nx + 1 < t.size() && t[nx].kind == TK_Comment && t[nx].line == t[i].line) nx++;
      bool hang = nx >= t.size() || t[nx].line > end_line[i];
      o.hang = hang;
      if (hang) {
        // 文のかたまりは、**その文が始まった行**から下げる。
        // 折り返した長い条件のうしろに { が来ても、中身が深くならない
        int base = (k == TK_LBrace && o.block) ? stmt_indent : line_indent;
        o.cont = base + kIndent;
        o.close = base;
      } else {
        o.cont = col;        // 囲みのすぐ右に合わせる
        o.close = col;
      }
      st.push(o);
    }
    last = i;
  }
  out += "\n";

  // 整えたものを読み直して、字句の並びが同じか確かめる。
  // 違っていれば、意味を変えているかもしれないので、もとのまま返す
  {
    DiagBag d2;
    Vec<Token> t2;
    Lexer lx(out, d2, 1, 1);
    lx.keep_comments(true);
    lx.run(&t2);
    if (d2.has_error()) return src;
    if (t2.size() != t.size()) return src;
    for (int i = 0; i + 1 < t.size(); i++) {
      if (t2[i].kind != t[i].kind) return src;
      if (!(raw_of(out, t2, i) == raw[i])) return src;
    }
  }

  if (ok) *ok = true;
  return out;
}

}  // namespace shark

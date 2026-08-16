// text.cpp — std.text（spec/library/text.md）
//
// Unicode の表は処理系に埋め込む。外部ライブラリには依存しない。
// 入れているのは、五十音順に必要な仮名の表と、正規表現の小さな実装。
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

static void decode_all(const Str& s, Vec<int>* out) {
  for (int i = 0; i < s.size();) {
    int cp;
    i += utf8_decode(s, i, &cp);
    out->push(cp);
  }
}

// ------------------------------------------------------------------ 仮名の表
struct KanaKey { short base; short daku; short small; short vowel; };
static const KanaKey kKana[83] = {
#include "kana_table.inc"
};

// 五十音順の並べ替え用の鍵
static void ja_key(const Str& s, Vec<int>* key) {
  Vec<int> cps;
  decode_all(s, &cps);
  int prev_vowel = -1;
  for (int i = 0; i < cps.size(); i++) {
    int cp = cps[i];
    if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;         // カタカナ→ひらがな
    if (cp == 0x30FC) {                                    // 長音は直前の母音として扱う
      if (prev_vowel >= 0) {
        key->push(1000 + prev_vowel);
        key->push(0);
        key->push(0);
        continue;
      }
    }
    if (cp >= 0x3041 && cp <= 0x3093) {
      const KanaKey& k = kKana[cp - 0x3041];
      key->push(1000 + k.base);
      key->push(k.daku);
      key->push(k.small);
      prev_vowel = k.vowel;
      continue;
    }
    prev_vowel = -1;
    if (cp < 0x3000) {          // 数字・英字などは仮名より前
      key->push(cp);
      key->push(0);
      key->push(0);
    } else {                    // 漢字などはコードポイント順で仮名の後ろ
      key->push(100000 + cp);
      key->push(0);
      key->push(0);
    }
  }
}

static int cmp_key(const Vec<int>& a, const Vec<int>& b) {
  int n = a.size() < b.size() ? a.size() : b.size();
  for (int i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

// ------------------------------------------------------------------ 正規表現
enum RKind { R_Char, R_Any, R_Class, R_Group, R_Rep, R_Start, R_End };

struct RNode {
  RKind kind;
  int ch;
  bool neg;
  Vec<int> lo, hi;          // 文字クラスの範囲
  Vec<Vec<RNode*> > alts;   // R_Group の中身（| で分けたもの）
  int group;                // ( ) の番号。-1 は覚えない
  RNode* child;             // R_Rep
  int rep_min, rep_max;     // -1 は無制限
  bool greedy;
  RNode() : kind(R_Char), ch(0), neg(false), group(-1), child(0), rep_min(0), rep_max(-1), greedy(true) {}
};

struct RegexProg {
  Vec<RNode*> pool;
  Vec<RNode*> seq;
  int ngroups;
  RegexProg() : ngroups(0) {}
};

void regex_obj_dispose(RegexObj* o) {
  if (!o->p) return;
  for (int i = 0; i < o->p->pool.size(); i++) { o->p->pool[i]->~RNode(); sk_free(o->p->pool[i]); }
  o->p->~RegexProg();
  sk_free(o->p);
  o->p = 0;
}

struct RParser {
  const Vec<int>& p;
  int i;
  RegexProg* prog;
  Str err;
  RParser(const Vec<int>& pat, RegexProg* pr) : p(pat), i(0), prog(pr) {}

  RNode* node(RKind k) {
    RNode* n = new (sk_alloc(sizeof(RNode))) RNode();
    n->kind = k;
    prog->pool.push(n);
    return n;
  }
  void add_class_escape(RNode* n, int c) {
    switch (c) {
      case 'd': n->lo.push('0'); n->hi.push('9'); break;
      case 'w':
        n->lo.push('a'); n->hi.push('z');
        n->lo.push('A'); n->hi.push('Z');
        n->lo.push('0'); n->hi.push('9');
        n->lo.push('_'); n->hi.push('_');
        break;
      case 's':
        n->lo.push(' '); n->hi.push(' ');
        n->lo.push('\t'); n->hi.push('\t');
        n->lo.push('\n'); n->hi.push('\n');
        n->lo.push('\r'); n->hi.push('\r');
        break;
      case 'n': n->lo.push('\n'); n->hi.push('\n'); break;
      case 't': n->lo.push('\t'); n->hi.push('\t'); break;
      default: n->lo.push(c); n->hi.push(c); break;
    }
  }
  bool parse_alts(Vec<Vec<RNode*> >* alts, bool top) {
    Vec<RNode*> cur;
    for (;;) {
      if (i >= p.size()) {
        if (!top) { err = Str("( が閉じていません"); return false; }
        alts->push(cur);
        return true;
      }
      int c = p[i];
      if (c == ')') {
        if (top) { err = Str(") が多すぎます"); return false; }
        alts->push(cur);
        return true;
      }
      if (c == '|') {
        i++;
        alts->push(cur);
        cur.clear();
        continue;
      }
      RNode* atom = parse_atom();
      if (!atom) return false;
      // 繰り返し
      while (i < p.size()) {
        int q = p[i];
        int lo = -1, hi = -1;
        if (q == '*') { lo = 0; hi = -1; i++; }
        else if (q == '+') { lo = 1; hi = -1; i++; }
        else if (q == '?') { lo = 0; hi = 1; i++; }
        else if (q == '{') {
          int save = i;
          i++;
          int a = 0, b = -1;
          bool got = false;
          while (i < p.size() && p[i] >= '0' && p[i] <= '9') { a = a * 10 + (p[i] - '0'); i++; got = true; }
          if (!got) { i = save; break; }
          if (i < p.size() && p[i] == ',') {
            i++;
            if (i < p.size() && p[i] >= '0' && p[i] <= '9') {
              b = 0;
              while (i < p.size() && p[i] >= '0' && p[i] <= '9') { b = b * 10 + (p[i] - '0'); i++; }
            }
          } else {
            b = a;
          }
          if (i >= p.size() || p[i] != '}') { i = save; break; }
          i++;
          lo = a;
          hi = b;
        } else {
          break;
        }
        RNode* rep = node(R_Rep);
        rep->child = atom;
        rep->rep_min = lo;
        rep->rep_max = hi;
        rep->greedy = true;
        if (i < p.size() && p[i] == '?') { rep->greedy = false; i++; }
        atom = rep;
      }
      cur.push(atom);
    }
  }
  RNode* parse_atom() {
    int c = p[i];
    if (c == '^') { i++; return node(R_Start); }
    if (c == '$') { i++; return node(R_End); }
    if (c == '.') { i++; return node(R_Any); }
    if (c == '(') {
      i++;
      RNode* g = node(R_Group);
      bool capture = true;
      if (i + 1 < p.size() && p[i] == '?' && p[i + 1] == ':') { capture = false; i += 2; }
      g->group = capture ? ++prog->ngroups : -1;
      if (!parse_alts(&g->alts, false)) return 0;
      if (i >= p.size() || p[i] != ')') { err = Str("( が閉じていません"); return 0; }
      i++;
      return g;
    }
    if (c == '[') {
      i++;
      RNode* n = node(R_Class);
      if (i < p.size() && p[i] == '^') { n->neg = true; i++; }
      bool first = true;
      while (i < p.size() && (p[i] != ']' || first)) {
        first = false;
        int a = p[i++];
        if (a == '\\' && i < p.size()) {
          add_class_escape(n, p[i++]);
          continue;
        }
        if (i + 1 < p.size() && p[i] == '-' && p[i + 1] != ']') {
          int b = p[i + 1];
          i += 2;
          n->lo.push(a);
          n->hi.push(b);
          continue;
        }
        n->lo.push(a);
        n->hi.push(a);
      }
      if (i >= p.size()) { err = Str("[ が閉じていません"); return 0; }
      i++;
      return n;
    }
    if (c == '\\') {
      i++;
      if (i >= p.size()) { err = Str("\\ の後ろがありません"); return 0; }
      int e = p[i++];
      if (e == 'd' || e == 'w' || e == 's') {
        RNode* n = node(R_Class);
        add_class_escape(n, e);
        return n;
      }
      if (e == 'D' || e == 'W' || e == 'S') {
        RNode* n = node(R_Class);
        n->neg = true;
        add_class_escape(n, e + 32);
        return n;
      }
      RNode* n = node(R_Char);
      n->ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
      return n;
    }
    i++;
    RNode* n = node(R_Char);
    n->ch = c;
    return n;
  }
};

// 1文字だけを見る部品か（繰り返しを積み上げずに数えられる）
static bool is_simple(RNode* n) {
  return n->kind == R_Char || n->kind == R_Any || n->kind == R_Class;
}

static const int kRegexMaxDepth = 4000;

struct RMatcher {
  const Vec<int>& s;
  Vec<int> gs, ge;
  int depth;
  bool overflow;   // 深くなりすぎて途中でやめた
  RMatcher(const Vec<int>& subject, int ngroups) : s(subject), depth(0), overflow(false) {
    for (int i = 0; i <= ngroups; i++) { gs.push(-1); ge.push(-1); }
  }
  struct Deeper {
    RMatcher* m;
    Deeper(RMatcher* q) : m(q) { m->depth++; }
    ~Deeper() { m->depth--; }
  };
  bool one_char(RNode* n, int pos) {
    if (pos >= s.size()) return false;
    switch (n->kind) {
      case R_Char: return s[pos] == n->ch;
      case R_Any: return true;
      case R_Class: return class_match(n, s[pos]);
      default: return false;
    }
  }
  bool class_match(RNode* n, int c) {
    bool in = false;
    for (int i = 0; i < n->lo.size(); i++)
      if (c >= n->lo[i] && c <= n->hi[i]) { in = true; break; }
    return n->neg ? !in : in;
  }
  bool seq_match(const Vec<RNode*>& seq, int si, int pos, int* end) {
    if (si >= seq.size()) { *end = pos; return true; }
    Deeper deeper(this);
    if (depth > kRegexMaxDepth) { overflow = true; return false; }
    return node_match(seq[si], pos, seq, si, end);
  }
  bool node_match(RNode* n, int pos, const Vec<RNode*>& seq, int si, int* end) {
    switch (n->kind) {
      case R_Char:
        if (pos < s.size() && s[pos] == n->ch) return seq_match(seq, si + 1, pos + 1, end);
        return false;
      case R_Any:
        if (pos < s.size()) return seq_match(seq, si + 1, pos + 1, end);
        return false;
      case R_Class:
        if (pos < s.size() && class_match(n, s[pos])) return seq_match(seq, si + 1, pos + 1, end);
        return false;
      case R_Start:
        if (pos == 0) return seq_match(seq, si + 1, pos, end);
        return false;
      case R_End:
        if (pos == s.size()) return seq_match(seq, si + 1, pos, end);
        return false;
      case R_Group: {
        for (int a = 0; a < n->alts.size(); a++) {
          int save_s = n->group > 0 ? gs[n->group] : 0;
          int save_e = n->group > 0 ? ge[n->group] : 0;
          if (n->group > 0) gs[n->group] = pos;
          // 中身を通した後、続きを見る
          if (group_then(n, a, 0, pos, seq, si, end)) return true;
          if (n->group > 0) { gs[n->group] = save_s; ge[n->group] = save_e; }
        }
        return false;
      }
      case R_Rep: {
        return rep_match(n, 0, pos, seq, si, end);
      }
    }
    return false;
  }
  // グループの中身を si2 から進め、終わったら外の続きへ
  bool group_then(RNode* g, int alt, int si2, int pos, const Vec<RNode*>& outer, int osi, int* end) {
    const Vec<RNode*>& inner = g->alts[alt];
    if (si2 >= inner.size()) {
      if (g->group > 0) ge[g->group] = pos;
      return seq_match(outer, osi + 1, pos, end);
    }
    // 中身の1つを進める（続きは自分自身）
    RNode* n = inner[si2];
    return inner_node(g, alt, si2, n, pos, outer, osi, end);
  }
  bool inner_node(RNode* g, int alt, int si2, RNode* n, int pos, const Vec<RNode*>& outer, int osi,
                  int* end) {
    switch (n->kind) {
      case R_Char:
        if (pos < s.size() && s[pos] == n->ch) return group_then(g, alt, si2 + 1, pos + 1, outer, osi, end);
        return false;
      case R_Any:
        if (pos < s.size()) return group_then(g, alt, si2 + 1, pos + 1, outer, osi, end);
        return false;
      case R_Class:
        if (pos < s.size() && class_match(n, s[pos])) return group_then(g, alt, si2 + 1, pos + 1, outer, osi, end);
        return false;
      case R_Start:
        if (pos == 0) return group_then(g, alt, si2 + 1, pos, outer, osi, end);
        return false;
      case R_End:
        if (pos == s.size()) return group_then(g, alt, si2 + 1, pos, outer, osi, end);
        return false;
      case R_Group: {
        // 入れ子のグループ: いったん外側の続きを作るため、まとめて扱う
        Vec<RNode*> rest;
        rest.push(n);
        for (int k = si2 + 1; k < g->alts[alt].size(); k++) rest.push(g->alts[alt][k]);
        // 中身を進めたあと、外の続きへ
        return nested(g, alt, rest, 0, pos, outer, osi, end);
      }
      case R_Rep: {
        Vec<RNode*> rest;
        rest.push(n);
        for (int k = si2 + 1; k < g->alts[alt].size(); k++) rest.push(g->alts[alt][k]);
        return nested(g, alt, rest, 0, pos, outer, osi, end);
      }
    }
    return false;
  }
  // rest の並びを進め、終わったらグループを閉じて外へ
  bool nested(RNode* g, int alt, const Vec<RNode*>& rest, int ri, int pos, const Vec<RNode*>& outer,
              int osi, int* end) {
    Deeper deeper(this);
    if (depth > kRegexMaxDepth) { overflow = true; return false; }
    if (ri >= rest.size()) {
      if (g->group > 0) ge[g->group] = pos;
      return seq_match(outer, osi + 1, pos, end);
    }
    RNode* n = rest[ri];
    if (n->kind == R_Rep) return rep_nested(g, alt, rest, ri, n, 0, pos, outer, osi, end);
    if (n->kind == R_Group) {
      for (int a = 0; a < n->alts.size(); a++) {
        if (n->group > 0) gs[n->group] = pos;
        Vec<RNode*> inner2;
        for (int k = 0; k < n->alts[a].size(); k++) inner2.push(n->alts[a][k]);
        for (int k = ri + 1; k < rest.size(); k++) inner2.push(rest[k]);
        if (nested(g, alt, inner2, 0, pos, outer, osi, end)) {
          if (n->group > 0) ge[n->group] = pos;
          return true;
        }
      }
      return false;
    }
    switch (n->kind) {
      case R_Char:
        if (pos < s.size() && s[pos] == n->ch) return nested(g, alt, rest, ri + 1, pos + 1, outer, osi, end);
        return false;
      case R_Any:
        if (pos < s.size()) return nested(g, alt, rest, ri + 1, pos + 1, outer, osi, end);
        return false;
      case R_Class:
        if (pos < s.size() && class_match(n, s[pos])) return nested(g, alt, rest, ri + 1, pos + 1, outer, osi, end);
        return false;
      case R_Start:
        if (pos == 0) return nested(g, alt, rest, ri + 1, pos, outer, osi, end);
        return false;
      case R_End:
        if (pos == s.size()) return nested(g, alt, rest, ri + 1, pos, outer, osi, end);
        return false;
      default:
        return false;
    }
  }
  bool rep_nested(RNode* g, int alt, const Vec<RNode*>& rest, int ri, RNode* rep, int count, int pos,
                  const Vec<RNode*>& outer, int osi, int* end) {
    if (count == 0 && is_simple(rep->child)) {
      int most = 0;
      while ((rep->rep_max < 0 || most < rep->rep_max) && one_char(rep->child, pos + most)) most++;
      if (most < rep->rep_min) return false;
      if (rep->greedy) {
        for (int k = most; k >= rep->rep_min; k--)
          if (nested(g, alt, rest, ri + 1, pos + k, outer, osi, end)) return true;
      } else {
        for (int k = rep->rep_min; k <= most; k++)
          if (nested(g, alt, rest, ri + 1, pos + k, outer, osi, end)) return true;
      }
      return false;
    }
    bool can_more = (rep->rep_max < 0 || count < rep->rep_max);
    if (rep->greedy && can_more) {
      Vec<RNode*> one;
      one.push(rep->child);
      int mid = -1;
      // 1回進めてから、残りを続ける
      if (try_one(rep->child, pos, &mid)) {
        if (mid != pos || count == 0) {
          if (rep_nested(g, alt, rest, ri, rep, count + 1, mid, outer, osi, end)) return true;
        }
      }
    }
    if (count >= rep->rep_min) {
      if (nested(g, alt, rest, ri + 1, pos, outer, osi, end)) return true;
    }
    if (!rep->greedy && can_more) {
      int mid = -1;
      if (try_one(rep->child, pos, &mid) && mid != pos) {
        if (rep_nested(g, alt, rest, ri, rep, count + 1, mid, outer, osi, end)) return true;
      }
    }
    return false;
  }
  // 1つの部品だけを進める（後戻りしない簡易版。グループの中の繰り返しに使う）
  bool try_one(RNode* n, int pos, int* end) {
    switch (n->kind) {
      case R_Char:
        if (pos < s.size() && s[pos] == n->ch) { *end = pos + 1; return true; }
        return false;
      case R_Any:
        if (pos < s.size()) { *end = pos + 1; return true; }
        return false;
      case R_Class:
        if (pos < s.size() && class_match(n, s[pos])) { *end = pos + 1; return true; }
        return false;
      case R_Group: {
        for (int a = 0; a < n->alts.size(); a++) {
          Vec<RNode*> empty;
          int e2 = -1;
          if (n->group > 0) gs[n->group] = pos;
          if (seq_match(n->alts[a], 0, pos, &e2)) {
            if (n->group > 0) ge[n->group] = e2;
            *end = e2;
            return true;
          }
        }
        return false;
      }
      case R_Rep: {
        int cur = pos, cnt = 0;
        while (n->rep_max < 0 || cnt < n->rep_max) {
          int nx;
          if (!try_one(n->child, cur, &nx) || nx == cur) break;
          cur = nx;
          cnt++;
        }
        if (cnt < n->rep_min) return false;
        *end = cur;
        return true;
      }
      default:
        *end = pos;
        return true;
    }
  }
  bool rep_match(RNode* rep, int count, int pos, const Vec<RNode*>& seq, int si, int* end) {
    if (count == 0 && is_simple(rep->child)) {
      // a* や [0-9]+ のような繰り返しは、進めるだけ進めてから戻す
      int most = 0;
      while ((rep->rep_max < 0 || most < rep->rep_max) && one_char(rep->child, pos + most)) most++;
      if (most < rep->rep_min) return false;
      if (rep->greedy) {
        for (int k = most; k >= rep->rep_min; k--)
          if (seq_match(seq, si + 1, pos + k, end)) return true;
      } else {
        for (int k = rep->rep_min; k <= most; k++)
          if (seq_match(seq, si + 1, pos + k, end)) return true;
      }
      return false;
    }
    bool can_more = (rep->rep_max < 0 || count < rep->rep_max);
    if (rep->greedy && can_more) {
      int mid;
      if (try_one(rep->child, pos, &mid) && (mid != pos)) {
        if (rep_match(rep, count + 1, mid, seq, si, end)) return true;
      }
    }
    if (count >= rep->rep_min) {
      if (seq_match(seq, si + 1, pos, end)) return true;
    }
    if (!rep->greedy && can_more) {
      int mid;
      if (try_one(rep->child, pos, &mid) && mid != pos) {
        if (rep_match(rep, count + 1, mid, seq, si, end)) return true;
      }
    }
    return false;
  }
};

static bool regex_search(RegexProg* prog, const Vec<int>& subject, int from, int* start, int* end,
                         Vec<int>* gs, Vec<int>* ge, bool* gave_up = 0) {
  for (int at = from; at <= subject.size(); at++) {
    RMatcher m(subject, prog->ngroups);
    int e = -1;
    bool hit = m.seq_match(prog->seq, 0, at, &e);
    if (m.overflow && gave_up) *gave_up = true;
    if (hit) {
      *start = at;
      *end = e;
      *gs = m.gs;
      *ge = m.ge;
      return true;
    }
  }
  return false;
}

static Str join_cps(const Vec<int>& cps, int from, int to) {
  Str r;
  for (int i = from; i < to && i < cps.size(); i++) utf8_encode(r, cps[i]);
  return r;
}

static Value make_match(const Vec<int>& cps, int st, int en, const Vec<int>& gs, const Vec<int>& ge) {
  MatchObj* m = new (sk_alloc(sizeof(MatchObj))) MatchObj();
  m->groups.push(join_cps(cps, st, en));
  m->starts.push(st);
  m->ends.push(en);
  for (int i = 1; i < gs.size(); i++) {
    if (gs[i] >= 0 && ge[i] >= gs[i]) {
      m->groups.push(join_cps(cps, gs[i], ge[i]));
      m->starts.push(gs[i]);
      m->ends.push(ge[i]);
    } else {
      m->groups.push(Str());
      m->starts.push(-1);
      m->ends.push(-1);
    }
  }
  return mk_obj_value(m);
}

// ------------------------------------------------------------------ 関数
static NativeStatus t_width(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(utf8_display_width(S(a, 0)));
  return N_Ok;
}
static NativeStatus t_code(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> cps;
  decode_all(S(a, 0), &cps);
  out = mk_int(cps.size() ? cps[0] : 0);
  return N_Ok;
}
static NativeStatus t_from_code(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  int64_t cp = A(a, 0)->i;
  if (cp < 0 || cp > 0x10FFFF) { out = mk_none(); return N_Ok; }
  Str s;
  utf8_encode(s, (int)cp);
  out = mk_str(s);
  return N_Ok;
}
static NativeStatus t_fold_case(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> cps;
  decode_all(S(a, 0), &cps);
  Str r;
  for (int i = 0; i < cps.size(); i++) {
    int c = cps[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c >= 0xFF21 && c <= 0xFF3A) c += 32;        // 全角英字
    if (c >= 0x30A1 && c <= 0x30F6) c -= 0x60;      // カタカナ→ひらがな
    utf8_encode(r, c);
  }
  out = mk_str(r);
  return N_Ok;
}
// 濁点の合成・分解だけを行う（埋め込む表を小さく保つため）
static NativeStatus t_normalize(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> cps;
  decode_all(S(a, 0), &cps);
  bool nfd = (S(a, 1) == "NFD");
  Str r;
  if (nfd) {
    for (int i = 0; i < cps.size(); i++) {
      int c = cps[i];
      if (c >= 0x3041 && c <= 0x3093) {
        const KanaKey& k = kKana[c - 0x3041];
        if (k.daku > 0) {
          // 清音に戻して結合文字を足す
          for (int b = 0; b < 83; b++)
            if (kKana[b].base == k.base && kKana[b].daku == 0 && kKana[b].small == k.small) {
              utf8_encode(r, 0x3041 + b);
              break;
            }
          utf8_encode(r, k.daku == 1 ? 0x3099 : 0x309A);
          continue;
        }
      }
      utf8_encode(r, c);
    }
  } else {
    for (int i = 0; i < cps.size(); i++) {
      int c = cps[i];
      if (i + 1 < cps.size() && (cps[i + 1] == 0x3099 || cps[i + 1] == 0x309A) && c >= 0x3041 &&
          c <= 0x3093) {
        const KanaKey& k = kKana[c - 0x3041];
        int want = cps[i + 1] == 0x3099 ? 1 : 2;
        bool done = false;
        for (int b = 0; b < 83; b++)
          if (kKana[b].base == k.base && kKana[b].daku == want && kKana[b].small == k.small) {
            utf8_encode(r, 0x3041 + b);
            done = true;
            break;
          }
        if (done) { i++; continue; }
      }
      utf8_encode(r, c);
    }
  }
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus t_is_digit(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> c;
  decode_all(S(a, 0), &c);
  bool ok = c.size() > 0;
  for (int i = 0; i < c.size(); i++) if (!((c[i] >= '0' && c[i] <= '9') || (c[i] >= 0xFF10 && c[i] <= 0xFF19))) ok = false;
  out = mk_bool(ok);
  return N_Ok;
}
static NativeStatus t_is_alpha(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> c;
  decode_all(S(a, 0), &c);
  bool ok = c.size() > 0;
  for (int i = 0; i < c.size(); i++) {
    int x = c[i];
    bool alpha = (x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || x >= 0x3040;
    if (!alpha) ok = false;
  }
  out = mk_bool(ok);
  return N_Ok;
}
static NativeStatus t_is_space(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> c;
  decode_all(S(a, 0), &c);
  bool ok = c.size() > 0;
  for (int i = 0; i < c.size(); i++)
    if (!(c[i] == ' ' || c[i] == '\t' || c[i] == '\n' || c[i] == '\r' || c[i] == 0x3000)) ok = false;
  out = mk_bool(ok);
  return N_Ok;
}
static NativeStatus t_is_upper(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> c;
  decode_all(S(a, 0), &c);
  bool ok = c.size() > 0;
  for (int i = 0; i < c.size(); i++) if (!(c[i] >= 'A' && c[i] <= 'Z')) ok = false;
  out = mk_bool(ok);
  return N_Ok;
}
static NativeStatus t_is_lower(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> c;
  decode_all(S(a, 0), &c);
  bool ok = c.size() > 0;
  for (int i = 0; i < c.size(); i++) if (!(c[i] >= 'a' && c[i] <= 'z')) ok = false;
  out = mk_bool(ok);
  return N_Ok;
}
static NativeStatus t_compare(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(S(a, 0).cmp(S(a, 1)));
  return N_Ok;
}
static NativeStatus t_compare_ja(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Vec<int> ka, kb;
  ja_key(S(a, 0), &ka);
  ja_key(S(a, 1), &kb);
  out = mk_int(cmp_key(ka, kb));
  return N_Ok;
}
static NativeStatus t_sort_ja(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* recv = A(a, 0);
  ListObj* l = (ListObj*)obj_unique(*recv);
  // 挿入整列（要素は文字列だけ）
  for (int i = 1; i < l->v.size(); i++) {
    Value v = l->v[i];
    Vec<int> kv;
    ja_key(((StrObj*)v.o)->s, &kv);
    int j = i - 1;
    while (j >= 0) {
      Vec<int> kj;
      ja_key(((StrObj*)l->v[j].o)->s, &kj);
      if (cmp_key(kj, kv) <= 0) break;
      l->v[j + 1] = l->v[j];
      j--;
    }
    l->v[j + 1] = v;
  }
  out = mk_void();
  return N_Ok;
}
static NativeStatus t_decode(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  const Str& enc = S(a, 1);
  if (!(enc == "utf-8" || enc == "UTF-8" || enc == "utf8")) {
    out = mk_result_err(vm.make_error(Str("この処理系は ") + enc + " を扱えません（utf-8 だけです）", 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_str(S(a, 0)));
  return N_Ok;
}
static NativeStatus t_encode(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  const Str& enc = S(a, 1);
  if (!(enc == "utf-8" || enc == "UTF-8" || enc == "utf8")) {
    out = mk_result_err(vm.make_error(Str("この処理系は ") + enc + " を扱えません（utf-8 だけです）", 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_bytes(S(a, 0)));
  return N_Ok;
}
static NativeStatus t_regex(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Vec<int> pat;
  decode_all(S(a, 0), &pat);
  RegexProg* prog = new (sk_alloc(sizeof(RegexProg))) RegexProg();
  RParser p(pat, prog);
  Vec<Vec<RNode*> > alts;
  bool ok = p.parse_alts(&alts, true);
  if (!ok || p.err.size()) {
    Str err = p.err;
    RegexObj tmp;
    tmp.p = prog;
    regex_obj_dispose(&tmp);
    out = mk_result_err(vm.make_error(Str("正規表現を読めません: ") + err, 0));
    return N_Ok;
  }
  if (alts.size() == 1) {
    prog->seq = alts[0];
  } else {
    RNode* g = new (sk_alloc(sizeof(RNode))) RNode();
    g->kind = R_Group;
    g->group = -1;
    g->alts = alts;
    prog->pool.push(g);
    prog->seq.push(g);
  }
  RegexObj* ro = new (sk_alloc(sizeof(RegexObj))) RegexObj();
  ro->p = prog;
  Value v = mk_obj_value(ro);
  out = mk_result_ok(v);
  val_release(v);
  return N_Ok;
}
static NativeStatus rx_find(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  RegexProg* prog = ((RegexObj*)A(a, 0)->o)->p;
  Vec<int> cps;
  decode_all(S(a, 1), &cps);
  int st, en;
  Vec<int> gs, ge;
  if (!regex_search(prog, cps, 0, &st, &en, &gs, &ge)) { out = mk_none(); return N_Ok; }
  out = make_match(cps, st, en, gs, ge);
  return N_Ok;
}
static NativeStatus rx_find_all(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  RegexProg* prog = ((RegexObj*)A(a, 0)->o)->p;
  Vec<int> cps;
  decode_all(S(a, 1), &cps);
  out = mk_list();
  int from = 0;
  while (from <= cps.size()) {
    int st, en;
    Vec<int> gs, ge;
    if (!regex_search(prog, cps, from, &st, &en, &gs, &ge)) break;
    Value m = make_match(cps, st, en, gs, ge);
    as_list(out)->v.push(m);
    from = (en > st) ? en : st + 1;
  }
  return N_Ok;
}
static NativeStatus rx_matches(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  RegexProg* prog = ((RegexObj*)A(a, 0)->o)->p;
  Vec<int> cps;
  decode_all(S(a, 1), &cps);
  int st, en;
  Vec<int> gs, ge;
  out = mk_bool(regex_search(prog, cps, 0, &st, &en, &gs, &ge));
  return N_Ok;
}
static NativeStatus rx_replace(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  RegexProg* prog = ((RegexObj*)A(a, 0)->o)->p;
  Vec<int> cps;
  decode_all(S(a, 1), &cps);
  Vec<int> rep;
  decode_all(S(a, 2), &rep);
  Str r;
  int from = 0;
  while (from <= cps.size()) {
    int st, en;
    Vec<int> gs, ge;
    if (!regex_search(prog, cps, from, &st, &en, &gs, &ge)) break;
    r += join_cps(cps, from, st);
    for (int i = 0; i < rep.size(); i++) {
      if (rep[i] == '$' && i + 1 < rep.size() && rep[i + 1] >= '0' && rep[i + 1] <= '9') {
        int g = rep[i + 1] - '0';
        if (g == 0) r += join_cps(cps, st, en);
        else if (g < gs.size() && gs[g] >= 0) r += join_cps(cps, gs[g], ge[g]);
        i++;
        continue;
      }
      utf8_encode(r, rep[i]);
    }
    from = (en > st) ? en : st + 1;
    if (en == st && st < cps.size()) utf8_encode(r, cps[st]);
  }
  r += join_cps(cps, from, cps.size());
  out = mk_str(r);
  return N_Ok;
}
static NativeStatus rx_split(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  RegexProg* prog = ((RegexObj*)A(a, 0)->o)->p;
  Vec<int> cps;
  decode_all(S(a, 1), &cps);
  out = mk_list();
  int from = 0, cur = 0;
  while (cur <= cps.size()) {
    int st, en;
    Vec<int> gs, ge;
    if (!regex_search(prog, cps, cur, &st, &en, &gs, &ge) || en == st) break;
    as_list(out)->v.push(mk_str(join_cps(cps, from, st)));
    from = en;
    cur = en;
  }
  as_list(out)->v.push(mk_str(join_cps(cps, from, cps.size())));
  return N_Ok;
}
static NativeStatus mt_text(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_str(((MatchObj*)A(a, 0)->o)->groups[0]);
  return N_Ok;
}
static NativeStatus mt_start(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(((MatchObj*)A(a, 0)->o)->starts[0]);
  return N_Ok;
}
static NativeStatus mt_end(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_int(((MatchObj*)A(a, 0)->o)->ends[0]);
  return N_Ok;
}
static NativeStatus mt_group(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  MatchObj* m = (MatchObj*)A(a, 0)->o;
  int64_t g = A(a, 1)->i;
  if (g < 0 || g >= m->groups.size() || m->starts[(int)g] < 0) { out = mk_none(); return N_Ok; }
  out = mk_str(m->groups[(int)g]);
  return N_Ok;
}

void register_text(Registry& r) {
  TypeTable& t = r.types();
  Type* ts = t.t_string();
  Type* ti = t.t_int();
  Type* tb = t.t_bool();
  Type* tv = t.t_void();
  r.add("text.normalize", t_normalize, ts, ts, ts);
  r.add("text.width", t_width, ti, ts);
  r.add("text.fold_case", t_fold_case, ts, ts);
  r.add("text.code", t_code, ti, ts);
  r.add("text.from_code", t_from_code, t.optional_of(ts), ti);
  r.add("text.is_digit", t_is_digit, tb, ts);
  r.add("text.is_alpha", t_is_alpha, tb, ts);
  r.add("text.is_space", t_is_space, tb, ts);
  r.add("text.is_upper", t_is_upper, tb, ts);
  r.add("text.is_lower", t_is_lower, tb, ts);
  r.add("text.compare", t_compare, ti, ts, ts);
  r.add("text.compare_ja", t_compare_ja, ti, ts, ts);
  int id = r.add("text.sort_ja", t_sort_ja, tv, t.list_of(ts));
  r.mark_ref0(id);
  r.add("text.decode", t_decode, t.result_of(ts), t.t_bytes(), ts);
  r.add("text.encode", t_encode, t.result_of(t.t_bytes()), ts, ts);
  r.add("text.regex", t_regex, t.result_of(t.simple(T_Regex)), ts);
  r.add_untyped("text.Regex.find", rx_find);
  r.add_untyped("text.Regex.find_all", rx_find_all);
  r.add_untyped("text.Regex.matches", rx_matches);
  r.add_untyped("text.Regex.replace", rx_replace);
  r.add_untyped("text.Regex.split", rx_split);
  r.add_untyped("text.Match.text", mt_text);
  r.add_untyped("text.Match.start", mt_start);
  r.add_untyped("text.Match.end", mt_end);
  r.add_untyped("text.Match.group", mt_group);
  r.enable_module("std.text");
}

}  // namespace shark

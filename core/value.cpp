#include "value.h"
#include "program.h"

namespace shark {

// --- 作る -----------------------------------------------------------------
Value mk_void() { Value v; v.k = V_Void; return v; }
Value mk_none() { Value v; v.k = V_None; return v; }
Value mk_int(int64_t x) { Value v; v.k = V_Int; v.i = x; return v; }
Value mk_float(double x) { Value v; v.k = V_Float; v.f = x; return v; }
Value mk_bool(bool x) { Value v; v.k = V_Bool; v.b = x; return v; }
Value mk_obj_value(Obj* o) { Value v; v.k = V_Obj; v.o = o; return v; }

static Value wrap(Obj* o) { Value v; v.k = V_Obj; v.o = o; return v; }

Value mk_str(const Str& s) { StrObj* o = new (sk_alloc(sizeof(StrObj))) StrObj(O_Str); o->s = s; return wrap(o); }
Value mk_str(const char* s) { return mk_str(Str(s)); }
Value mk_bytes(const Str& s) { StrObj* o = new (sk_alloc(sizeof(StrObj))) StrObj(O_Bytes); o->s = s; return wrap(o); }
Value mk_list() { return wrap(new (sk_alloc(sizeof(ListObj))) ListObj()); }
Value mk_map() { return wrap(new (sk_alloc(sizeof(MapObj))) MapObj()); }
Value mk_range(int64_t s, int64_t e, int64_t st) {
  RangeObj* o = new (sk_alloc(sizeof(RangeObj))) RangeObj();
  o->start = s; o->end = e; o->step = st; return wrap(o);
}
Value mk_func(int fn) { FuncObj* o = new (sk_alloc(sizeof(FuncObj))) FuncObj(); o->fn = fn; return wrap(o); }
Value mk_time(int64_t ns, int32_t off_s) {
  TimeObj* o = new (sk_alloc(sizeof(TimeObj))) TimeObj();
  o->unix_ns = ns;
  o->off_s = off_s;
  return wrap(o);
}
Value mk_dur(int64_t ns) { DurObj* o = new (sk_alloc(sizeof(DurObj))) DurObj(); o->ns = ns; return wrap(o); }
Value mk_json() { return wrap(new (sk_alloc(sizeof(JsonObj))) JsonObj()); }

Value mk_inst(ClassInfo* c) {
  InstObj* o = new (sk_alloc(sizeof(InstObj))) InstObj();
  o->cls = c;
  return wrap(o);
}
Value mk_result_ok(const Value& v) {
  ResultObj* o = new (sk_alloc(sizeof(ResultObj))) ResultObj();
  o->ok = true; o->val = val_retain(v); return wrap(o);
}
Value mk_result_err(const Value& e) {
  ResultObj* o = new (sk_alloc(sizeof(ResultObj))) ResultObj();
  o->ok = false; o->val = val_retain(e); return wrap(o);
}

// --- 参照カウント ---------------------------------------------------------
void obj_retain(Obj* o) { if (o) o->rc++; }

void free_obj(Obj* o);

// 片付け待ちの置き場。長い鎖でも C++ の再帰にならないよう、順に片付ける
// （free_obj の中の val_release は、下の while が回っている間はここへ積むだけになる）
static Vec<Obj*> g_dead;
static bool g_draining = false;

void obj_release(Obj* o) {
  if (!o) return;
  if (--o->rc > 0) return;
  // 中に値を持たないものは、その場で片付ける（一時的な文字列はほとんどこちら）
  switch (o->kind) {
    case O_Str: case O_Bytes: case O_Range: case O_Func:
    case O_Time: case O_Dur: case O_Regex: case O_Match:
      free_obj(o);
      return;
    default:
      break;
  }
  g_dead.push(o);
  if (g_draining) return;   // 外側が片付ける

  g_draining = true;
  while (g_dead.size() > 0) {
    Obj* x = g_dead.back();
    g_dead.pop();
    free_obj(x);
  }
  g_draining = false;
  // 深い鎖のあとは置き場が大きくなっているので、返しておく
  if (g_dead.capacity() > 1024) {
    Vec<Obj*> empty;
    empty.swap_with(g_dead);
  }
}

void free_obj(Obj* o) {
  switch (o->kind) {
    case O_Str: case O_Bytes: ((StrObj*)o)->~StrObj(); break;
    case O_List: {
      ListObj* l = (ListObj*)o;
      for (int i = 0; i < l->v.size(); i++) val_release(l->v[i]);
      l->~ListObj();
      break;
    }
    case O_Map: {
      MapObj* m = (MapObj*)o;
      for (int i = 0; i < m->e.size(); i++) { val_release(m->e[i].key); val_release(m->e[i].val); }
      m->~MapObj();
      break;
    }
    case O_Inst: {
      InstObj* n = (InstObj*)o;
      for (int i = 0; i < n->fields.size(); i++) val_release(n->fields[i]);
      n->~InstObj();
      break;
    }
    case O_Result: { ResultObj* r = (ResultObj*)o; val_release(r->val); r->~ResultObj(); break; }
    case O_Range: ((RangeObj*)o)->~RangeObj(); break;
    case O_Func: ((FuncObj*)o)->~FuncObj(); break;
    case O_Time: ((TimeObj*)o)->~TimeObj(); break;
    case O_Dur: ((DurObj*)o)->~DurObj(); break;
    case O_File: { FileObj* f = (FileObj*)o; file_obj_dispose(f); f->~FileObj(); break; }
    case O_Json: {
      JsonObj* j = (JsonObj*)o;
      for (int i = 0; i < j->items.size(); i++) val_release(j->items[i]);
      j->~JsonObj();
      break;
    }
    case O_Task: { TaskObj* t = (TaskObj*)o; task_obj_dispose(t); t->~TaskObj(); break; }
    case O_Chan: { ChanObj* c = (ChanObj*)o; chan_obj_dispose(c); c->~ChanObj(); break; }
    case O_Regex: { RegexObj* r = (RegexObj*)o; regex_obj_dispose(r); r->~RegexObj(); break; }
    case O_Match: ((MatchObj*)o)->~MatchObj(); break;
  }
  sk_free(o);
}

void obj_release_pool_free() {
  if (g_draining) return;
  Vec<Obj*> empty;
  empty.swap_with(g_dead);
}

// --- 書き込み時コピー -----------------------------------------------------
static Obj* clone_obj(Obj* o) {
  switch (o->kind) {
    case O_Str: case O_Bytes: {
      StrObj* n = new (sk_alloc(sizeof(StrObj))) StrObj(o->kind);
      n->s = ((StrObj*)o)->s; return n;
    }
    case O_List: {
      ListObj* src = (ListObj*)o;
      ListObj* n = new (sk_alloc(sizeof(ListObj))) ListObj();
      n->v.reserve(src->v.size());
      for (int i = 0; i < src->v.size(); i++) n->v.push(val_retain(src->v[i]));
      return n;
    }
    case O_Map: {
      MapObj* src = (MapObj*)o;
      MapObj* n = new (sk_alloc(sizeof(MapObj))) MapObj();
      n->live = src->live;
      n->idx = src->idx;
      for (int i = 0; i < src->e.size(); i++) {
        MapEntry en;
        en.dead = src->e[i].dead;
        en.key = val_retain(src->e[i].key);
        en.val = val_retain(src->e[i].val);
        n->e.push(en);
      }
      return n;
    }
    case O_Inst: {
      InstObj* src = (InstObj*)o;
      InstObj* n = new (sk_alloc(sizeof(InstObj))) InstObj();
      n->cls = src->cls;  // どのクラスかは一緒に複製される（切り取られない）
      n->fields.reserve(src->fields.size());
      for (int i = 0; i < src->fields.size(); i++) n->fields.push(val_retain(src->fields[i]));
      return n;
    }
    case O_Result: {
      ResultObj* src = (ResultObj*)o;
      ResultObj* n = new (sk_alloc(sizeof(ResultObj))) ResultObj();
      n->ok = src->ok; n->val = val_retain(src->val); return n;
    }
    case O_Json: {
      JsonObj* src = (JsonObj*)o;
      JsonObj* n = new (sk_alloc(sizeof(JsonObj))) JsonObj();
      n->jk = src->jk; n->b = src->b; n->num = src->num;
      n->num_is_int = src->num_is_int; n->inum = src->inum; n->s = src->s; n->keys = src->keys;
      for (int i = 0; i < src->items.size(); i++) n->items.push(val_retain(src->items[i]));
      return n;
    }
    default:
      // ハンドル型（File / Task / channel）と、書き換えのない型は複製しない。
      // 呼び出し側は「同じ番地なら何もしない」ので、参照数も動かさない
      return o;
  }
}

Obj* obj_unique(Value& v) {
  if (v.k != V_Obj) return 0;
  if (v.o->rc == 1) return v.o;
  Obj* n = clone_obj(v.o);
  if (n != v.o) { obj_release(v.o); v.o = n; }
  return v.o;
}

// --- 比べる ---------------------------------------------------------------
bool val_equal(const Value& a, const Value& b) {
  if (a.k != b.k) {
    return false;
  }
  switch (a.k) {
    case V_Void: case V_None: return true;
    case V_Int: return a.i == b.i;
    case V_Float: return a.f == b.f;
    case V_Bool: return a.b == b.b;
    case V_Ref: return a.r == b.r;
    case V_Obj: break;
  }
  Obj* x = a.o; Obj* y = b.o;
  if (x == y) return true;
  if (x->kind != y->kind) return false;
  switch (x->kind) {
    case O_Str: case O_Bytes: return ((StrObj*)x)->s == ((StrObj*)y)->s;
    case O_List: {
      ListObj* p = (ListObj*)x; ListObj* q = (ListObj*)y;
      if (p->v.size() != q->v.size()) return false;
      for (int i = 0; i < p->v.size(); i++) if (!val_equal(p->v[i], q->v[i])) return false;
      return true;
    }
    case O_Map: {
      MapObj* p = (MapObj*)x; MapObj* q = (MapObj*)y;
      if (p->live != q->live) return false;
      for (int i = 0; i < p->e.size(); i++) {
        if (p->e[i].dead) continue;
        Value* w = map_find(q, p->e[i].key);
        if (!w || !val_equal(p->e[i].val, *w)) return false;
      }
      return true;
    }
    case O_Inst: {
      // 1. 実体の型が違えば等しくない　2. 同じならメンバを順に比べる
      InstObj* p = (InstObj*)x; InstObj* q = (InstObj*)y;
      if (p->cls != q->cls) return false;
      if (p->fields.size() != q->fields.size()) return false;
      for (int i = 0; i < p->fields.size(); i++) if (!val_equal(p->fields[i], q->fields[i])) return false;
      return true;
    }
    case O_Result: {
      ResultObj* p = (ResultObj*)x; ResultObj* q = (ResultObj*)y;
      return p->ok == q->ok && val_equal(p->val, q->val);
    }
    case O_Time: return ((TimeObj*)x)->unix_ns == ((TimeObj*)y)->unix_ns;
    case O_Dur: return ((DurObj*)x)->ns == ((DurObj*)y)->ns;
    case O_Range: {
      RangeObj* p = (RangeObj*)x; RangeObj* q = (RangeObj*)y;
      return p->start == q->start && p->end == q->end && p->step == q->step;
    }
    case O_Func: return ((FuncObj*)x)->fn == ((FuncObj*)y)->fn;
    default: return x == y;  // ハンドル型は同じ実体かどうか
  }
}

uint64_t val_hash(const Value& v) {
  switch (v.k) {
    case V_Int: return (uint64_t)v.i * 1099511628211ull;
    case V_Bool: return v.b ? 1231u : 1237u;
    case V_Float: {
      double d = v.f;
      if (d == (double)(int64_t)d) return (uint64_t)(int64_t)d * 1099511628211ull;
      uint64_t bits; sk_memcpy(&bits, &d, sizeof d);
      return bits * 1099511628211ull;
    }
    case V_None: return 7ull;
    case V_Obj:
      if (v.o->kind == O_Str || v.o->kind == O_Bytes) return ((StrObj*)v.o)->s.hash();
      return (uint64_t)(uintptr_t)v.o;
    default: return 0;
  }
}

int val_compare(const Value& a, const Value& b) {
  switch (a.k) {
    case V_Int: return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    case V_Float: return a.f < b.f ? -1 : (a.f > b.f ? 1 : 0);
    case V_Bool: return (a.b ? 1 : 0) - (b.b ? 1 : 0);
    case V_Obj: {
      if (a.o->kind == O_Str || a.o->kind == O_Bytes) return ((StrObj*)a.o)->s.cmp(((StrObj*)b.o)->s);
      if (a.o->kind == O_Time) {
        int64_t x = ((TimeObj*)a.o)->unix_ns, y = ((TimeObj*)b.o)->unix_ns;
        return x < y ? -1 : (x > y ? 1 : 0);
      }
      if (a.o->kind == O_Dur) {
        int64_t x = ((DurObj*)a.o)->ns, y = ((DurObj*)b.o)->ns;
        return x < y ? -1 : (x > y ? 1 : 0);
      }
      return 0;
    }
    default: return 0;
  }
}

// --- map ------------------------------------------------------------------
//
// 挿入した順を保ったまま引けるように、並び（e）と索引（idx）を分けて持つ。
// 小さいうちは索引を作らず、順に見る方が速い。
static const int kMapIndexFrom = 8;

static void map_reindex(MapObj* m) {
  int cap = 16;
  while (cap < (m->e.size() + 1) * 2) cap *= 2;
  m->idx.clear();
  m->idx.resize(cap, -1);
  int mask = cap - 1;
  for (int i = 0; i < m->e.size(); i++) {
    if (m->e[i].dead) continue;
    int b = (int)(val_hash(m->e[i].key) & (uint64_t)mask);
    while (m->idx[b] >= 0) b = (b + 1) & mask;
    m->idx[b] = i;
  }
}

Value* map_find(MapObj* m, const Value& key) {
  if (m->idx.size() == 0) {
    for (int i = 0; i < m->e.size(); i++) {
      if (m->e[i].dead) continue;
      if (val_equal(m->e[i].key, key)) return &m->e[i].val;
    }
    return 0;
  }
  int mask = m->idx.size() - 1;
  int b = (int)(val_hash(key) & (uint64_t)mask);
  for (int step = 0; step <= mask; step++) {
    int slot = m->idx[b];
    if (slot == -1) return 0;
    if (slot >= 0 && !m->e[slot].dead && val_equal(m->e[slot].key, key)) return &m->e[slot].val;
    b = (b + 1) & mask;
  }
  return 0;
}

void map_set(MapObj* m, const Value& key, const Value& val) {
  Value* p = map_find(m, key);
  if (p) { Value old = *p; *p = val_retain(val); val_release(old); return; }
  MapEntry en;
  en.key = val_retain(key);
  en.val = val_retain(val);
  m->e.push(en);
  m->live++;

  if (m->idx.size() == 0) {
    if (m->e.size() > kMapIndexFrom) map_reindex(m);
    return;
  }
  // 詰まってきたら索引を作り直す（消した跡もここで片付く）
  if (m->e.size() * 10 >= m->idx.size() * 7) {
    map_reindex(m);
    return;
  }
  int mask = m->idx.size() - 1;
  int b = (int)(val_hash(key) & (uint64_t)mask);
  while (m->idx[b] >= 0) b = (b + 1) & mask;
  m->idx[b] = m->e.size() - 1;
}

bool map_remove(MapObj* m, const Value& key) {
  if (m->idx.size() == 0) {
    for (int i = 0; i < m->e.size(); i++) {
      if (m->e[i].dead) continue;
      if (!val_equal(m->e[i].key, key)) continue;
      val_release(m->e[i].key);
      val_release(m->e[i].val);
      m->e[i].dead = true;
      m->live--;
      return true;
    }
    return false;
  }
  int mask = m->idx.size() - 1;
  int b = (int)(val_hash(key) & (uint64_t)mask);
  for (int step = 0; step <= mask; step++) {
    int slot = m->idx[b];
    if (slot == -1) return false;
    if (slot >= 0 && !m->e[slot].dead && val_equal(m->e[slot].key, key)) {
      val_release(m->e[slot].key);
      val_release(m->e[slot].val);
      m->e[slot].dead = true;
      m->idx[b] = -2;   // 消した跡。ここで探索を止めない
      m->live--;
      return true;
    }
    b = (b + 1) & mask;
  }
  return false;
}

// --- UTF-8 ----------------------------------------------------------------
static int utf8_seq_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

int utf8_len(const Str& s) {
  int n = 0;
  for (int i = 0; i < s.size();) { i += utf8_seq_len((unsigned char)s[i]); n++; }
  return n;
}

int utf8_offset(const Str& s, int chars) {
  int i = 0, n = 0;
  while (i < s.size() && n < chars) { i += utf8_seq_len((unsigned char)s[i]); n++; }
  return i;
}

int utf8_decode(const Str& s, int at, int* cp) {
  unsigned char c = (unsigned char)s[at];
  int len = utf8_seq_len(c);
  if (at + len > s.size()) { *cp = c; return 1; }
  int v = 0;
  if (len == 1) v = c;
  else if (len == 2) v = c & 0x1F;
  else if (len == 3) v = c & 0x0F;
  else v = c & 0x07;
  for (int k = 1; k < len; k++) v = (v << 6) | ((unsigned char)s[at + k] & 0x3F);
  *cp = v;
  return len;
}

void utf8_encode(Str& out, int cp) {
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

// 全角は 2 と数える（spec/library/text.md）
static bool cp_is_wide(int cp) {
  return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
         (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
         (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

int utf8_display_width(const Str& s) {
  int w = 0;
  for (int i = 0; i < s.size();) {
    int cp; i += utf8_decode(s, i, &cp);
    w += cp_is_wide(cp) ? 2 : 1;
  }
  return w;
}

// --- 表示 -----------------------------------------------------------------
Str val_to_display(const Value& v) {
  switch (v.k) {
    case V_Void: return Str("void");
    case V_None: return Str("none");
    case V_Int: return str_from_int(v.i);
    case V_Float: return str_from_float(v.f);
    case V_Bool: return Str(v.b ? "true" : "false");
    case V_Ref: return v.r ? val_to_display(*v.r) : Str("ref");
    case V_Obj: break;
  }
  switch (v.o->kind) {
    case O_Str: return ((StrObj*)v.o)->s;
    case O_Bytes: {
      Str r("b\"");
      const Str& s = ((StrObj*)v.o)->s;
      for (int i = 0; i < s.size(); i++) {
        r += "\\x";
        r += str_from_uint_base((unsigned char)s[i] >> 4, 16, false);
        r += str_from_uint_base((unsigned char)s[i] & 15, 16, false);
      }
      r += "\"";
      return r;
    }
    case O_List: {
      ListObj* l = (ListObj*)v.o;
      Str r("[");
      for (int i = 0; i < l->v.size(); i++) {
        if (i) r += ", ";
        bool q = l->v[i].k == V_Obj && l->v[i].o->kind == O_Str;
        if (q) r += "\"";
        r += val_to_display(l->v[i]);
        if (q) r += "\"";
      }
      r += "]";
      return r;
    }
    case O_Map: {
      MapObj* m = (MapObj*)v.o;
      Str r("{");
      bool first = true;
      for (int i = 0; i < m->e.size(); i++) {
        if (m->e[i].dead) continue;
        if (!first) r += ", ";
        first = false;
        bool qk = m->e[i].key.k == V_Obj && m->e[i].key.o->kind == O_Str;
        if (qk) r += "\"";
        r += val_to_display(m->e[i].key);
        if (qk) r += "\"";
        r += ": ";
        bool qv = m->e[i].val.k == V_Obj && m->e[i].val.o->kind == O_Str;
        if (qv) r += "\"";
        r += val_to_display(m->e[i].val);
        if (qv) r += "\"";
      }
      r += "}";
      return r;
    }
    case O_Result: {
      ResultObj* r = (ResultObj*)v.o;
      if (r->ok) return Str("ok(") + val_to_display(r->val) + ")";
      return Str("error(") + val_to_display(r->val) + ")";
    }
    case O_Range: {
      RangeObj* r = (RangeObj*)v.o;
      return Str("range(") + str_from_int(r->start) + ", " + str_from_int(r->end) + ")";
    }
    case O_Time: return Str("Time");
    case O_Dur: return str_from_float((double)((DurObj*)v.o)->ns / 1e9) + "s";
    case O_Inst: return inst_to_display((InstObj*)v.o);
    case O_Json: return json_to_text(v, -1);
    default: return Str("<") + obj_kind_name(v.o->kind) + ">";
  }
}

const char* obj_kind_name(ObjKind k) {
  switch (k) {
    case O_Str: return "string";
    case O_Bytes: return "bytes";
    case O_List: return "list";
    case O_Map: return "map";
    case O_Inst: return "object";
    case O_Result: return "Result";
    case O_Range: return "Range";
    case O_Func: return "func";
    case O_Time: return "Time";
    case O_Dur: return "Duration";
    case O_File: return "File";
    case O_Json: return "Json";
    case O_Task: return "Task";
    case O_Chan: return "channel";
    case O_Regex: return "Regex";
    case O_Match: return "Match";
  }
  return "?";
}

}  // namespace shark

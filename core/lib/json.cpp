// json.cpp — std.json（spec/library/json.md）
//
// 添字は常に Json を返す。無いキーや範囲外でも止まらず「無い」を表す Json になる。
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

static Value json_num(double d, bool is_int, int64_t iv) {
  Value v = mk_json();
  JsonObj* j = as_json(v);
  j->jk = J_Num;
  j->num = d;
  j->num_is_int = is_int;
  j->inum = iv;
  return v;
}

// --- 読む ---
static const int kJsonMaxDepth = 200;

struct JParser {
  const Str& s;
  int i;
  int depth;
  Str err;
  JParser(const Str& src) : s(src), i(0), depth(0) {}

  // 深くなりすぎないように数える（壊れた入力で C++ 側のスタックが尽きないため）
  struct Deeper {
    JParser* p;
    Deeper(JParser* q) : p(q) { p->depth++; }
    ~Deeper() { p->depth--; }
  };

  bool read_hex4(int* out) {
    int v = 0;
    for (int k = 0; k < 4; k++) {
      if (i >= s.size()) return false;
      char h = s[i];
      int d = (h >= '0' && h <= '9') ? h - '0'
              : (h >= 'a' && h <= 'f') ? h - 'a' + 10
              : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
      if (d < 0) return false;
      v = v * 16 + d;
      i++;
    }
    *out = v;
    return true;
  }

  void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
  bool lit(const char* w) {
    int n = (int)sk_strlen(w);
    if (i + n > s.size()) return false;
    for (int k = 0; k < n; k++) if (s[i + k] != w[k]) return false;
    i += n;
    return true;
  }
  Value parse() {
    ws();
    if (i >= s.size()) { err = Str("中身がありません"); return mk_json(); }
    char c = s[i];
    if (c == '{') return parse_obj();
    if (c == '[') return parse_arr();
    if (c == '"') {
      Str out;
      if (!parse_str(&out)) return mk_json();
      Value v = mk_json();
      as_json(v)->jk = J_Str;
      as_json(v)->s = out;
      return v;
    }
    if (lit("true")) { Value v = mk_json(); as_json(v)->jk = J_Bool; as_json(v)->b = true; return v; }
    if (lit("false")) { Value v = mk_json(); as_json(v)->jk = J_Bool; as_json(v)->b = false; return v; }
    if (lit("null")) return mk_json();
    return parse_num();
  }
  Value parse_num() {
    int start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
    bool isint = true;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                            s[i] == '-' || s[i] == '+')) {
      if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') isint = false;
      i++;
    }
    Str num = s.sub(start, i - start);
    if (num.size() == 0) { err = Str("読めない値があります"); return mk_json(); }
    double d = 0;
    int64_t iv = 0;
    if (isint && str_to_int(num, &iv)) return json_num((double)iv, true, iv);
    if (!str_to_float(num, &d)) { err = Str("数として読めません: ") + num; return mk_json(); }
    return json_num(d, false, (int64_t)d);
  }
  bool parse_str(Str* out) {
    if (s[i] != '"') { err = Str("文字列は \" で始めます"); return false; }
    i++;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) {
        i++;
        char e = s[i++];
        switch (e) {
          case 'n': out->push('\n'); break;
          case 't': out->push('\t'); break;
          case 'r': out->push('\r'); break;
          case 'b': out->push('\b'); break;
          case 'f': out->push('\f'); break;
          case '"': out->push('"'); break;
          case '\\': out->push('\\'); break;
          case '/': out->push('/'); break;
          case 'u': {
            int v = 0;
            if (!read_hex4(&v)) { err = Str("\\u の後ろには 16 進 4 桁が要ります"); return false; }
            // 上位のサロゲートは、続く \uXXXX と組にして1文字にする
            if (v >= 0xD800 && v <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
              int save = i;
              i += 2;
              int lo = 0;
              if (read_hex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
              } else {
                i = save;   // 組になっていなければ、そのままの文字として扱う
              }
            }
            utf8_encode(*out, v);
            break;
          }
          default: out->push(e); break;
        }
        continue;
      }
      out->push(s[i++]);
    }
    if (i >= s.size()) { err = Str("文字列が閉じていません"); return false; }
    i++;
    return true;
  }
  Value parse_obj() {
    Deeper deeper(this);
    if (depth > kJsonMaxDepth) { err = Str("入れ子が深すぎます"); return mk_json(); }
    i++;  // {
    Value v = mk_json();
    JsonObj* j = as_json(v);
    j->jk = J_Map;
    ws();
    if (i < s.size() && s[i] == '}') { i++; return v; }
    for (;;) {
      ws();
      Str key;
      if (!parse_str(&key)) return v;
      ws();
      if (i >= s.size() || s[i] != ':') { err = Str("キーの後ろに : が要ります"); return v; }
      i++;
      Value val = parse();
      j->keys.push(key);
      j->items.push(val);
      if (err.size()) return v;
      ws();
      if (i < s.size() && s[i] == ',') { i++; continue; }
      if (i < s.size() && s[i] == '}') { i++; return v; }
      err = Str("} が要ります");
      return v;
    }
  }
  Value parse_arr() {
    Deeper deeper(this);
    if (depth > kJsonMaxDepth) { err = Str("入れ子が深すぎます"); return mk_json(); }
    i++;  // [
    Value v = mk_json();
    JsonObj* j = as_json(v);
    j->jk = J_List;
    ws();
    if (i < s.size() && s[i] == ']') { i++; return v; }
    for (;;) {
      Value item = parse();
      j->items.push(item);
      if (err.size()) return v;
      ws();
      if (i < s.size() && s[i] == ',') { i++; continue; }
      if (i < s.size() && s[i] == ']') { i++; return v; }
      err = Str("] が要ります");
      return v;
    }
  }
};

// --- 書く ---
static void esc(Str& out, const Str& s) {
  out += "\"";
  for (int i = 0; i < s.size(); i++) {
    char c = s[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if ((unsigned char)c < 0x20) {
          out += "\\u00";
          out += str_from_uint_base((unsigned char)c >> 4, 16, false);
          out += str_from_uint_base((unsigned char)c & 15, 16, false);
        } else {
          out.push(c);
        }
        break;
    }
  }
  out += "\"";
}

static void write_json(Str& out, const Value& v, int indent, int depth) {
  if (v.k != V_Obj || v.o->kind != O_Json) { out += "null"; return; }
  if (depth > 1000) { out += "null"; return; }   // 深すぎるものは切る
  JsonObj* j = (JsonObj*)v.o;
  Str nl = indent >= 0 ? Str("\n") : Str();
  Str pad, pad2;
  for (int i = 0; i < indent * depth && indent > 0; i++) pad += " ";
  for (int i = 0; i < indent * (depth + 1) && indent > 0; i++) pad2 += " ";
  switch (j->jk) {
    case J_None: out += "null"; break;
    case J_Bool: out += j->b ? "true" : "false"; break;
    case J_Num: out += j->num_is_int ? str_from_int(j->inum) : str_from_float(j->num); break;
    case J_Str: esc(out, j->s); break;
    case J_List: {
      if (j->items.size() == 0) { out += "[]"; break; }
      out += "[";
      out += nl;
      for (int i = 0; i < j->items.size(); i++) {
        if (i) { out += ","; out += nl; }
        out += pad2;
        write_json(out, j->items[i], indent, depth + 1);
      }
      out += nl;
      out += pad;
      out += "]";
      break;
    }
    case J_Map: {
      if (j->items.size() == 0) { out += "{}"; break; }
      out += "{";
      out += nl;
      for (int i = 0; i < j->items.size(); i++) {
        if (i) { out += ","; out += nl; }
        out += pad2;
        esc(out, j->keys[i]);
        out += indent >= 0 ? ": " : ":";
        write_json(out, j->items[i], indent, depth + 1);
      }
      out += nl;
      out += pad;
      out += "}";
      break;
    }
  }
}

Str json_to_text(const Value& v, int indent) {
  Str out;
  write_json(out, v, indent, 0);
  return out;
}

static NativeStatus j_parse(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  JParser p(S(a, 0));
  Value v = p.parse();
  if (p.err.size()) {
    val_release(v);
    out = mk_result_err(vm.make_error(Str("JSON を読めません: ") + p.err, 0));
    return N_Ok;
  }
  out = mk_result_ok(v);
  val_release(v);
  return N_Ok;
}
// ファイルから読む（std.file を入れている処理系だけ）
static NativeStatus j_parse_file(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  if (!platform().file) {
    out = mk_result_err(vm.make_error(Str("この処理系はファイルを読めません"), 0));
    return N_Ok;
  }
  Str err;
  void* h = platform().file->open(S(a, 0).c_str(), "r", &err);
  if (!h) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  Str body;
  char buf[4096];
  for (;;) {
    int got = platform().file->read(h, buf, sizeof buf);
    if (got <= 0) break;
    body.append(buf, got);
  }
  platform().file->close(h);
  JParser p(body);
  Value v = p.parse();
  if (p.err.size()) {
    val_release(v);
    out = mk_result_err(vm.make_error(Str("JSON を読めません: ") + p.err, 0));
    return N_Ok;
  }
  out = mk_result_ok(v);
  val_release(v);
  return N_Ok;
}

static NativeStatus j_stringify(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_str(json_to_text(*A(a, 0), -1));
  return N_Ok;
}
static NativeStatus j_stringify_pretty(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_str(json_to_text(*A(a, 0), (int)A(a, 1)->i));
  return N_Ok;
}
static NativeStatus j_none(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)a; (void)n;
  out = mk_json();
  return N_Ok;
}
static NativeStatus j_of(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  Value* v = A(a, 0);
  switch (v->k) {
    case V_Int: out = json_num((double)v->i, true, v->i); return N_Ok;
    case V_Float: out = json_num(v->f, false, (int64_t)v->f); return N_Ok;
    case V_Bool: {
      out = mk_json();
      as_json(out)->jk = J_Bool;
      as_json(out)->b = v->b;
      return N_Ok;
    }
    case V_None: out = mk_json(); return N_Ok;
    default: break;
  }
  if (v->k == V_Obj && v->o->kind == O_Str) {
    out = mk_json();
    as_json(out)->jk = J_Str;
    as_json(out)->s = ((StrObj*)v->o)->s;
    return N_Ok;
  }
  if (v->k == V_Obj && v->o->kind == O_List) {
    out = mk_json();
    JsonObj* j = as_json(out);
    j->jk = J_List;
    ListObj* l = (ListObj*)v->o;
    for (int i = 0; i < l->v.size(); i++) j->items.push(val_retain(l->v[i]));
    return N_Ok;
  }
  if (v->k == V_Obj && v->o->kind == O_Map) {
    out = mk_json();
    JsonObj* j = as_json(out);
    j->jk = J_Map;
    MapObj* m = (MapObj*)v->o;
    for (int i = 0; i < m->e.size(); i++) {
      if (m->e[i].dead) continue;
      j->keys.push(val_to_display(m->e[i].key));
      j->items.push(val_retain(m->e[i].val));
    }
    return N_Ok;
  }
  if (v->k == V_Obj && v->o->kind == O_Json) { out = val_retain(*v); return N_Ok; }
  out = mk_json();
  return N_Ok;
}

// --- Json のメソッド ---
static NativeStatus jm_string(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  JsonObj* j = as_json(*A(a, 0));
  out = j->jk == J_Str ? mk_str(j->s) : mk_none();
  return N_Ok;
}
static NativeStatus jm_int(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  JsonObj* j = as_json(*A(a, 0));
  out = j->jk == J_Num ? mk_int(j->num_is_int ? j->inum : (int64_t)j->num) : mk_none();
  return N_Ok;
}
static NativeStatus jm_float(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  JsonObj* j = as_json(*A(a, 0));
  out = j->jk == J_Num ? mk_float(j->num) : mk_none();
  return N_Ok;
}
static NativeStatus jm_bool(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  JsonObj* j = as_json(*A(a, 0));
  out = j->jk == J_Bool ? mk_bool(j->b) : mk_none();
  return N_Ok;
}
static NativeStatus jm_list(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  JsonObj* j = as_json(*A(a, 0));
  out = mk_list();
  if (j->jk == J_List)
    for (int i = 0; i < j->items.size(); i++) as_list(out)->v.push(val_retain(j->items[i]));
  return N_Ok;
}
static NativeStatus jm_keys(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  JsonObj* j = as_json(*A(a, 0));
  out = mk_list();
  if (j->jk == J_Map)
    for (int i = 0; i < j->keys.size(); i++) as_list(out)->v.push(mk_str(j->keys[i]));
  return N_Ok;
}
static NativeStatus jm_exists(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_bool(as_json(*A(a, 0))->jk != J_None);
  return N_Ok;
}
static NativeStatus jm_kind(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  switch (as_json(*A(a, 0))->jk) {
    case J_None: out = mk_str("none"); break;
    case J_Bool: out = mk_str("bool"); break;
    case J_Num: out = mk_str("number"); break;
    case J_Str: out = mk_str("string"); break;
    case J_List: out = mk_str("list"); break;
    case J_Map: out = mk_str("map"); break;
  }
  return N_Ok;
}

void register_json(Registry& r) {
  TypeTable& t = r.types();
  Type* ts = t.t_string();
  Type* ti = t.t_int();
  Type* tf = t.t_float();
  Type* tb = t.t_bool();
  Type* tj = t.simple(T_Json);
  r.add("json.parse", j_parse, t.result_of(tj), ts);
  if (platform().file) r.add("json.parse_file", j_parse_file, t.result_of(tj), ts);
  r.add("json.stringify", j_stringify, ts, tj);
  r.add("json.stringify_pretty", j_stringify_pretty, ts, tj, ti);
  r.add("json.none", j_none, tj);
  r.add("json.of", j_of, tj, ti);
  r.add("json.of", j_of, tj, tf);
  r.add("json.of", j_of, tj, ts);
  r.add("json.of", j_of, tj, tb);
  r.add("json.of", j_of, tj, t.list_of(tj));
  r.add("json.of", j_of, tj, t.map_of(ts, tj));
  r.add_untyped("json.Json.string", jm_string);
  r.add_untyped("json.Json.int", jm_int);
  r.add_untyped("json.Json.float", jm_float);
  r.add_untyped("json.Json.bool", jm_bool);
  r.add_untyped("json.Json.list", jm_list);
  r.add_untyped("json.Json.keys", jm_keys);
  r.add_untyped("json.Json.exists", jm_exists);
  r.add_untyped("json.Json.kind", jm_kind);
  r.enable_module("std.json");
}

}  // namespace shark

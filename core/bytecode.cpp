// bytecode.cpp — Program をバイト列にする／から戻す（spec/runtime/bytecode.md）
//
// 数はほとんど可変長（下位 7 ビットずつ、続きがあれば最上位ビットを立てる）で入れる。
// 位置を指すもの（型・クラス・関数）は番号にし、「無い」は -1 で表す。
#include "bytecode.h"

namespace shark {

const char kBytecodeMagic[4] = {'S', 'H', 'K', 'C'};

static Str L(Lang lang, const char* ja, const char* en) { return Str(lang == LANG_JA ? ja : en); }

// 定数の種類（FuncInfo::consts に入る値だけ）
enum ConstTag { C_Void = 0, C_None, C_Int, C_Float, C_Bool, C_Str, C_Bytes, C_Func };

// 関数の印
enum FuncFlag {
  FF_Public = 1, FF_Method = 2, FF_Init = 4, FF_Native = 8, FF_Virtual = 16,
  FF_Override = 32, FF_Pure = 64, FF_Test = 128, FF_Generic = 256,
};
enum ClassFlag { CF_Public = 1, CF_Abstract = 2, CF_Interface = 4, CF_Builtin = 8 };
enum GlobalFlag { GF_Public = 1, GF_Const = 2 };

uint32_t modules_bits(const Config& cfg) {
  uint32_t b = 0;
  if (cfg.with_file) b |= 1;
  if (cfg.with_path) b |= 2;
  if (cfg.with_text) b |= 4;
  if (cfg.with_fmt) b |= 8;
  if (cfg.with_json) b |= 16;
  if (cfg.with_os) b |= 32;
  if (cfg.with_crypto) b |= 64;
  if (cfg.with_test) b |= 128;
  return b;
}

void modules_to_config(uint32_t bits, Config* cfg) {
  cfg->with_file = (bits & 1) != 0;
  cfg->with_path = (bits & 2) != 0;
  cfg->with_text = (bits & 4) != 0;
  cfg->with_fmt = (bits & 8) != 0;
  cfg->with_json = (bits & 16) != 0;
  cfg->with_os = (bits & 32) != 0;
  cfg->with_crypto = (bits & 64) != 0;
  cfg->with_test = (bits & 128) != 0;
}

// ------------------------------------------------------------------ 書く道具
namespace {

struct Writer {
  Str b;
  void u8(int v) { b.push((char)(v & 0xff)); }
  void fixed32(uint32_t v) { for (int i = 0; i < 4; i++) u8((int)((v >> (8 * i)) & 0xff)); }
  void fixed64(uint64_t v) { for (int i = 0; i < 8; i++) u8((int)((v >> (8 * i)) & 0xff)); }
  void uv(uint64_t v) {
    while (v >= 0x80) { u8((int)((v & 0x7f) | 0x80)); v >>= 7; }
    u8((int)v);
  }
  void sv(int64_t v) { uv(((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }
  void f64(double d) { uint64_t u = 0; sk_memcpy(&u, &d, sizeof u); fixed64(u); }
  void str(const Str& s) { uv((uint64_t)s.size()); b.append(s.data(), s.size()); }
};

struct Reader {
  const Str& b;
  int p;
  bool bad;
  explicit Reader(const Str& s) : b(s), p(0), bad(false) {}
  int u8() {
    if (p >= b.size()) { bad = true; return 0; }
    return (unsigned char)b[p++];
  }
  uint32_t fixed32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)u8() << (8 * i); return v; }
  uint64_t fixed64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)u8() << (8 * i); return v; }
  uint64_t uv() {
    uint64_t v = 0;
    int shift = 0;
    for (;;) {
      int c = u8();
      if (bad) return 0;
      v |= (uint64_t)(c & 0x7f) << shift;
      if (!(c & 0x80)) break;
      shift += 7;
      if (shift > 63) { bad = true; return 0; }
    }
    return v;
  }
  int64_t sv() { uint64_t u = uv(); return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }
  double f64() { uint64_t u = fixed64(); double d = 0; sk_memcpy(&d, &u, sizeof d); return d; }
  int idx() { return (int)sv(); }
  // 個数。壊れたファイルで途方もない数を読んだら、そこで止める
  int count() {
    uint64_t n = uv();
    if (n > 0x4000000ull || (int)n > b.size()) { bad = true; return 0; }
    return (int)n;
  }
  Str str() {
    int n = count();
    if (bad || p + n > b.size()) { bad = true; return Str(); }
    Str s = b.sub(p, n);
    p += n;
    return s;
  }
};

// 型を並べる。指しているものが必ず先に来るように、深いところから入れる
struct TypeIndex {
  Vec<Type*> order;
  Vec<Type*> working;
  bool cycle;
  TypeIndex() : cycle(false) {}

  int find(Type* t) const {
    for (int i = 0; i < order.size(); i++) if (order[i] == t) return i;
    return -1;
  }
  int put(Type* t) {
    if (!t) return -1;
    int i = find(t);
    if (i >= 0) return i;
    for (int k = 0; k < working.size(); k++) if (working[k] == t) { cycle = true; return -1; }
    working.push(t);
    put(t->a);
    put(t->b);
    put(t->ret);
    for (int k = 0; k < t->params.size(); k++) put(t->params[k]);
    for (int k = 0; k < t->targs.size(); k++) put(t->targs[k]);
    working.pop();
    order.push(t);
    return order.size() - 1;
  }
};

int class_index(Program& prog, ClassInfo* c) {
  if (!c) return -1;
  for (int i = 0; i < prog.classes.size(); i++) if (prog.classes[i] == c) return i;
  return -1;
}

bool write_const(Writer& w, const Value& v, Lang lang, Str* err) {
  if (v.k == V_Void) { w.u8(C_Void); return true; }
  if (v.k == V_None) { w.u8(C_None); return true; }
  if (v.k == V_Int) { w.u8(C_Int); w.sv(v.i); return true; }
  if (v.k == V_Float) { w.u8(C_Float); w.f64(v.f); return true; }
  if (v.k == V_Bool) { w.u8(C_Bool); w.u8(v.b ? 1 : 0); return true; }
  if (v.k == V_Obj && v.o) {
    if (v.o->kind == O_Str) { w.u8(C_Str); w.str(((StrObj*)v.o)->s); return true; }
    if (v.o->kind == O_Bytes) { w.u8(C_Bytes); w.str(((StrObj*)v.o)->s); return true; }
    if (v.o->kind == O_Func) { w.u8(C_Func); w.sv(((FuncObj*)v.o)->fn); return true; }
  }
  *err = L(lang, "この種類の定数は保存できません", "this kind of constant cannot be saved");
  return false;
}

bool read_const(Reader& r, int nfuncs, Value* out, Lang lang, Str* err) {
  int tag = r.u8();
  switch (tag) {
    case C_Void: *out = mk_void(); return true;
    case C_None: *out = mk_none(); return true;
    case C_Int: *out = mk_int(r.sv()); return true;
    case C_Float: *out = mk_float(r.f64()); return true;
    case C_Bool: *out = mk_bool(r.u8() != 0); return true;
    case C_Str: *out = mk_str(r.str()); return true;
    case C_Bytes: *out = mk_bytes(r.str()); return true;
    case C_Func: {
      int fn = r.idx();
      if (fn < 0 || fn >= nfuncs) { r.bad = true; return false; }
      *out = mk_func(fn);
      return true;
    }
    default:
      *err = L(lang, "知らない種類の定数が入っています", "unknown constant kind in the bytecode");
      return false;
  }
}

}  // namespace

// ------------------------------------------------------------------ 書く
bool bytecode_write(Program& prog, const Registry& reg, const BytecodeHeader& h, Str* out,
                    Str* err) {
  Lang lang = h.lang;
  // 指しているものを集める（型は、指す先が先に並ぶ順にする）
  TypeIndex tix;
  for (int i = 0; i < prog.globals.size(); i++) tix.put(prog.globals[i]->type);
  for (int i = 0; i < prog.classes.size(); i++) {
    ClassInfo* c = prog.classes[i];
    for (int k = 0; k < c->fields.size(); k++) tix.put(c->fields[k].type);
    for (int k = 0; k < c->gtypes.size(); k++) tix.put(c->gtypes[k]);
  }
  for (int i = 0; i < prog.funcs.size(); i++) {
    FuncInfo* f = prog.funcs[i];
    for (int k = 0; k < f->params.size(); k++) tix.put(f->params[k].type);
    tix.put(f->ret);
    for (int k = 0; k < f->gtypes.size(); k++) tix.put(f->gtypes[k]);
  }
  if (tix.cycle) {
    *err = L(lang, "型が輪になっていて保存できません", "cannot save: the types form a cycle");
    return false;
  }

  Writer w;
  // --- 覚え書き ---
  for (int i = 0; i < 4; i++) w.u8(kBytecodeMagic[i]);
  w.fixed32((uint32_t)kBytecodeVersion);
  w.str(h.main_file);
  w.u8(h.lang == LANG_EN ? 1 : 0);
  w.fixed32((uint32_t)h.memory_mb);
  w.fixed32(h.modules);
  w.fixed64(registry_signature(reg));

  // --- 大きさ（先に読めるように） ---
  w.uv((uint64_t)prog.classes.size());
  w.uv((uint64_t)prog.funcs.size());
  w.uv((uint64_t)tix.order.size());

  // --- 型 ---
  for (int i = 0; i < tix.order.size(); i++) {
    Type* t = tix.order[i];
    w.u8((int)t->kind);
    w.sv(tix.find(t->a));
    w.sv(tix.find(t->b));
    w.sv(class_index(prog, t->cls));
    w.str(t->name);
    w.uv((uint64_t)t->params.size());
    for (int k = 0; k < t->params.size(); k++) w.sv(tix.find(t->params[k]));
    w.sv(tix.find(t->ret));
    w.uv((uint64_t)t->constraints.size());
    for (int k = 0; k < t->constraints.size(); k++) w.sv(class_index(prog, t->constraints[k]));
    w.uv((uint64_t)t->targs.size());
    for (int k = 0; k < t->targs.size(); k++) w.sv(tix.find(t->targs[k]));
  }

  // --- クラス ---
  for (int i = 0; i < prog.classes.size(); i++) {
    ClassInfo* c = prog.classes[i];
    w.str(c->name);
    w.str(c->module);
    w.sv(class_index(prog, c->base));
    w.uv((uint64_t)c->interfaces.size());
    for (int k = 0; k < c->interfaces.size(); k++) w.sv(class_index(prog, c->interfaces[k]));
    w.uv((uint64_t)c->fields.size());
    for (int k = 0; k < c->fields.size(); k++) {
      const FieldInfo& f = c->fields[k];
      w.str(f.name);
      w.sv(tix.find(f.type));
      w.u8(f.is_public ? 1 : 0);
      w.sv(class_index(prog, f.owner));
    }
    w.uv((uint64_t)c->methods.size());
    for (int k = 0; k < c->methods.size(); k++) {
      w.str(c->methods[k].name);
      w.sv(c->methods[k].func);
      w.u8(c->methods[k].is_public ? 1 : 0);
    }
    w.uv((uint64_t)c->vtable.size());
    for (int k = 0; k < c->vtable.size(); k++) w.sv(c->vtable[k]);
    int flags = 0;
    if (c->is_public) flags |= CF_Public;
    if (c->is_abstract) flags |= CF_Abstract;
    if (c->is_interface) flags |= CF_Interface;
    if (c->is_builtin) flags |= CF_Builtin;
    w.uv((uint64_t)flags);
    w.uv((uint64_t)c->gparams.size());
    for (int k = 0; k < c->gparams.size(); k++) w.str(c->gparams[k]);
    w.uv((uint64_t)c->gtypes.size());
    for (int k = 0; k < c->gtypes.size(); k++) w.sv(tix.find(c->gtypes[k]));
  }

  // --- 関数 ---
  for (int i = 0; i < prog.funcs.size(); i++) {
    FuncInfo* f = prog.funcs[i];
    w.str(f->name);
    w.str(f->module);
    w.str(f->file);
    w.uv((uint64_t)f->params.size());
    for (int k = 0; k < f->params.size(); k++) {
      w.str(f->params[k].name);
      w.sv(tix.find(f->params[k].type));
      w.u8(f->params[k].is_ref ? 1 : 0);
    }
    w.sv(tix.find(f->ret));
    int flags = 0;
    if (f->is_public) flags |= FF_Public;
    if (f->is_method) flags |= FF_Method;
    if (f->is_init) flags |= FF_Init;
    if (f->is_native) flags |= FF_Native;
    if (f->is_virtual) flags |= FF_Virtual;
    if (f->is_override) flags |= FF_Override;
    if (f->is_pure) flags |= FF_Pure;
    if (f->is_test) flags |= FF_Test;
    if (f->is_generic) flags |= FF_Generic;
    w.uv((uint64_t)flags);
    w.sv(class_index(prog, f->owner));
    w.sv(f->vslot);
    w.uv((uint64_t)f->nlocals);
    w.uv((uint64_t)f->gparams.size());
    for (int k = 0; k < f->gparams.size(); k++) w.str(f->gparams[k]);
    w.uv((uint64_t)f->gtypes.size());
    for (int k = 0; k < f->gtypes.size(); k++) w.sv(tix.find(f->gtypes[k]));
    // 命令の並びと、そこで使う定数と、行番号
    w.uv((uint64_t)f->code.size());
    for (int k = 0; k < f->code.size(); k++) w.u8(f->code[k]);
    w.uv((uint64_t)f->consts.size());
    for (int k = 0; k < f->consts.size(); k++)
      if (!write_const(w, f->consts[k], lang, err)) return false;
    w.uv((uint64_t)f->lines.size());
    for (int k = 0; k < f->lines.size(); k++) w.uv((uint64_t)f->lines[k]);
    // 処理系が持つメソッド（Error など）は、名前でつなぎ直す。関数ポインタは書けない
    if (f->is_native && !builtin_native_method(f->owner ? f->owner->name : Str(), f->name,
                                               f->params.size())) {
      *err = L(lang, "この処理系関数はバイトコードに保存できません",
               "this native function cannot be saved to bytecode");
      *err += ": ";
      *err += f->name;
      return false;
    }
  }

  // --- グローバル ---
  w.uv((uint64_t)prog.globals.size());
  for (int i = 0; i < prog.globals.size(); i++) {
    GlobalInfo* g = prog.globals[i];
    w.str(g->name);
    w.str(g->module);
    w.sv(tix.find(g->type));
    int flags = 0;
    if (g->is_public) flags |= GF_Public;
    if (g->is_const) flags |= GF_Const;
    w.uv((uint64_t)flags);
  }

  // --- 走らせる順 ---
  w.uv((uint64_t)prog.inits.size());
  for (int i = 0; i < prog.inits.size(); i++) w.sv(prog.inits[i]);
  w.sv(prog.entry);

  *out = w.b;
  return true;
}

// ------------------------------------------------------------------ 読む
bool bytecode_read_header(const Str& in, BytecodeHeader* h, Lang lang, Str* err) {
  if (in.size() < 4 || in[0] != kBytecodeMagic[0] || in[1] != kBytecodeMagic[1] ||
      in[2] != kBytecodeMagic[2] || in[3] != kBytecodeMagic[3]) {
    *err = L(lang, "Shark のバイトコードではありません", "not a Shark bytecode file");
    return false;
  }
  Reader r(in);
  r.p = 4;
  h->version = (int)r.fixed32();
  if (h->version != kBytecodeVersion) {
    *err = L(lang, "このバイトコードは別の版で作られています（作り直してください）",
             "this bytecode was made by another version (rebuild it)");
    return false;
  }
  h->main_file = r.str();
  h->lang = r.u8() == 1 ? LANG_EN : LANG_JA;
  h->memory_mb = (int)r.fixed32();
  h->modules = r.fixed32();
  h->natives = r.fixed64();
  if (r.bad) {
    *err = L(lang, "バイトコードの覚え書きが壊れています", "the bytecode header is damaged");
    return false;
  }
  return true;
}

bool bytecode_read(const Str& in, Program* prog, TypeTable& types, const Registry& reg, Lang lang,
                   Str* err) {
  BytecodeHeader h;
  if (!bytecode_read_header(in, &h, lang, err)) return false;
  if (h.natives != registry_signature(reg)) {
    *err = L(lang,
             "このバイトコードは、いまの処理系と組み合わせが違います\n"
             "  同じ版の shark で作り直してください",
             "this bytecode does not match this runtime\n  rebuild it with the same shark");
    return false;
  }

  Reader r(in);
  // 覚え書きを読み飛ばす
  r.p = 4;
  r.fixed32();
  r.str();
  r.u8();
  r.fixed32();
  r.fixed32();
  r.fixed64();

  int nclasses = r.count();
  int nfuncs = r.count();
  int ntypes = r.count();
  if (r.bad) {
    *err = L(lang, "バイトコードが壊れています", "the bytecode is damaged");
    return false;
  }

  // クラスの器を先に作る（型がクラスを指し、クラスが型を指すため）
  for (int i = 0; i < nclasses; i++)
    prog->classes.push(new (sk_alloc(sizeof(ClassInfo))) ClassInfo());

  // --- 型 ---
  Vec<Type*> tys;
  for (int i = 0; i < ntypes; i++) {
    int kind = r.u8();
    int ia = r.idx(), ib = r.idx(), ic = r.idx();
    Str name = r.str();
    int nparams = r.count();
    Vec<Type*> params;
    for (int k = 0; k < nparams; k++) {
      int pi = r.idx();
      params.push(pi >= 0 && pi < tys.size() ? tys[pi] : 0);
    }
    int iret = r.idx();
    int ncons = r.count();
    Vec<ClassInfo*> cons;
    for (int k = 0; k < ncons; k++) {
      int ci = r.idx();
      if (ci >= 0 && ci < nclasses) cons.push(prog->classes[ci]);
    }
    int ntargs = r.count();
    Vec<Type*> targs;
    for (int k = 0; k < ntargs; k++) {
      int ti = r.idx();
      targs.push(ti >= 0 && ti < tys.size() ? tys[ti] : 0);
    }
    if (r.bad || kind < 0 || kind > (int)T_Output) {
      *err = L(lang, "バイトコードの型が壊れています", "the types in the bytecode are damaged");
      return false;
    }
    // 指す先はすでに並んでいる（書くときに深いところから入れてある）
    Type* a = ia >= 0 && ia < tys.size() ? tys[ia] : 0;
    Type* b = ib >= 0 && ib < tys.size() ? tys[ib] : 0;
    Type* ret = iret >= 0 && iret < tys.size() ? tys[iret] : 0;
    ClassInfo* cls = ic >= 0 && ic < nclasses ? prog->classes[ic] : 0;
    Type* t = 0;
    switch ((TypeKind)kind) {
      case T_List: t = types.list_of(a); break;
      case T_Map: t = types.map_of(a, b); break;
      case T_Optional: t = types.optional_of(a); break;
      case T_Result: t = types.result_of(a); break;
      case T_Task: t = types.task_of(a); break;
      case T_Channel: t = types.channel_of(a); break;
      case T_Class: t = types.class_type_args(cls, targs); break;
      case T_Func: t = types.func_type(params, ret); break;
      case T_Generic:
        t = types.generic(name);
        t->constraints = cons;
        break;
      default: t = types.simple((TypeKind)kind); break;
    }
    tys.push(t);
  }

  // --- クラスの中身 ---
  for (int i = 0; i < nclasses; i++) {
    ClassInfo* c = prog->classes[i];
    c->name = r.str();
    c->module = r.str();
    int ibase = r.idx();
    c->base = ibase >= 0 && ibase < nclasses ? prog->classes[ibase] : 0;
    int nif = r.count();
    for (int k = 0; k < nif; k++) {
      int ii = r.idx();
      if (ii >= 0 && ii < nclasses) c->interfaces.push(prog->classes[ii]);
    }
    int nf = r.count();
    for (int k = 0; k < nf; k++) {
      FieldInfo f;
      f.name = r.str();
      int ti = r.idx();
      f.type = ti >= 0 && ti < tys.size() ? tys[ti] : 0;
      f.is_public = r.u8() != 0;
      int oi = r.idx();
      f.owner = oi >= 0 && oi < nclasses ? prog->classes[oi] : 0;
      c->fields.push(f);
    }
    int nm = r.count();
    for (int k = 0; k < nm; k++) {
      MethodRef m;
      m.name = r.str();
      m.func = r.idx();
      m.is_public = r.u8() != 0;
      if (m.func < -1 || m.func >= nfuncs) { r.bad = true; break; }
      c->methods.push(m);
    }
    int nv = r.count();
    for (int k = 0; k < nv; k++) {
      int fi = r.idx();
      // -1 は純粋仮想、-2 はその位置にメソッドが無い（check.cpp の layout_class）
      if (fi < -2 || fi >= nfuncs) { r.bad = true; break; }
      c->vtable.push(fi);
    }
    int flags = (int)r.uv();
    c->is_public = (flags & CF_Public) != 0;
    c->is_abstract = (flags & CF_Abstract) != 0;
    c->is_interface = (flags & CF_Interface) != 0;
    c->is_builtin = (flags & CF_Builtin) != 0;
    c->layout_state = 2;   // 場所は決まったものが入っている
    int ngp = r.count();
    for (int k = 0; k < ngp; k++) c->gparams.push(r.str());
    int ngt = r.count();
    for (int k = 0; k < ngt; k++) {
      int ti = r.idx();
      c->gtypes.push(ti >= 0 && ti < tys.size() ? tys[ti] : 0);
    }
    if (r.bad) {
      *err = L(lang, "バイトコードのクラスが壊れています", "the classes in the bytecode are damaged");
      return false;
    }
  }

  // --- 関数 ---
  for (int i = 0; i < nfuncs; i++) {
    FuncInfo* f = new (sk_alloc(sizeof(FuncInfo))) FuncInfo();
    f->index = i;
    prog->funcs.push(f);
    f->name = r.str();
    f->module = r.str();
    f->file = r.str();
    int np = r.count();
    for (int k = 0; k < np; k++) {
      ParamInfo p;
      p.name = r.str();
      int ti = r.idx();
      p.type = ti >= 0 && ti < tys.size() ? tys[ti] : 0;
      p.is_ref = r.u8() != 0;
      f->params.push(p);
    }
    int iret = r.idx();
    f->ret = iret >= 0 && iret < tys.size() ? tys[iret] : 0;
    int flags = (int)r.uv();
    f->is_public = (flags & FF_Public) != 0;
    f->is_method = (flags & FF_Method) != 0;
    f->is_init = (flags & FF_Init) != 0;
    f->is_native = (flags & FF_Native) != 0;
    f->is_virtual = (flags & FF_Virtual) != 0;
    f->is_override = (flags & FF_Override) != 0;
    f->is_pure = (flags & FF_Pure) != 0;
    f->is_test = (flags & FF_Test) != 0;
    f->is_generic = (flags & FF_Generic) != 0;
    int oi = r.idx();
    f->owner = oi >= 0 && oi < nclasses ? prog->classes[oi] : 0;
    f->vslot = r.idx();
    f->nlocals = (int)r.uv();
    int ngp = r.count();
    for (int k = 0; k < ngp; k++) f->gparams.push(r.str());
    int ngt = r.count();
    for (int k = 0; k < ngt; k++) {
      int ti = r.idx();
      f->gtypes.push(ti >= 0 && ti < tys.size() ? tys[ti] : 0);
    }
    int ncode = r.count();
    for (int k = 0; k < ncode; k++) f->code.push((uint8_t)r.u8());
    int nconsts = r.count();
    for (int k = 0; k < nconsts; k++) {
      Value v;
      if (!read_const(r, nfuncs, &v, lang, err)) {
        if (err->size() == 0)
          *err = L(lang, "バイトコードの定数が壊れています", "the constants in the bytecode are damaged");
        return false;
      }
      f->consts.push(v);
    }
    int nlines = r.count();
    for (int k = 0; k < nlines; k++) f->lines.push((uint32_t)r.uv());
    if (f->is_native) {
      f->native = builtin_native_method(f->owner ? f->owner->name : Str(), f->name,
                                        f->params.size());
      if (!f->native) {
        *err = L(lang, "この処理系にない処理系関数を呼んでいます",
                 "the bytecode needs a native function this runtime does not have");
        *err += ": ";
        *err += f->name;
        return false;
      }
    }
    if (r.bad) {
      *err = L(lang, "バイトコードの関数が壊れています", "the functions in the bytecode are damaged");
      return false;
    }
  }

  // --- グローバル ---
  int nglobals = r.count();
  for (int i = 0; i < nglobals; i++) {
    GlobalInfo* g = new (sk_alloc(sizeof(GlobalInfo))) GlobalInfo();
    g->name = r.str();
    g->module = r.str();
    int ti = r.idx();
    g->type = ti >= 0 && ti < tys.size() ? tys[ti] : 0;
    int flags = (int)r.uv();
    g->is_public = (flags & GF_Public) != 0;
    g->is_const = (flags & GF_Const) != 0;
    g->index = i;
    prog->globals.push(g);
  }

  // --- 走らせる順 ---
  int ninits = r.count();
  for (int i = 0; i < ninits; i++) {
    int fi = r.idx();
    if (fi < 0 || fi >= nfuncs) { r.bad = true; break; }
    prog->inits.push(fi);
  }
  prog->entry = r.idx();
  if (prog->entry < -1 || prog->entry >= nfuncs) r.bad = true;
  prog->types = &types;

  if (r.bad) {
    *err = L(lang, "バイトコードが壊れています（最後まで読めません）",
             "the bytecode is damaged (it ends too early)");
    return false;
  }
  return true;
}

}  // namespace shark

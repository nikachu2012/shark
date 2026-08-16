#include "check.h"

#include "opcodes.h"

namespace shark {

// ------------------------------------------------------------------ 準備
Checker::Checker(Program& prog, Registry& reg, TypeTable& types, DiagBag& diag, Arena& arena)
    : prog_(prog), reg_(reg), t_(types), diag_(diag), arena_(arena), unit_(0), fc_(0),
      c_error_(0), c_comparable_(0) {
  t_none_ = t_.optional_of(t_.t_unknown());
  make_builtin_classes();
}

static FuncInfo* new_func(Program& prog) {
  FuncInfo* f = new (sk_alloc(sizeof(FuncInfo))) FuncInfo();
  f->index = prog.funcs.size();
  prog.funcs.push(f);
  return f;
}

static ClassInfo* new_class(Program& prog) {
  ClassInfo* c = new (sk_alloc(sizeof(ClassInfo))) ClassInfo();
  prog.classes.push(c);
  return c;
}

// Error のメソッドは処理系が持つ（本体は lib/builtin.cpp）
NativeStatus n_error_message(VM& vm, Value* args, int n, Value& out);
NativeStatus n_error_code(VM& vm, Value* args, int n, Value& out);
NativeStatus n_error_init1(VM& vm, Value* args, int n, Value& out);
NativeStatus n_error_init2(VM& vm, Value* args, int n, Value& out);

void Checker::make_builtin_classes() {
  // class Error { public virtual func message() -> string; public virtual func code() -> int; }
  c_error_ = new_class(prog_);
  c_error_->name = Str("Error");
  c_error_->module = Str("std");
  c_error_->is_public = true;
  c_error_->is_builtin = true;
  {
    FieldInfo f;
    f.name = Str("message"); f.type = t_.t_string(); f.is_public = false; f.owner = c_error_;
    c_error_->fields.push(f);
    f.name = Str("code"); f.type = t_.t_int();
    c_error_->fields.push(f);
  }
  struct { const char* name; NativeFn fn; bool init; int nparams; Type* ret; } em[] = {
    {"message", n_error_message, false, 0, t_.t_string()},
    {"code", n_error_code, false, 0, t_.t_int()},
    {"init", n_error_init1, true, 1, t_.t_void()},
    {"init", n_error_init2, true, 2, t_.t_void()},
  };
  for (int i = 0; i < 4; i++) {
    FuncInfo* f = new_func(prog_);
    f->name = Str(em[i].name);
    f->module = Str("std");
    f->owner = c_error_;
    f->is_method = true;
    f->is_public = true;
    f->is_native = true;
    f->native = em[i].fn;
    f->is_init = em[i].init;
    f->ret = em[i].ret;
    if (em[i].init) {
      ParamInfo p; p.name = Str("message"); p.type = t_.t_string(); f->params.push(p);
      if (em[i].nparams == 2) { ParamInfo q; q.name = Str("code"); q.type = t_.t_int(); f->params.push(q); }
    } else {
      f->is_virtual = true;   // 独自のエラーで上書きできる
      f->vslot = vslot_for(f);
    }
    MethodRef m; m.name = f->name; m.func = f->index; m.is_public = true;
    c_error_->methods.push(m);
  }
  layout_class(c_error_);

  // class Comparable { virtual func compare(other: This) -> int; }
  c_comparable_ = new_class(prog_);
  c_comparable_->name = Str("Comparable");
  c_comparable_->module = Str("std");
  c_comparable_->is_public = true;
  c_comparable_->is_builtin = true;
  c_comparable_->is_interface = true;
  {
    FuncInfo* f = new_func(prog_);
    f->name = Str("compare");
    f->module = Str("std");
    f->owner = c_comparable_;
    f->is_method = true;
    f->is_public = true;
    f->is_virtual = true;
    f->is_pure = true;
    f->ret = t_.t_int();
    ParamInfo p; p.name = Str("other"); p.type = t_.generic(Str("This")); f->params.push(p);
    f->vslot = vslot_for(f);
    MethodRef m; m.name = f->name; m.func = f->index; m.is_public = true;
    c_comparable_->methods.push(m);
  }
  layout_class(c_comparable_);
}

// virtual の表の位置。同じ名前・同じ引数なら、どのクラスでも同じ位置になる
int Checker::vslot_for(FuncInfo* f) {
  Str key = f->name;
  key += "(";
  for (int i = 0; i < f->params.size(); i++) {
    if (i) key += ",";
    Type* pt = f->params[i].type;
    // 自分のクラス／This はまとめて "This" とみなす（インタフェースの This に合わせる）
    if (pt && pt->kind == T_Generic && pt->name == "This") key += "This";
    else if (pt && pt->kind == T_Class && f->owner && pt->cls == f->owner) key += "This";
    else key += type_name(pt);
  }
  key += ")";
  for (int i = 0; i < vkeys_.size(); i++) if (vkeys_[i] == key) return i;
  vkeys_.push(key);
  return vkeys_.size() - 1;
}

// ------------------------------------------------------------------ 宣言を集める
void Checker::collect(Unit* u) {
  units_.push(u);
  unit_ = u;
  collect_class_shells(u);
}

void Checker::collect_class_shells(Unit* u) {
  for (int i = 0; i < u->classes.size(); i++) {
    ClassDecl* cd = u->classes[i];
    ClassInfo* c = new_class(prog_);
    c->name = cd->name;
    c->module = u->module;
    c->is_public = cd->is_public;
    c->decl = cd;
    for (int k = 0; k < cd->gparams.size(); k++) c->gparams.push(cd->gparams[k].name);
    cd->info = c;
  }
}

ClassInfo* Checker::find_class(const Str& name, Unit* u) {
  // "mod.Class" の形
  int dot = -1;
  for (int i = 0; i < name.size(); i++) if (name[i] == '.') dot = i;
  if (dot >= 0) {
    Str alias = name.sub(0, dot);
    Str cname = name.sub(dot + 1, name.size() - dot - 1);
    bool found = false;
    Str mod = module_of_alias(alias, u, &found);
    if (!found) return 0;
    for (int i = 0; i < prog_.classes.size(); i++)
      if (prog_.classes[i]->name == cname && prog_.classes[i]->module == mod &&
          prog_.classes[i]->is_public)
        return prog_.classes[i];
    return 0;
  }
  // 自分のモジュール
  for (int i = 0; i < prog_.classes.size(); i++)
    if (prog_.classes[i]->name == name && prog_.classes[i]->module == u->module)
      return prog_.classes[i];
  // 処理系が持つもの（Error / Comparable）
  for (int i = 0; i < prog_.classes.size(); i++)
    if (prog_.classes[i]->name == name && prog_.classes[i]->module == "std")
      return prog_.classes[i];
  return 0;
}

Str Checker::module_of_alias(const Str& alias, Unit* u, bool* found) {
  *found = false;
  // task と parallel は構文と結び付いているので、import を省略できる
  // （spec/library/overview.md）
  if (alias == "task" && reg_.has_module(Str("std.task"))) { *found = true; return Str("std.task"); }
  for (int i = 0; i < u->imports.size(); i++) {
    const ImportDecl& im = u->imports[i];
    Str shortname = im.alias;
    if (shortname.size() == 0) {
      int last = -1;
      for (int k = 0; k < im.path.size(); k++) if (im.path[k] == '.' || im.path[k] == '/') last = k;
      shortname = im.path.sub(last + 1, im.path.size() - last - 1);
    }
    if (shortname == alias) { *found = true; return im.path; }
  }
  return Str();
}

Type* Checker::resolve_type(TypeExpr* te, Unit* u, FuncCtx* fc, ClassInfo* cls) {
  if (!te) return t_.t_void();
  Type* base = 0;
  const Str& n = te->name;
  int na = te->args.size();
  Vec<Type*> args;
  for (int i = 0; i < na; i++) args.push(resolve_type(te->args[i], u, fc, cls));

  if (n == "func") {
    Vec<Type*> ps;
    for (int i = 0; i < te->fn_params.size(); i++) ps.push(resolve_type(te->fn_params[i], u, fc, cls));
    Type* r = te->fn_ret ? resolve_type(te->fn_ret, u, fc, cls) : t_.t_void();
    base = t_.func_type(ps, r);
  } else if (n == "int") base = t_.t_int();
  else if (n == "float") base = t_.t_float();
  else if (n == "bool") base = t_.t_bool();
  else if (n == "void") base = t_.t_void();
  else if (n == "string") base = t_.t_string();
  else if (n == "bytes") base = t_.t_bytes();
  else if (n == "Range") base = t_.simple(T_Range);
  else if (n == "Time") base = t_.simple(T_Time);
  else if (n == "Duration") base = t_.simple(T_Duration);
  else if (n == "File") base = t_.simple(T_File);
  else if (n == "Json") base = t_.simple(T_Json);
  else if (n == "Regex") base = t_.simple(T_Regex);
  else if (n == "Match") base = t_.simple(T_Match);
  else if (n == "Output") base = t_.simple(T_Output);
  else if (n == "list") {
    if (na != 1) {
      err_at("E0105", diag_.L("list には要素の型を1つ書きます: list<int>",
                              "list takes one type argument: list<int>"), te->line, te->col, te->len);
      return t_.t_unknown();
    }
    base = t_.list_of(args[0]);
  } else if (n == "map") {
    if (na != 2) {
      err_at("E0105", diag_.L("map にはキーと値の型を書きます: map<string, int>",
                              "map takes two type arguments: map<string, int>"), te->line, te->col, te->len);
      return t_.t_unknown();
    }
    base = t_.map_of(args[0], args[1]);
  } else if (n == "Result") {
    base = t_.result_of(na == 1 ? args[0] : t_.t_void());
  } else if (n == "Task") {
    base = t_.task_of(na == 1 ? args[0] : t_.t_void());
  } else if (n == "channel") {
    base = t_.channel_of(na == 1 ? args[0] : t_.t_unknown());
  } else if (n == "This") {
    if (cls) base = t_.class_type(cls);
    else base = t_.generic(Str("This"));
  } else {
    // 型引数
    if (fc) {
      for (int i = 0; i < fc->gnames.size(); i++)
        if (fc->gnames[i] == n) { base = fc->gtypes[i]; break; }
    }
    if (!base && cls) {
      for (int i = 0; i < cls->gparams.size(); i++)
        if (cls->gparams[i] == n) {
          base = i < cls->gtypes.size() ? cls->gtypes[i] : t_.generic(n);
          break;
        }
    }
    if (!base) {
      ClassInfo* c = find_class(n, u);
      if (c) {
        base = args.size() ? t_.class_type_args(c, args) : t_.class_type(c);
      } else {
        Diagnostic& d = diag_.error("E0106", diag_.L(Str("型 ") + n + " が見つかりません",
                                                     Str("unknown type: ") + n));
        d.spans.push(Span(te->line, te->col, te->len > 0 ? te->len : (int)n.size()));
        d.help.push(diag_.L("綴りを確かめるか、定義しているモジュールを import します",
                            "check the spelling, or import the module that defines it"));
        return t_.t_unknown();
      }
    }
  }
  if (te->optional) base = t_.optional_of(base);
  return base;
}

// 型引数を当てはめる
Type* Checker::subst(Type* t, const Vec<Str>& names, const Vec<Type*>& args) {
  if (!t) return t;
  switch (t->kind) {
    case T_Generic: {
      for (int i = 0; i < names.size(); i++) if (names[i] == t->name) return args[i];
      return t;
    }
    case T_List: return t_.list_of(subst(t->a, names, args));
    case T_Map: return t_.map_of(subst(t->a, names, args), subst(t->b, names, args));
    case T_Optional: return t_.optional_of(subst(t->a, names, args));
    case T_Result: return t_.result_of(subst(t->a, names, args));
    case T_Task: return t_.task_of(subst(t->a, names, args));
    case T_Channel: return t_.channel_of(subst(t->a, names, args));
    case T_Class: {
      if (t->targs.size() == 0) return t;
      Vec<Type*> na;
      for (int i = 0; i < t->targs.size(); i++) na.push(subst(t->targs[i], names, args));
      return t_.class_type_args(t->cls, na);
    }
    case T_Func: {
      Vec<Type*> ps;
      for (int i = 0; i < t->params.size(); i++) ps.push(subst(t->params[i], names, args));
      return t_.func_type(ps, subst(t->ret, names, args));
    }
    default: return t;
  }
}

// 型 t がインタフェース iface を満たすか
bool Checker::satisfies(Type* t, ClassInfo* iface) {
  if (!t || !iface) return false;
  if (iface == c_comparable_) {
    // 基本型は処理系が Comparable を実装済みとして扱う
    switch (t->kind) {
      case T_Int: case T_Float: case T_String: case T_Bytes: case T_Bool:
      case T_Time: case T_Duration: case T_Unknown:
        return true;
      default: break;
    }
  }
  if (t->kind == T_Class) return class_is(t->cls, iface);
  if (t->kind == T_Generic) {
    for (int i = 0; i < t->constraints.size(); i++)
      if (class_is(t->constraints[i], iface)) return true;
    return false;
  }
  return t->kind == T_Unknown;
}

// ------------------------------------------------------------------ クラスの中身
void Checker::collect_class_bodies(Unit* u) {
  unit_ = u;
  diag_.set_file(u->display);
  for (int i = 0; i < u->classes.size(); i++) {
    ClassDecl* cd = u->classes[i];
    ClassInfo* c = cd->info;
    // 型引数（制約を付ける）
    c->gtypes.clear();
    for (int k = 0; k < cd->gparams.size(); k++) {
      Type* gt = t_.generic(cd->gparams[k].name);
      for (int ci = 0; ci < cd->gparams[k].constraints.size(); ci++) {
        ClassInfo* ic = find_class(cd->gparams[k].constraints[ci], u);
        if (ic) gt->constraints.push(ic);
      }
      c->gtypes.push(gt);
    }
    // 親とインタフェース
    for (int k = 0; k < cd->bases.size(); k++) {
      ClassInfo* b = find_class(cd->bases[k], u);
      if (!b) {
        err_at("E0106", diag_.L(Str("型 ") + cd->bases[k] + " が見つかりません",
                                Str("unknown type: ") + cd->bases[k]),
               cd->base_lines[k], cd->base_cols[k], cd->base_lens[k]);
        continue;
      }
      if (k == 0) {
        c->base = b;
      } else {
        c->interfaces.push(b);
      }
    }
    // メンバ変数
    for (int k = 0; k < cd->fields.size(); k++) {
      FieldInfo f;
      f.name = cd->fields[k].name;
      f.type = resolve_type(cd->fields[k].type, u, 0, c);
      f.is_public = cd->fields[k].is_public;
      f.owner = c;
      c->fields.push(f);
    }
    // メソッドの型
    for (int k = 0; k < cd->methods.size(); k++) {
      FuncDecl* md = cd->methods[k];
      FuncInfo* f = new_func(prog_);
      md->info = f;
      f->name = md->name;
      f->module = u->module;
      f->owner = c;
      f->is_method = true;
      f->is_public = md->is_public;
      f->is_virtual = md->is_virtual || md->is_override;
      f->is_override = md->is_override;
      f->is_pure = (md->body == 0);
      f->is_init = (md->name == "init");
      f->decl = md;
      FuncCtx tmp;
      for (int g = 0; g < md->gparams.size(); g++) {
        Type* gt = t_.generic(md->gparams[g].name);
        for (int ci = 0; ci < md->gparams[g].constraints.size(); ci++) {
          ClassInfo* ic = find_class(md->gparams[g].constraints[ci], u);
          if (ic) gt->constraints.push(ic);
        }
        tmp.gnames.push(md->gparams[g].name);
        tmp.gtypes.push(gt);
        f->gparams.push(md->gparams[g].name);
        f->gtypes.push(gt);
      }
      f->is_generic = f->gparams.size() > 0;
      for (int p = 0; p < md->params.size(); p++) {
        ParamInfo pi;
        pi.name = md->params[p].name;
        pi.type = resolve_type(md->params[p].type, u, &tmp, c);
        pi.is_ref = md->params[p].is_ref;
        f->params.push(pi);
      }
      f->ret = md->ret ? resolve_type(md->ret, u, &tmp, c) : t_.t_void();
      if (f->is_init && md->ret) {
        err_at("E0407", diag_.L("init に戻り値の型は書けません", "init cannot declare a return type"),
               md->line, md->col, 4);
      }
      if (f->is_virtual) f->vslot = vslot_for(f);
      MethodRef m;
      m.name = f->name;
      m.func = f->index;
      m.is_public = f->is_public;
      c->methods.push(m);
    }
  }
}

void Checker::layout_class(ClassInfo* c) {
  if (c->layout_state == 2) return;
  if (c->layout_state == 1) {
    err_at("E0404", diag_.L(Str("クラス ") + c->name + " の継承が輪になっています",
                            Str("inheritance cycle at class ") + c->name),
           c->decl ? c->decl->line : 1, c->decl ? c->decl->col : 1, (int)c->name.size());
    c->layout_state = 2;
    return;
  }
  c->layout_state = 1;

  if (c->base) {
    layout_class(c->base);
    // 親のメンバを先頭に置く
    Vec<FieldInfo> own;
    own = c->fields;
    c->fields.clear();
    for (int i = 0; i < c->base->fields.size(); i++) c->fields.push(c->base->fields[i]);
    for (int i = 0; i < own.size(); i++) c->fields.push(own[i]);
    c->vtable = c->base->vtable;
    if (c->base->is_interface && c->base->fields.size() == 0) {
      // 1番目がインタフェースでも構わない（実装を持つ親が無いだけ）
    }
  }
  for (int i = 0; i < c->interfaces.size(); i++) {
    ClassInfo* ic = c->interfaces[i];
    layout_class(ic);
    if (!ic->is_interface && ic->fields.size() > 0) {
      err_at("E0405", diag_.L(Str("実装を持つ親は1つまでです（") + ic->name + " はメンバ変数を持っています）",
                              Str("only one base class with state is allowed: ") + ic->name),
             c->decl ? c->decl->line : 1, c->decl ? c->decl->col : 1, (int)c->name.size());
      continue;
    }
    for (int s = 0; s < ic->vtable.size(); s++) {
      if (c->vtable.size() <= s) c->vtable.resize(s + 1, -2);
      if (c->vtable[s] == -2) c->vtable[s] = ic->vtable[s];
    }
  }
  // 自分の virtual を表に入れる
  for (int i = 0; i < c->methods.size(); i++) {
    FuncInfo* f = prog_.funcs[c->methods[i].func];
    if (!f->is_virtual) continue;
    int s = f->vslot;
    if (c->vtable.size() <= s) c->vtable.resize(s + 1, -2);
    c->vtable[s] = f->is_pure ? -1 : f->index;
  }
  // 純粋仮想が残っていれば抽象クラス
  c->is_abstract = false;
  for (int s = 0; s < c->vtable.size(); s++) if (c->vtable[s] == -1) c->is_abstract = true;
  // メンバ変数を持たず、純粋仮想だけならインタフェース
  if (!c->is_builtin) {
    bool only_pure = c->fields.size() == 0;
    for (int i = 0; i < c->methods.size() && only_pure; i++)
      if (!prog_.funcs[c->methods[i].func]->is_pure) only_pure = false;
    c->is_interface = only_pure && c->methods.size() > 0;
  }
  c->layout_state = 2;
}

// 親（インタフェース含む）から、同じ名前・同じ引数のメソッドを探す
// 親の引数と子の引数が合うか。インタフェースの This は「実装したクラス自身」
static bool params_match(ClassInfo* impl, FuncInfo* parent, FuncInfo* child) {
  if (parent->params.size() != child->params.size()) return false;
  for (int i = 0; i < parent->params.size(); i++) {
    Type* pt = parent->params[i].type;
    Type* ct = child->params[i].type;
    if (pt && pt->kind == T_Generic && pt->name == "This") {
      if (!(ct && ct->kind == T_Class && class_is(impl, ct->cls))) return false;
      continue;
    }
    if (!type_same(pt, ct)) return false;
  }
  return true;
}

static FuncInfo* find_in_bases(Program& prog, ClassInfo* c, FuncInfo* f, bool* found_name) {
  for (ClassInfo* p = c ? c->base : 0; p; p = p->base) {
    for (int i = 0; i < p->methods.size(); i++) {
      FuncInfo* g = prog.funcs[p->methods[i].func];
      if (!(g->name == f->name)) continue;
      *found_name = true;
      if (params_match(c, g, f)) return g;
    }
  }
  if (c) {
    for (int i = 0; i < c->interfaces.size(); i++) {
      ClassInfo* ic = c->interfaces[i];
      for (int k = 0; k < ic->methods.size(); k++) {
        FuncInfo* g = prog.funcs[ic->methods[k].func];
        if (!(g->name == f->name)) continue;
        *found_name = true;
        if (params_match(c, g, f)) return g;
      }
      bool sub = false;
      FuncInfo* g = find_in_bases(prog, ic, f, &sub);
      if (sub) *found_name = true;
      if (g) return g;
    }
  }
  return 0;
}

// クラスが自分自身を値として持っていないか（代入がコピーなので大きさが決まらない）
static bool holds_by_value(ClassInfo* c, ClassInfo* target, Vec<ClassInfo*>* seen) {
  for (int i = 0; i < c->fields.size(); i++) {
    Type* ft = c->fields[i].type;
    if (!ft || ft->kind != T_Class || !ft->cls) continue;
    if (ft->cls == target) return true;
    bool visited = false;
    for (int k = 0; k < seen->size(); k++) if ((*seen)[k] == ft->cls) visited = true;
    if (visited) continue;
    seen->push(ft->cls);
    if (holds_by_value(ft->cls, target, seen)) return true;
  }
  return false;
}

void Checker::collect_funcs(Unit* u) {
  unit_ = u;
  diag_.set_file(u->display);
  for (int i = 0; i < u->funcs.size(); i++) {
    FuncDecl* fd = u->funcs[i];
    FuncInfo* f = new_func(prog_);
    fd->info = f;
    f->name = fd->name;
    f->module = u->module;
    f->file = u->display;
    f->file = u->display;
    f->is_public = fd->is_public;
    f->decl = fd;
    f->is_test = fd->name.size() > 5 && fd->name.sub(0, 5) == "test_";
    FuncCtx tmp;
    for (int g = 0; g < fd->gparams.size(); g++) {
      Type* gt = t_.generic(fd->gparams[g].name);
      for (int ci = 0; ci < fd->gparams[g].constraints.size(); ci++) {
        ClassInfo* ic = find_class(fd->gparams[g].constraints[ci], u);
        if (!ic) {
          err_at("E0106", diag_.L(Str("インタフェース ") + fd->gparams[g].constraints[ci] + " が見つかりません",
                                  Str("unknown interface: ") + fd->gparams[g].constraints[ci]),
                 fd->line, fd->col, (int)fd->name.size());
          continue;
        }
        gt->constraints.push(ic);
      }
      tmp.gnames.push(fd->gparams[g].name);
      tmp.gtypes.push(gt);
      f->gparams.push(fd->gparams[g].name);
      f->gtypes.push(gt);
    }
    f->is_generic = f->gparams.size() > 0;
    for (int p = 0; p < fd->params.size(); p++) {
      ParamInfo pi;
      pi.name = fd->params[p].name;
      pi.type = resolve_type(fd->params[p].type, u, &tmp, 0);
      pi.is_ref = fd->params[p].is_ref;
      f->params.push(pi);
    }
    f->ret = fd->ret ? resolve_type(fd->ret, u, &tmp, 0) : t_.t_void();
  }
}

void Checker::collect_globals(Unit* u) {
  unit_ = u;
  diag_.set_file(u->display);
  for (int i = 0; i < u->globals.size(); i++) {
    GlobalDecl* gd = u->globals[i];
    GlobalInfo* g = new (sk_alloc(sizeof(GlobalInfo))) GlobalInfo();
    g->name = gd->name;
    g->module = u->module;
    g->is_public = gd->is_public;
    g->is_const = gd->is_const;
    g->index = prog_.globals.size();
    g->type = gd->type ? resolve_type(gd->type, u, 0, 0) : 0;  // 初期値から決めるものは後で
    gd->index = g->index;
    prog_.globals.push(g);
  }
}

// 式を短く言い表す（直し方の例に使う）
static Str expr_text(Node* e) {
  if (!e) return Str();
  switch (e->kind) {
    case E_Ident: return e->name;
    case E_This: return Str("this");
    case E_Int: return str_from_int(e->ival);
    case E_Float: return str_from_float(e->dval);
    case E_Str: return Str("\"") + e->name + "\"";
    case E_Field: {
      Str b = expr_text(e->a);
      return b.size() ? b + "." + e->name : e->name;
    }
    case E_Call: {
      Str b = expr_text(e->a);
      return b.size() ? b + "(...)" : Str();
    }
    case E_Index: {
      Str b = expr_text(e->a);
      return b.size() ? b + "[...]" : Str();
    }
    default: return Str();
  }
}

// ------------------------------------------------------------------ 診断の道具
void Checker::err(const char* code, const Str& msg, Node* at, const Str& label) {
  Diagnostic& d = diag_.error(code, msg);
  if (at) d.spans.push(Span(at->line, at->col, at->len > 0 ? at->len : 1, label));
}

void Checker::err_at(const char* code, const Str& msg, int line, int col, int len) {
  Diagnostic& d = diag_.error(code, msg);
  d.spans.push(Span(line, col, len > 0 ? len : 1));
}

void Checker::warn(const char* code, const Str& msg, Node* at) {
  Diagnostic& d = diag_.warn(code, msg);
  if (at) d.spans.push(Span(at->line, at->col, at->len > 0 ? at->len : 1));
}

void Checker::need_bool(Node* cond, const char* what) {
  if (!cond || !cond->type) return;
  if (cond->type->kind == T_Bool || cond->type->kind == T_Unknown) return;
  Str tn = type_name(cond->type);
  Diagnostic& d = diag_.error("E0103", diag_.L(Str(what) + " の条件に書けるのは bool だけです（いまは " + tn + "）",
                                               Str("this ") + what + " condition must be bool (found " + tn + ")"));
  d.spans.push(Span(cond->line, cond->col, cond->len));
  if (cond->type->kind == T_Int)
    d.help.push(diag_.L("0 と比べます: 例 n != 0", "compare with a value, e.g. n != 0"));
  else if (cond->type->kind == T_String)
    d.help.push(diag_.L("長さと比べます: 例 s.len() > 0", "compare the length, e.g. s.len() > 0"));
  else if (is_optional(cond->type))
    d.help.push(diag_.L("値があるかを見るなら if var x = ... と書きます",
                        "to test for a value write: if var x = ..."));
  else
    d.help.push(diag_.L("比べた結果（== < > など）を書きます", "write a comparison"));
}

bool Checker::need_assign(Type* to, Type* from, Node* at, const char* what_ja, const char* what_en) {
  if (!to || !from) return true;
  if (type_assignable(to, from)) return true;
  // int と float を混ぜた
  if ((to->kind == T_Int && from->kind == T_Float) || (to->kind == T_Float && from->kind == T_Int)) {
    Diagnostic& d = diag_.error("E0102", diag_.L(Str("int と float は混ぜられません（") + what_ja + "）",
                                                 Str("int and float cannot be mixed (") + what_en + ")"));
    d.spans.push(Span(at->line, at->col, at->len, type_name(from)));
    d.help.push(to->kind == T_Float
                    ? diag_.L("float(...) で変換します", "convert with float(...)")
                    : diag_.L("int(...) で変換します", "convert with int(...)"));
    return false;
  }
  // T? をそのまま T として使った
  if (is_optional(from) && !is_optional(to)) {
    Diagnostic& d = diag_.error("E0201", diag_.L(type_name(from) + " をそのまま " + type_name(to) + " としては使えません",
                                                 type_name(from) + " cannot be used as " + type_name(to)));
    d.spans.push(Span(at->line, at->col, at->len, diag_.L("ここは値が無いかもしれません", "this may hold no value")));
    note_optional(d, expr_text(at));
    return false;
  }
  Diagnostic& d = diag_.error("E0101", diag_.L(Str(what_ja) + "の型が合いません: " + type_name(to) + " に " +
                                                   type_name(from) + " は入りません",
                                               Str(what_en) + " type mismatch: cannot put " + type_name(from) +
                                                   " into " + type_name(to)));
  d.spans.push(Span(at->line, at->col, at->len, type_name(from)));
  if (to->kind == T_String && from->kind != T_String)
    d.help.push(diag_.L("string(...) で文字列にします", "convert with string(...)"));
  else if (to->kind == T_Int && from->kind == T_String)
    d.help.push(diag_.L("int(...) で変換します（失敗すると none になります）",
                        "convert with int(...), which returns none on failure"));
  else
    d.help.push(diag_.L(Str("入れる値を ") + type_name(to) + " にするか、変数の型を " + type_name(from) + " にします",
                        Str("make the value a ") + type_name(to) + ", or declare the variable as " + type_name(from)));
  return false;
}

void Checker::note_optional(Diagnostic& d, const Str& text) {
  Str e = text.size() ? text : Str("値");
  d.help.push(diag_.L(Str("次のどれかを選びます\n") + e + " ?? 既定値" + "\n    値が無いときに使う値を決める\n" +
                          "if var v = " + e + " { }\n    値がある時だけ処理する\n" + e + "!" +
                          "\n    絶対にあると分かっているとき（無ければ止まる）",
                      Str("choose one:\n") + e + " ?? default\nif var v = " + e + " { }\n" + e + "!"));
}

// ------------------------------------------------------------------ 変数
void Checker::push_scope() { fc_->scopes.push(fc_->locals.size()); }

void Checker::pop_scope() {
  int start = fc_->scopes.back();
  fc_->scopes.pop();
  for (int i = fc_->locals.size() - 1; i >= start; i--) {
    Local& l = fc_->locals[i];
    if (!l.used && l.name.size() > 0 && l.name != "_" && l.name != "this") {
      Diagnostic& d = diag_.warn("W0001", diag_.L(Str("変数 ") + l.name + " を使っていません",
                                                  Str("unused variable: ") + l.name));
      d.spans.push(Span(l.line, l.col, l.len));
      d.help.push(diag_.L("書き間違いでなければ、消すか _ という名前にします",
                          "remove it, or name it _ if it is intentional"));
    }
  }
  while (fc_->locals.size() > start) {
    fc_->next_slot--;
    fc_->locals.pop();
  }
}

Local* Checker::find_local(const Str& name) {
  for (int i = fc_->locals.size() - 1; i >= 0; i--)
    if (fc_->locals[i].name == name) return &fc_->locals[i];
  return 0;
}

int Checker::declare_local(const Str& name, Type* t, bool is_const, Node* at) {
  for (int i = fc_->locals.size() - 1; i >= (fc_->scopes.size() ? fc_->scopes.back() : 0); i--) {
    if (fc_->locals[i].name == name) {
      err("E0110", diag_.L(Str("同じ名前の変数がもうあります: ") + name,
                           Str("a variable with this name already exists: ") + name), at);
      break;
    }
  }
  Local l;
  l.name = name;
  l.type = t;
  l.is_const = is_const;
  l.slot = fc_->next_slot++;
  if (fc_->next_slot > fc_->max_slot) fc_->max_slot = fc_->next_slot;
  if (at) { l.line = at->line; l.col = at->col; l.len = at->len; }
  fc_->locals.push(l);
  return l.slot;
}

GlobalInfo* Checker::find_global(const Str& name, Unit* u) {
  for (int i = 0; i < prog_.globals.size(); i++)
    if (prog_.globals[i]->name == name && prog_.globals[i]->module == u->module)
      return prog_.globals[i];
  return 0;
}

// ------------------------------------------------------------------ 文
static bool always_returns(Node* s) {
  if (!s) return false;
  switch (s->kind) {
    case S_Return: return true;
    case S_Panic: return true;
    case S_Block: {
      for (int i = 0; i < s->list.size(); i++) if (always_returns(s->list[i])) return true;
      return false;
    }
    case S_If: {
      if (!s->c) return false;
      return always_returns(s->b) && always_returns(s->c);
    }
    case S_While: {
      // while true { } は抜けないが、break があるかは見ない。安全側に倒す
      return false;
    }
    default: return false;
  }
}

void Checker::check_block(Node* b) {
  push_scope();
  bool warned = false;
  for (int i = 0; i < b->list.size(); i++) {
    check_stmt(b->list[i]);
    if (!warned && always_returns(b->list[i]) && i + 1 < b->list.size()) {
      warn("W0002", diag_.L("ここから先は実行されません", "this code is never reached"), b->list[i + 1]);
      warned = true;
    }
  }
  pop_scope();
}

void Checker::check_var_decl(Node* s) {
  Type* declared = s->tann ? resolve_type(s->tann, unit_, fc_, fc_->cls) : 0;
  Type* t = declared;
  if (s->a) {
    Type* it = check_expr(s->a);
    if (!declared) {
      // 初期値から決める。決まらないものはここで止める
      if (it && it->kind == T_Optional && it->a && it->a->kind == T_Unknown) {
        Diagnostic& d = diag_.error("E0104", diag_.L("none だけでは型が決まりません",
                                                     "none alone does not determine a type"));
        d.spans.push(Span(s->line, s->col, s->len));
        d.help.push(diag_.L(Str("型を書きます: var ") + s->name + ": int? = none;",
                            Str("write the type: var ") + s->name + ": int? = none;"));
        t = t_.t_unknown();
      } else if (it && it->kind == T_List && it->a && it->a->kind == T_Unknown) {
        Diagnostic& d = diag_.error("E0104", diag_.L("空の [] だけでは要素の型が決まりません",
                                                     "an empty [] does not determine the element type"));
        d.spans.push(Span(s->line, s->col, s->len));
        d.help.push(diag_.L(Str("型を書きます: var ") + s->name + ": list<int> = [];",
                            Str("write the type: var ") + s->name + ": list<int> = [];"));
        t = t_.t_unknown();
      } else if (it && it->kind == T_Map && it->a && it->a->kind == T_Unknown) {
        Diagnostic& d = diag_.error("E0104", diag_.L("空の {} だけではキーと値の型が決まりません",
                                                     "an empty {} does not determine the key/value types"));
        d.spans.push(Span(s->line, s->col, s->len));
        d.help.push(diag_.L(Str("型を書きます: var ") + s->name + ": map<string, int> = {};",
                            Str("write the type: var ") + s->name + ": map<string, int> = {};"));
        t = t_.t_unknown();
      } else if (it && it->kind == T_Void) {
        Diagnostic& d = diag_.error("E0111", diag_.L("値を返さない呼び出しは変数に入れられません",
                                                     "a call that returns void cannot be assigned"));
        d.spans.push(Span(s->line, s->col, s->len));
        d.help.push(diag_.L("戻り値のある関数を使うか、そのまま文として書きます",
                            "use a function that returns a value, or call it as a statement"));
        t = t_.t_unknown();
      } else {
        t = it;
      }
    } else {
      need_assign(declared, it, s->a, "変数", "variable");
    }
  } else if (!declared) {
    Diagnostic& d = diag_.error("E0112", diag_.L("初期値のない変数には型注釈が要ります",
                                                 "a variable without an initializer needs a type"));
    d.spans.push(Span(s->line, s->col, s->len));
    d.help.push(diag_.L(Str("例: var ") + s->name + ": int;   （既定値 0 で始まります）",
                        Str("example: var ") + s->name + ": int;")); 
    t = t_.t_unknown();
  }
  if (t && t->kind == T_Void) t = t_.t_unknown();
  s->type = t;
  s->slot = declare_local(s->name, t, s->is_const, s);
}

void Checker::check_assign(Node* s) {
  Node* target = s->a;
  // _ = f(); は「意図して捨てる」
  if (target->kind == E_Ident && target->name == "_") {
    s->opcode = 1;
    check_expr(s->b);
    return;
  }
  Type* tt = 0;
  if (target->kind == E_Ident) {
    Local* l = find_local(target->name);
    if (l) {
      if (l->is_const) {
        err("E0113", diag_.L(Str("const の ") + l->name + " は書き換えられません",
                             Str("cannot assign to const ") + l->name), target);
      }
      l->used = true;
      target->slot = l->slot;
      target->is_global = false;
      target->is_ref_param = l->is_ref;
      tt = l->type;
      target->type = tt;
    } else {
      GlobalInfo* g = find_global(target->name, unit_);
      if (g) {
        if (g->is_const)
          err("E0113", diag_.L(Str("const の ") + g->name + " は書き換えられません",
                               Str("cannot assign to const ") + g->name), target);
        target->slot = g->index;
        target->is_global = true;
        tt = g->type;
        target->type = tt;
      } else {
        err("E0114", diag_.L(Str("変数 ") + target->name + " が見つかりません",
                             Str("unknown variable: ") + target->name), target);
        check_expr(s->b);
        return;
      }
    }
  } else if (target->kind == E_Field || target->kind == E_Index) {
    tt = check_expr(target);
    if (target->kind == E_Field && target->optional_chain) {
      err("E0115", diag_.L("?. の結果には代入できません", "cannot assign through ?."), target);
    }
  } else {
    err("E0115", diag_.L("ここには代入できません", "this is not something you can assign to"), target);
    check_expr(s->b);
    return;
  }

  Type* vt = check_expr(s->b);
  if (s->name.size() > 0) {
    // 複合代入。演算できる組み合わせかを見る
    if (tt && vt) {
      bool ok = false;
      if (tt->kind == T_Int && vt->kind == T_Int) ok = true;
      else if (tt->kind == T_Float && vt->kind == T_Float) ok = true;
      else if (tt->kind == T_String && vt->kind == T_String && s->name == "+") ok = true;
      else if (tt->kind == T_Unknown || vt->kind == T_Unknown) ok = true;
      if (!ok) {
        if ((tt->kind == T_Int && vt->kind == T_Float) || (tt->kind == T_Float && vt->kind == T_Int)) {
          Diagnostic& d = diag_.error("E0102", diag_.L("int と float は混ぜられません",
                                                       "int and float cannot be mixed"));
          d.spans.push(Span(s->line, s->col, s->len));
          d.help.push(tt->kind == T_Float ? diag_.L("float(...) で変換します", "convert with float(...)")
                                          : diag_.L("int(...) で変換します", "convert with int(...)"));
        } else {
          err("E0116", diag_.L(Str(type_name(tt)) + " と " + type_name(vt) + " に " + s->name + "= は使えません",
                               Str("operator ") + s->name + "= cannot be used on " + type_name(tt) +
                                   " and " + type_name(vt)), s);
        }
      }
    }
  } else {
    need_assign(tt, vt, s->b, "代入", "assignment");
  }
  s->type = tt;
}

void Checker::check_if(Node* s) {
  if (s->bind.size() > 0) {
    Type* et = check_expr(s->a);
    Type* bt = 0;
    Type* et2 = 0;
    if (is_optional(et)) bt = et->a;
    else if (is_result(et)) { bt = et->a; et2 = t_.class_type(c_error_); }
    else if (et && et->kind != T_Unknown) {
      Diagnostic& d = diag_.error("E0203", diag_.L(Str("if var で取り出せるのは T? か Result<T> です（いまは ") +
                                                       type_name(et) + "）",
                                                   Str("if var needs a T? or Result<T>, found ") + type_name(et)));
      d.spans.push(Span(s->a->line, s->a->col, s->a->len));
      d.help.push(diag_.L("値が必ずあるなら if var は要りません", "if the value always exists, use a plain if"));
    }
    if (!bt) bt = t_.t_unknown();
    s->bind_type = bt;
    push_scope();
    s->slot = declare_local(s->bind, bt, false, s);
    fc_->locals.back().used = true;   // 値の有無を見るだけのこともある
    // ブロックの中では取り出した値が使える
    check_block(s->b);
    pop_scope();
    if (s->c) {
      if (s->bind2.size() > 0) {
        if (!et2) {
          err("E0204", diag_.L("else var で受け取れるのは Result の失敗だけです",
                               "else var only applies to a Result"), s->c);
          et2 = t_.class_type(c_error_);
        }
        push_scope();
        s->slot2 = declare_local(s->bind2, et2, false, s);
        fc_->locals.back().used = true;
        s->bind2_type = et2;
        check_stmt(s->c);
        pop_scope();
      } else {
        check_stmt(s->c);
      }
    }
    return;
  }
  Type* ct = check_expr(s->a);
  (void)ct;
  need_bool(s->a, "if");
  check_stmt(s->b);
  if (s->c) check_stmt(s->c);
}

void Checker::check_while(Node* s) {
  if (s->bind.size() > 0) {
    Type* et = check_expr(s->a);
    Type* bt = 0;
    if (is_optional(et)) bt = et->a;
    else if (is_result(et)) bt = et->a;
    else if (et && et->kind != T_Unknown) {
      Diagnostic& d = diag_.error("E0203", diag_.L(Str("while var で取り出せるのは T? か Result<T> です（いまは ") +
                                                       type_name(et) + "）",
                                                   Str("while var needs a T? or Result<T>, found ") + type_name(et)));
      d.spans.push(Span(s->a->line, s->a->col, s->a->len));
    }
    if (!bt) bt = t_.t_unknown();
    s->bind_type = bt;
    push_scope();
    s->slot = declare_local(s->bind, bt, false, s);
    fc_->locals.back().used = true;
    fc_->loop_depth++;
    check_block(s->b);
    fc_->loop_depth--;
    pop_scope();
    return;
  }
  check_expr(s->a);
  need_bool(s->a, "while");
  fc_->loop_depth++;
  check_stmt(s->b);
  fc_->loop_depth--;
}

void Checker::check_for(Node* s) {
  Type* it = check_expr(s->a);
  Type* et = t_.t_unknown();
  if (it) {
    if (it->kind == T_List) et = it->a;
    else if (it->kind == T_Range) et = t_.t_int();
    else if (it->kind == T_Map) {
      Diagnostic& d = diag_.error("E0117", diag_.L("map はそのままでは回せません",
                                                   "a map cannot be iterated directly"));
      d.spans.push(Span(s->a->line, s->a->col, s->a->len));
      d.help.push(diag_.L("キーを回します: for var k in m.keys() { }",
                          "iterate the keys: for var k in m.keys() { }"));
    } else if (it->kind == T_String) {
      Diagnostic& d = diag_.error("E0117", diag_.L("string はそのままでは回せません",
                                                   "a string cannot be iterated directly"));
      d.spans.push(Span(s->a->line, s->a->col, s->a->len));
      d.help.push(diag_.L("1文字ずつ回します: for var c in s.chars() { }",
                          "iterate characters: for var c in s.chars() { }"));
    } else if (it->kind != T_Unknown) {
      Diagnostic& d = diag_.error("E0117", diag_.L(type_name(it) + " は回せません",
                                                   type_name(it) + " cannot be iterated"));
      d.spans.push(Span(s->a->line, s->a->col, s->a->len));
      d.help.push(diag_.L("回せるのは list と range です", "only list and range can be iterated"));
    }
  }
  push_scope();
  s->bind_type = et;
  s->slot = declare_local(s->bind, et, false, s);
  fc_->locals.back().used = true;   // 回すだけで中身を使わないこともある
  fc_->loop_depth++;
  check_block(s->b);
  fc_->loop_depth--;
  pop_scope();
}

void Checker::check_return(Node* s) {
  Type* want = fc_->ret;
  if (!s->a) {
    if (want && want->kind != T_Void && want->kind != T_Unknown) {
      Diagnostic& d = diag_.error("E0118", diag_.L(Str("この関数は ") + type_name(want) + " を返します",
                                                   Str("this function must return ") + type_name(want)));
      d.spans.push(Span(s->line, s->col, s->len));
      d.help.push(diag_.L("return の後ろに値を書きます", "write a value after return"));
    }
    return;
  }
  Type* got = check_expr(s->a);
  if (!want || !got) return;
  if (want->kind == T_Void) {
    if (got->kind != T_Void) {
      Diagnostic& d = diag_.error("E0118", diag_.L("この関数は値を返しません（-> void）",
                                                   "this function returns void"));
      d.spans.push(Span(s->a->line, s->a->col, s->a->len));
      d.help.push(diag_.L("戻り値の型を書き足すか、return; と書きます",
                          "declare a return type, or write return;"));
    }
    return;
  }
  // Result<T> を返す関数では、値も Error もそのまま書ける
  if (is_result(want)) {
    if (type_assignable(want, got)) { s->opcode = 0; return; }
    if (got->kind == T_Class && class_is(got->cls, c_error_)) { s->opcode = 2; return; }  // 失敗として包む
    if (type_assignable(want->a, got)) { s->opcode = 1; return; }  // 成功として包む
  }
  need_assign(want, got, s->a, "戻り値", "return value");
}

void Checker::check_stmt(Node* s) {
  if (!s) return;
  switch (s->kind) {
    case S_Block: check_block(s); break;
    case S_VarDecl: check_var_decl(s); break;
    case S_Assign: check_assign(s); break;
    case S_If: check_if(s); break;
    case S_While: check_while(s); break;
    case S_For: check_for(s); break;
    case S_Return: check_return(s); break;
    case S_Break:
    case S_Continue:
      if (fc_->loop_depth == 0)
        err("E0119", diag_.L(Str(s->kind == S_Break ? "break" : "continue") + " は繰り返しの中だけで使えます",
                             Str(s->kind == S_Break ? "break" : "continue") + " may only be used inside a loop"), s);
      break;
    case S_Panic: {
      if (s->a) {
        Type* t = check_expr(s->a);
        if (t && t->kind != T_String && t->kind != T_Unknown)
          err("E0120", diag_.L("panic には理由の文字列を渡します", "panic takes a message string"), s->a);
      } else {
        err("E0120", diag_.L("panic には理由の文字列を渡します", "panic takes a message string"), s);
      }
      break;
    }
    case S_Expr: {
      Type* t = check_expr(s->a);
      if (t && is_result(t) && !(s->a->kind == E_Call && s->a->opcode == CK_None)) {
        Diagnostic& d = diag_.warn("E0202", diag_.L("失敗するかもしれない呼び出しの結果を捨てています",
                                                    "the Result of this call is discarded"));
        d.spans.push(Span(s->a->line, s->a->col, s->a->len, type_name(t)));
        d.help.push(diag_.L("次のどれかを選びます\n  try f()       失敗ならこの関数から返る\n"
                            "  if var v = f() { } else var e { }\n  _ = f();      意図して捨てる",
                            "choose one:\n  try f()\n  if var v = f() { } else var e { }\n  _ = f();"));
      }
      break;
    }
    default:
      check_expr(s);
      break;
  }
}

// ------------------------------------------------------------------ 式
Type* Checker::check_expr(Node* e) {
  if (!e) return t_.t_unknown();
  Type* t = t_.t_unknown();
  switch (e->kind) {
    case E_Int: t = t_.t_int(); break;
    case E_Float: t = t_.t_float(); break;
    case E_Bool: t = t_.t_bool(); break;
    case E_Str: t = t_.t_string(); break;
    case E_Bytes: t = t_.t_bytes(); break;
    case E_None: t = t_none_; break;
    case E_ListLit: t = check_list_lit(e); break;
    case E_MapLit: t = check_map_lit(e); break;
    case E_Ident: t = check_ident(e); break;
    case E_This: {
      if (!fc_->cls) {
        err("E0122", diag_.L("this はクラスのメソッドの中だけで使えます",
                             "this may only be used inside a method"), e);
        t = t_.t_unknown();
      } else {
        t = t_.class_type(fc_->cls);
        e->slot = 0;
      }
      break;
    }
    case E_Super: {
      if (!fc_->cls || !fc_->cls->base) {
        err("E0123", diag_.L("super は親クラスを持つクラスの中だけで使えます",
                             "super may only be used in a class with a base class"), e);
        t = t_.t_unknown();
      } else {
        t = t_.class_type(fc_->cls->base);
        e->slot = 0;
      }
      break;
    }
    case E_Field: t = check_field(e); break;
    case E_Index: t = check_index(e); break;
    case E_Call: t = check_call(e); break;
    case E_Unary: t = check_unary(e); break;
    case E_Binary: t = check_binary(e); break;
    case E_FStr: t = check_fstr(e); break;
    case E_Force: {
      Type* it = check_expr(e->a);
      if (is_optional(it)) t = it->a;
      else if (is_result(it)) t = it->a;
      else if (it && it->kind != T_Unknown) {
        Diagnostic& d = diag_.error("E0206", diag_.L(Str("! を付けられるのは T? か Result<T> です（いまは ") +
                                                         type_name(it) + "）",
                                                     Str("! applies to T? or Result<T>, found ") + type_name(it)));
        d.spans.push(Span(e->line, e->col, 1));
        d.help.push(diag_.L("値が必ずあるなら ! は要りません", "the value always exists here; drop the !"));
        t = it;
      }
      break;
    }
    case E_Try: t = check_try(e); break;
    case E_Task: t = check_task(e); break;
    case E_Parallel: t = check_parallel(e); break;
    case E_Ref: {
      Type* it = check_expr(e->a);
      if (!(e->a->kind == E_Ident || e->a->kind == E_Field || e->a->kind == E_Index)) {
        Diagnostic& d = diag_.error("E0303", diag_.L("ref に書けるのは変数だけです",
                                                     "ref may only be applied to a variable"));
        d.spans.push(Span(e->line, e->col, e->len));
        d.help.push(diag_.L("計算した結果を ref で渡すことはできません",
                            "a computed value cannot be passed by ref"));
      }
      t = it;
      break;
    }
    case S_Panic: {
      if (e->a) check_expr(e->a);
      t = t_.t_void();
      break;
    }
    default: t = t_.t_unknown(); break;
  }
  e->type = t;
  return t;
}

Type* Checker::check_list_lit(Node* e) {
  if (e->list.size() == 0) return t_.list_of(t_.t_unknown());
  Type* et = check_expr(e->list[0]);
  if (et && et->kind == T_Optional && et->a && et->a->kind == T_Unknown) {
    // [none, none] のような形は型が決まらない
    et = t_.t_unknown();
  }
  for (int i = 1; i < e->list.size(); i++) {
    Type* it = check_expr(e->list[i]);
    if (!type_assignable(et, it)) {
      if (is_optional(it) && type_assignable(it, et)) { et = it; continue; }
      Diagnostic& d = diag_.error("E0121", diag_.L("配列の要素は全部同じ型でなければなりません",
                                                   "all list elements must have the same type"));
      d.spans.push(Span(e->list[0]->line, e->list[0]->col, e->list[0]->len, type_name(et)));
      d.spans.push(Span(e->list[i]->line, e->list[i]->col, e->list[i]->len, type_name(it)));
      d.help.push(diag_.L("型を揃えるか、共通の親クラスの型にします",
                          "make the types match, or use a common base class"));
      break;
    }
  }
  return t_.list_of(et);
}

Type* Checker::check_map_lit(Node* e) {
  if (e->pairs.size() == 0) return t_.map_of(t_.t_unknown(), t_.t_unknown());
  Type* kt = check_expr(e->pairs[0].key);
  Type* vt = check_expr(e->pairs[0].val);
  for (int i = 1; i < e->pairs.size(); i++) {
    Type* k = check_expr(e->pairs[i].key);
    Type* v = check_expr(e->pairs[i].val);
    if (!type_assignable(kt, k)) {
      Diagnostic& d = diag_.error("E0121", diag_.L("連想配列のキーは全部同じ型でなければなりません",
                                                   "all map keys must have the same type"));
      d.spans.push(Span(e->pairs[i].key->line, e->pairs[i].key->col, e->pairs[i].key->len, type_name(k)));
      break;
    }
    if (!type_assignable(vt, v)) {
      Diagnostic& d = diag_.error("E0121", diag_.L("連想配列の値は全部同じ型でなければなりません",
                                                   "all map values must have the same type"));
      d.spans.push(Span(e->pairs[i].val->line, e->pairs[i].val->col, e->pairs[i].val->len, type_name(v)));
      break;
    }
  }
  switch (kt ? kt->kind : T_Unknown) {
    case T_Int: case T_Float: case T_Bool: case T_String: case T_Bytes: case T_Unknown: break;
    default: {
      Diagnostic& d = diag_.error("E0124", diag_.L(Str("キーには ") + type_name(kt) + " は使えません",
                                                   Str("a map key cannot be ") + type_name(kt)));
      d.spans.push(Span(e->pairs[0].key->line, e->pairs[0].key->col, e->pairs[0].key->len));
      d.help.push(diag_.L("キーに使えるのは int float bool string bytes です",
                          "map keys may be int, float, bool, string or bytes"));
      break;
    }
  }
  return t_.map_of(kt, vt);
}

Type* Checker::check_ident(Node* e) {
  Local* l = find_local(e->name);
  if (l) {
    l->used = true;
    e->slot = l->slot;
    e->is_global = false;
    e->is_ref_param = l->is_ref;
    return l->type;
  }
  GlobalInfo* g = find_global(e->name, unit_);
  if (g) {
    e->slot = g->index;
    e->is_global = true;
    return g->type ? g->type : t_.t_unknown();
  }
  // 関数を値として使う
  for (int i = 0; i < prog_.funcs.size(); i++) {
    FuncInfo* f = prog_.funcs[i];
    if (f->owner || !(f->name == e->name)) continue;
    if (!(f->module == unit_->module)) continue;
    Vec<Type*> ps;
    for (int k = 0; k < f->params.size(); k++) ps.push(f->params[k].type);
    e->resolved = f->index;
    e->opcode = CK_Func;
    return t_.func_type(ps, f->ret);
  }
  // モジュール名
  bool found = false;
  module_of_alias(e->name, unit_, &found);
  if (found) {
    Diagnostic& d = diag_.error("E0125", diag_.L(Str("モジュール ") + e->name + " はそのままでは値になりません",
                                                 Str("module ") + e->name + " is not a value"));
    d.spans.push(Span(e->line, e->col, e->len));
    d.help.push(diag_.L(Str(e->name) + ".関数名(...) のように、中のものを呼びます",
                        Str("call something inside it: ") + e->name + ".name(...)"));
    return t_.t_unknown();
  }
  Diagnostic& d = diag_.error("E0114", diag_.L(Str("変数 ") + e->name + " が見つかりません",
                                               Str("unknown name: ") + e->name));
  d.spans.push(Span(e->line, e->col, e->len));
  // 近い名前を候補として示す
  Str best;
  int best_score = 3;
  for (int i = fc_->locals.size() - 1; i >= 0; i--) {
    const Str& n = fc_->locals[i].name;
    int diff = n.size() > e->name.size() ? n.size() - e->name.size() : e->name.size() - n.size();
    if (diff > 2) continue;
    int same = 0;
    int m = n.size() < e->name.size() ? n.size() : e->name.size();
    for (int k = 0; k < m; k++) if (n[k] == e->name[k]) same++;
    int score = (m - same) + diff;
    if (score < best_score) { best_score = score; best = n; }
  }
  if (best.size()) d.help.push(diag_.L(Str("もしかして ") + best + " のことですか",
                                       Str("did you mean ") + best + "?"));
  else d.help.push(diag_.L("var で宣言してから使います", "declare it first with var"));
  return t_.t_unknown();
}

Type* Checker::check_unary(Node* e) {
  Type* it = check_expr(e->a);
  if (e->name == "!") {
    if (it && it->kind != T_Bool && it->kind != T_Unknown) {
      Diagnostic& d = diag_.error("E0126", diag_.L(Str("! は bool に付けます（いまは ") + type_name(it) + "）",
                                                   Str("! applies to bool, found ") + type_name(it)));
      d.spans.push(Span(e->line, e->col, e->len));
      if (is_optional(it))
        d.help.push(diag_.L("値の有無を見るなら if var、値を取り出すなら 後ろに ! を書きます",
                            "use if var to test, or a trailing ! to force the value"));
    }
    e->opcode = OP_NOT;
    return t_.t_bool();
  }
  // -x
  if (it && it->kind == T_Int) { e->opcode = OP_NEG_INT; return it; }
  if (it && it->kind == T_Float) { e->opcode = OP_NEG_FLOAT; return it; }
  if (it && it->kind != T_Unknown) {
    Diagnostic& d = diag_.error("E0126", diag_.L(Str("- は数に付けます（いまは ") + type_name(it) + "）",
                                                 Str("- applies to numbers, found ") + type_name(it)));
    d.spans.push(Span(e->line, e->col, e->len));
  }
  e->opcode = OP_NEG_INT;
  return it ? it : t_.t_unknown();
}

Type* Checker::check_try(Node* e) {
  Type* it = check_expr(e->a);
  if (!is_result(fc_->ret)) {
    Diagnostic& d = diag_.error("E0207", diag_.L("try は Result を返す関数の中だけで書けます",
                                                 "try may only be used in a function returning Result"));
    d.spans.push(Span(e->line, e->col, e->len));
    d.help.push(diag_.L("この関数の戻り値を Result<T> にするか、if var で受け取ります",
                        "make this function return Result<T>, or receive it with if var"));
  }
  if (is_result(it)) return it->a;
  if (it && it->kind != T_Unknown) {
    Diagnostic& d = diag_.error("E0207", diag_.L(Str("try を書けるのは Result<T> にだけです（いまは ") +
                                                     type_name(it) + "）",
                                                 Str("try applies to Result<T>, found ") + type_name(it)));
    d.spans.push(Span(e->a->line, e->a->col, e->a->len));
  }
  return it;
}

Type* Checker::check_task(Node* e) {
  if (!e->a || e->a->kind != E_Call) {
    Diagnostic& d = diag_.error("E0304", diag_.L("task の後ろには関数の呼び出しを書きます",
                                                 "task must be followed by a function call"));
    d.spans.push(Span(e->line, e->col, e->len));
    d.help.push(diag_.L("例: var t = task fetch(url);", "example: var t = task fetch(url);"));
    if (e->a) check_expr(e->a);
    return t_.task_of(t_.t_unknown());
  }
  Type* rt = check_expr(e->a);
  // ref はタスクの境界を越えられない
  Node* call = e->a;
  for (int i = 0; i < call->list.size(); i++) {
    if (call->list[i]->kind == E_Ref) {
      Diagnostic& d = diag_.error("E0302", diag_.L("ref はタスクに渡せません",
                                                   "ref cannot be passed to a task"));
      d.spans.push(Span(call->list[i]->line, call->list[i]->col, call->list[i]->len));
      d.help.push(diag_.L("ref を外して値のまま渡します。コピーが渡るので、競合は起きません",
                          "pass the value itself; it is copied, so there is no data race"));
    }
  }
  return t_.task_of(rt ? rt : t_.t_unknown());
}

Type* Checker::check_parallel(Node* e) {
  Type* et = 0;
  for (int i = 0; i < e->list.size(); i++) {
    Node* item = e->list[i];
    if (item->kind != E_Task) {
      Diagnostic& d = diag_.error("E0305", diag_.L("parallel の中に書けるのは task ... だけです",
                                                   "only task calls may appear inside parallel"));
      d.spans.push(Span(item->line, item->col, item->len));
      check_expr(item);
      continue;
    }
    Type* tt = check_expr(item);
    Type* inner = (tt && tt->kind == T_Task) ? tt->a : t_.t_unknown();
    if (!et) et = inner;
    else if (!type_same(et, inner)) {
      Diagnostic& d = diag_.error("E0306", diag_.L("parallel の中のタスクは、全部同じ型を返さなければなりません",
                                                   "all tasks in a parallel block must return the same type"));
      d.spans.push(Span(item->line, item->col, item->len, type_name(inner)));
      d.help.push(diag_.L(Str("最初のタスクは ") + type_name(et) + " を返しています",
                          Str("the first task returns ") + type_name(et)));
    }
  }
  if (!et) et = t_.t_unknown();
  return t_.list_of(et);
}

Type* Checker::check_fstr(Node* e) {
  for (int i = 0; i < e->parts.size(); i++) {
    if (!e->parts[i].is_expr) continue;
    Type* pt = check_expr(e->parts[i].expr);
    // クラスは to_string() を使う
    if (pt && pt->kind == T_Class) {
      Node* call = arena_.make<Node>();
      call->kind = E_Call;
      call->line = e->parts[i].expr->line;
      call->col = e->parts[i].expr->col;
      call->len = e->parts[i].expr->len;
      Node* fld = arena_.make<Node>();
      fld->kind = E_Field;
      fld->name = Str("to_string");
      fld->line = call->line; fld->col = call->col; fld->len = call->len;
      fld->a = e->parts[i].expr;
      call->a = fld;
      Type* rt = check_expr(call);
      if (rt && rt->kind == T_String) {
        e->parts[i].expr = call;
      } else {
        Diagnostic& d = diag_.error("E0127", diag_.L(type_name(pt) + " を文字列にするには to_string() が要ります",
                                                     Str("to_string() is required to format ") + type_name(pt)));
        d.spans.push(Span(call->line, call->col, call->len));
        d.help.push(diag_.L("クラスに func to_string() -> string { } を足します",
                            "add func to_string() -> string { } to the class"));
      }
    } else if (pt && is_optional(pt)) {
      Diagnostic& d = diag_.error("E0201", diag_.L(type_name(pt) + " はそのままでは文字列にできません",
                                                   type_name(pt) + " cannot be formatted directly"));
      d.spans.push(Span(e->parts[i].expr->line, e->parts[i].expr->col, e->parts[i].expr->len));
      note_optional(d, expr_text(e->parts[i].expr));
    }
  }
  return t_.t_string();
}

Type* Checker::check_binary(Node* e) {
  const Str& op = e->name;
  // && と || は左辺で決まれば右辺を見ない
  if (op == "&&" || op == "||") {
    check_expr(e->a);
    check_expr(e->b);
    need_bool(e->a, op == "&&" ? "&&" : "||");
    need_bool(e->b, op == "&&" ? "&&" : "||");
    return t_.t_bool();
  }
  if (op == "??") {
    Type* lt = check_expr(e->a);
    Type* rt = check_expr(e->b);
    Type* inner = 0;
    if (is_optional(lt)) inner = lt->a;
    else if (is_result(lt)) inner = lt->a;
    else {
      if (lt && lt->kind != T_Unknown) {
        Diagnostic& d = diag_.error("E0208", diag_.L(Str("?? を書けるのは T? か Result<T> にだけです（いまは ") +
                                                         type_name(lt) + "）",
                                                     Str("?? applies to T? or Result<T>, found ") + type_name(lt)));
        d.spans.push(Span(e->a->line, e->a->col, e->a->len));
        d.help.push(diag_.L("左側が必ず値を持つなら ?? は要りません", "the left side always has a value"));
      }
      return lt;
    }
    if (inner && inner->kind == T_Unknown) return rt;
    if (!type_assignable(inner, rt)) {
      // 既定値も T? のときは結果も T?
      if (is_optional(rt) && type_assignable(rt, inner)) return rt;
      need_assign(inner, rt, e->b, "?? の既定値", "?? default value");
    }
    return inner;
  }

  Type* lt = check_expr(e->a);
  Type* rt = check_expr(e->b);
  if (!lt || !rt) return t_.t_unknown();
  bool unknown = lt->kind == T_Unknown || rt->kind == T_Unknown;

  if (op == "==" || op == "!=") {
    e->opcode = (op == "==") ? OP_EQ : OP_NE;
    bool lnone = (lt == t_none_), rnone = (rt == t_none_);
    if (lnone || rnone) {
      Type* other = lnone ? rt : lt;
      if (!is_optional(other) && other->kind != T_Unknown) {
        Diagnostic& d = diag_.error("E0209", diag_.L(type_name(other) + " は none にはなりません",
                                                     type_name(other) + " can never be none"));
        d.spans.push(Span(e->line, e->col, e->len));
        d.help.push(diag_.L("値が無いこともある型は int? のように ? を付けて宣言します",
                            "declare the type with ? if it may hold no value"));
      }
      return t_.t_bool();
    }
    if (!unknown && !type_assignable(lt, rt) && !type_assignable(rt, lt)) {
      Diagnostic& d = diag_.error("E0128", diag_.L(type_name(lt) + " と " + type_name(rt) + " は比べられません",
                                                   Str("cannot compare ") + type_name(lt) + " with " + type_name(rt)));
      d.spans.push(Span(e->line, e->col, e->len));
      if ((lt->kind == T_Int && rt->kind == T_Float) || (lt->kind == T_Float && rt->kind == T_Int))
        d.help.push(diag_.L("float(...) か int(...) で型を揃えます", "convert one side with float(...) or int(...)"));
    }
    return t_.t_bool();
  }

  if (op == "<" || op == "<=" || op == ">" || op == ">=") {
    e->opcode = op == "<" ? OP_LT : op == "<=" ? OP_LE : op == ">" ? OP_GT : OP_GE;
    if (unknown) return t_.t_bool();
    bool ok = type_same(lt, rt);
    if (ok) {
      switch (lt->kind) {
        case T_Int: case T_Float: case T_String: case T_Bytes: case T_Time: case T_Duration: break;
        default: ok = false; break;
      }
    }
    if (!ok) {
      if ((lt->kind == T_Int && rt->kind == T_Float) || (lt->kind == T_Float && rt->kind == T_Int)) {
        Diagnostic& d = diag_.error("E0102", diag_.L("int と float は比べられません",
                                                     "int and float cannot be compared directly"));
        d.spans.push(Span(e->line, e->col, e->len));
        d.help.push(diag_.L("float(...) で型を揃えます", "convert with float(...)"));
      } else {
        Diagnostic& d = diag_.error("E0128", diag_.L(type_name(lt) + " と " + type_name(rt) + " は大小を比べられません",
                                                     Str("cannot order ") + type_name(lt) + " and " + type_name(rt)));
        d.spans.push(Span(e->line, e->col, e->len));
        if (lt->kind == T_Class)
          d.help.push(diag_.L("クラスを並べるには Comparable を実装して compare() を使います",
                              "implement Comparable and use compare()"));
      }
    }
    return t_.t_bool();
  }

  // 算術
  if (unknown) return lt->kind == T_Unknown ? rt : lt;
  if (lt->kind == T_Int && rt->kind == T_Int) {
    e->opcode = op == "+" ? OP_ADD_INT : op == "-" ? OP_SUB_INT : op == "*" ? OP_MUL_INT
                : op == "/" ? OP_DIV_INT : OP_MOD_INT;
    return t_.t_int();
  }
  if (lt->kind == T_Float && rt->kind == T_Float) {
    if (op == "%") {
      Diagnostic& d = diag_.error("E0129", diag_.L("% は int にだけ使えます", "% applies to int only"));
      d.spans.push(Span(e->line, e->col, e->len));
      d.help.push(diag_.L("小数の余りが要るときは math で計算します", "use math for floating point remainder"));
      return t_.t_float();
    }
    e->opcode = op == "+" ? OP_ADD_FLOAT : op == "-" ? OP_SUB_FLOAT : op == "*" ? OP_MUL_FLOAT : OP_DIV_FLOAT;
    return t_.t_float();
  }
  if (lt->kind == T_String && rt->kind == T_String && op == "+") {
    e->opcode = OP_CONCAT;
    return t_.t_string();
  }
  if (lt->kind == T_Time && rt->kind == T_Duration && (op == "+" || op == "-")) {
    e->opcode = op == "+" ? OP_ADD_TIME_DUR : OP_SUB_TIME_DUR;
    return t_.simple(T_Time);
  }
  if (lt->kind == T_Time && rt->kind == T_Time && op == "-") {
    e->opcode = OP_SUB_TIME_TIME;
    return t_.simple(T_Duration);
  }
  if (lt->kind == T_Duration && rt->kind == T_Duration && (op == "+" || op == "-")) {
    e->opcode = op == "+" ? OP_ADD_DUR_DUR : OP_SUB_DUR_DUR;
    return t_.simple(T_Duration);
  }
  // 誤り
  if ((lt->kind == T_Int && rt->kind == T_Float) || (lt->kind == T_Float && rt->kind == T_Int)) {
    Str verb = op == "+" ? Str("足せません") : op == "-" ? Str("引けません")
               : op == "*" ? Str("掛けられません") : op == "/" ? Str("割れません")
                                                              : Str("計算できません");
    Str verb2 = op == "+" ? Str("足せます") : op == "-" ? Str("引けます")
                : op == "*" ? Str("掛けられます") : op == "/" ? Str("割れます") : Str("計算できます");
    Diagnostic& d = diag_.error("E0102", diag_.L(Str("int と float は") + verb,
                                                 Str("int and float cannot be combined with ") + op));
    d.spans.push(Span(e->a->line, e->a->col, e->a->len, type_name(lt)));
    d.spans.push(Span(e->b->line, e->b->col, e->b->len, type_name(rt)));
    Str l = expr_text(e->a), r = expr_text(e->b);
    if (l.size() && r.size()) {
      Str fix = lt->kind == T_Int ? (Str("float(") + l + ") " + op + " " + r)
                                  : (l + " " + op + " float(" + r + ")");
      d.help.push(diag_.L(fix + " と書くと" + verb2, Str("write ") + fix));
    } else if (lt->kind == T_Int) {
      d.help.push(diag_.L("左側を float(...) で変換します", "convert the left side with float(...)"));
    } else {
      d.help.push(diag_.L("右側を float(...) で変換します", "convert the right side with float(...)"));
    }
    return lt;
  }
  if (is_optional(lt) || is_optional(rt)) {
    Type* o = is_optional(lt) ? lt : rt;
    Node* on = is_optional(lt) ? e->a : e->b;
    Diagnostic& d = diag_.error("E0201", diag_.L(type_name(o) + " には " + op + " は使えません",
                                                 Str("operator ") + op + " cannot be used on " + type_name(o)));
    d.spans.push(Span(on->line, on->col, on->len, diag_.L("ここは値が無いかもしれません", "this may hold no value")));
    note_optional(d, expr_text(is_optional(lt) ? e->a : e->b));
    return is_optional(lt) ? lt->a : rt->a;
  }
  if (lt->kind == T_String && op == "+") {
    Diagnostic& d = diag_.error("E0130", diag_.L(Str("string と ") + type_name(rt) + " はつなげられません",
                                                 Str("cannot concatenate string and ") + type_name(rt)));
    d.spans.push(Span(e->line, e->col, e->len));
    d.help.push(diag_.L("string(...) で文字列にするか、f\"{...}\" を使います",
                        "convert with string(...), or use an f-string"));
    return t_.t_string();
  }
  Diagnostic& d = diag_.error("E0129", diag_.L(type_name(lt) + " と " + type_name(rt) + " に " + op + " は使えません",
                                               Str("operator ") + op + " cannot be used on " + type_name(lt) +
                                                   " and " + type_name(rt)));
  d.spans.push(Span(e->line, e->col, e->len));
  return lt;
}

Type* Checker::check_index(Node* e) {
  Type* ct = check_expr(e->a);
  Type* it = check_expr(e->b);
  if (!ct) return t_.t_unknown();
  switch (ct->kind) {
    case T_List:
      if (it && it->kind != T_Int && it->kind != T_Unknown) {
        Diagnostic& d = diag_.error("E0131", diag_.L(Str("配列の添字は int です（いまは ") + type_name(it) + "）",
                                                     Str("a list index must be int, found ") + type_name(it)));
        d.spans.push(Span(e->b->line, e->b->col, e->b->len));
      }
      return ct->a;
    case T_Map:
      if (!type_assignable(ct->a, it)) {
        Diagnostic& d = diag_.error("E0131", diag_.L(Str("キーの型が違います: ") + type_name(ct->a) + " が要ります",
                                                     Str("wrong key type: expected ") + type_name(ct->a)));
        d.spans.push(Span(e->b->line, e->b->col, e->b->len, type_name(it)));
      }
      return ct->b;
    case T_String:
      return t_.t_string();
    case T_Bytes:
      return t_.t_int();
    case T_Json:
      return t_.simple(T_Json);
    case T_Unknown:
      return t_.t_unknown();
    default: {
      Diagnostic& d = diag_.error("E0131", diag_.L(type_name(ct) + " に添字は使えません",
                                                   Str("cannot index ") + type_name(ct)));
      d.spans.push(Span(e->a->line, e->a->col, e->a->len));
      if (is_optional(ct)) note_optional(d, Str());
      return t_.t_unknown();
    }
  }
}

// ------------------------------------------------------------------ メンバ参照
static Str short_module_name(const Str& path) {
  int last = -1;
  for (int i = 0; i < path.size(); i++) if (path[i] == '.' || path[i] == '/') last = i;
  return path.sub(last + 1, path.size() - last - 1);
}

Type* Checker::check_field(Node* e) {
  Node* obj = e->a;
  // モジュールの中のもの
  if (obj->kind == E_Ident && !find_local(obj->name) && !find_global(obj->name, unit_)) {
    bool found = false;
    Str mod = module_of_alias(obj->name, unit_, &found);
    if (found) {
      e->name.size();
      Str full = short_module_name(mod) + "." + e->name;
      Vec<int> ids;
      reg_.find_all(full, &ids);
      if (ids.size() > 0) {
        // 定数（引数の無いもの）はその場で呼ぶ
        const NativeEntry& ne = reg_[ids[0]];
        if (ne.params.size() == 0) {
          e->opcode = CK_Native;
          e->resolved = ids[0];
          return ne.ret;
        }
        Diagnostic& d = diag_.error("E0133", diag_.L(full + " は関数です。呼び出して使います",
                                                     full + " is a function; call it"));
        d.spans.push(Span(e->line, e->col, e->len));
        d.help.push(diag_.L(full + "(...) と書きます", full + "(...)"));
        return t_.t_unknown();
      }
      // ユーザーのモジュール
      for (int i = 0; i < prog_.globals.size(); i++) {
        GlobalInfo* g = prog_.globals[i];
        if (g->module == mod && g->name == e->name) {
          if (!g->is_public) {
            Diagnostic& d = diag_.error("E0134", diag_.L(e->name + " は " + mod + " の外からは使えません",
                                                         e->name + " is not public in " + mod));
            d.spans.push(Span(e->line, e->col, e->len));
            d.help.push(diag_.L("使わせたいものには public を付けます", "mark it public to expose it"));
          }
          e->slot = g->index;
          e->is_global = true;
          return g->type ? g->type : t_.t_unknown();
        }
      }
      for (int i = 0; i < prog_.funcs.size(); i++) {
        FuncInfo* f = prog_.funcs[i];
        if (f->owner || !(f->module == mod) || !(f->name == e->name)) continue;
        Vec<Type*> ps;
        for (int k = 0; k < f->params.size(); k++) ps.push(f->params[k].type);
        e->resolved = f->index;
        e->opcode = CK_Func;
        return t_.func_type(ps, f->ret);
      }
      Diagnostic& d = diag_.error("E0135", diag_.L(mod + " に " + e->name + " はありません",
                                                   mod + " has no member named " + e->name));
      d.spans.push(Span(e->line, e->col, e->len));
      d.help.push(diag_.L("綴りを確かめます。public を付け忘れていることもあります",
                          "check the spelling; it may also be missing public"));
      return t_.t_unknown();
    }
  }

  Type* rt = check_expr(obj);
  if (!rt) return t_.t_unknown();
  if (e->optional_chain) {
    if (!is_optional(rt)) {
      Diagnostic& d = diag_.error("E0210", diag_.L(Str("?. を書けるのは T? にだけです（いまは ") + type_name(rt) + "）",
                                                   Str("?. applies to T?, found ") + type_name(rt)));
      d.spans.push(Span(e->line, e->col, e->len));
      d.help.push(diag_.L("値が必ずあるなら . を使います", "use . when the value always exists"));
    } else {
      rt = rt->a;
    }
  }
  if (is_optional(rt)) {
    Diagnostic& d = diag_.error("E0201", diag_.L(type_name(rt) + " のメンバはそのままでは触れません",
                                                 Str("cannot access a member of ") + type_name(rt)));
    d.spans.push(Span(obj->line, obj->col, obj->len, diag_.L("ここは値が無いかもしれません", "may hold no value")));
    d.help.push(diag_.L("?. を使うか、if var で取り出してから触ります",
                        "use ?., or unwrap it with if var"));
    return t_.t_unknown();
  }
  if (rt->kind == T_Class && rt->cls) {
    ClassInfo* c = rt->cls;
    for (int i = 0; i < c->fields.size(); i++) {
      if (!(c->fields[i].name == e->name)) continue;
      bool ok = c->fields[i].is_public;
      if (!ok && fc_->cls && class_is(fc_->cls, c->fields[i].owner)) ok = true;
      if (!ok) {
        Diagnostic& d = diag_.error("E0136", diag_.L(e->name + " は " + c->name + " の中からしか触れません",
                                                     e->name + " is private to " + c->name));
        d.spans.push(Span(e->line, e->col, e->len));
        d.help.push(diag_.L("外から使わせたいときは public を付けるか、値を返すメソッドを足します",
                            "add public, or expose it through a method"));
      }
      e->resolved = i;
      Type* ft = c->fields[i].type;
      if (rt->targs.size() && c->gparams.size()) ft = subst(ft, c->gparams, rt->targs);
      // a?.b の結果は必ず T? になる（spec/syntax.md）
      if (e->optional_chain) ft = t_.optional_of(ft);
      return ft;
    }
    Diagnostic& d = diag_.error("E0137", diag_.L(c->name + " に " + e->name + " というメンバはありません",
                                                 c->name + " has no member named " + e->name));
    d.spans.push(Span(e->line, e->col, e->len));
    Str near;
    for (int i = 0; i < c->fields.size(); i++) {
      if (near.size()) near += ", ";
      near += c->fields[i].name;
    }
    for (int i = 0; i < c->methods.size(); i++) {
      if (near.size()) near += ", ";
      near += c->methods[i].name;
      near += "()";
    }
    if (near.size()) d.help.push(diag_.L(Str("あるのは: ") + near, Str("available: ") + near));
    return t_.t_unknown();
  }
  if (rt->kind == T_Unknown) return t_.t_unknown();
  Diagnostic& d = diag_.error("E0137", diag_.L(type_name(rt) + " に " + e->name + " というメンバはありません",
                                               type_name(rt) + " has no member named " + e->name));
  d.spans.push(Span(e->line, e->col, e->len));
  d.help.push(diag_.L("メソッドなら () を付けて呼びます", "if it is a method, call it with ()"));
  return t_.t_unknown();
}

// ------------------------------------------------------------------ 呼び出し
static void collect_sig(Str& out, const Str& name, const Vec<Type*>& ps) {
  out += name;
  out += "(";
  for (int i = 0; i < ps.size(); i++) { if (i) out += ", "; out += type_name(ps[i]); }
  out += ")";
}

// 型引数を引数から決める
static bool unify(Type* pt, Type* at, const Vec<Str>& names, Vec<Type*>& out) {
  if (!pt || !at) return true;
  if (pt->kind == T_Generic) {
    for (int i = 0; i < names.size(); i++) {
      if (!(names[i] == pt->name)) continue;
      if (!out[i]) { out[i] = at; return true; }
      return type_assignable(out[i], at) || type_assignable(at, out[i]);
    }
    return true;
  }
  if (at->kind == T_Unknown) return true;
  switch (pt->kind) {
    case T_List: return at->kind == T_List && unify(pt->a, at->a, names, out);
    case T_Optional:
      if (at->kind == T_Optional) return unify(pt->a, at->a, names, out);
      return unify(pt->a, at, names, out);
    case T_Result: return at->kind == T_Result && unify(pt->a, at->a, names, out);
    case T_Task: return at->kind == T_Task && unify(pt->a, at->a, names, out);
    case T_Channel: return at->kind == T_Channel && unify(pt->a, at->a, names, out);
    case T_Map:
      return at->kind == T_Map && unify(pt->a, at->a, names, out) && unify(pt->b, at->b, names, out);
    case T_Func: {
      if (at->kind != T_Func || at->params.size() != pt->params.size()) return false;
      for (int i = 0; i < pt->params.size(); i++)
        if (!unify(pt->params[i], at->params[i], names, out)) return false;
      return unify(pt->ret, at->ret, names, out);
    }
    default: return true;
  }
}

bool Checker::resolve_overload(Node* call, const Vec<int>& cand_funcs, const Vec<int>& cand_natives,
                               Vec<Node*>& args, const Str& shown, const Vec<Str>* cls_gparams,
                               const Vec<Type*>* cls_targs) {
  int nargs = args.size();
  int best = -1;
  bool best_native = false;
  int best_score = -1;
  bool ambiguous = false;
  Vec<Type*> best_targs;
  Type* best_ret = 0;

  for (int pass = 0; pass < 2; pass++) {
    int count = pass == 0 ? cand_funcs.size() : cand_natives.size();
    for (int ci = 0; ci < count; ci++) {
      int idx = pass == 0 ? cand_funcs[ci] : cand_natives[ci];
      Vec<Type*> ps;
      Type* ret = 0;
      FuncInfo* fi = 0;
      const Vec<Str>* gnames = 0;
      if (pass == 0) {
        fi = prog_.funcs[idx];
        for (int i = 0; i < fi->params.size(); i++) {
          Type* pt = fi->params[i].type;
          if (cls_gparams && cls_targs && cls_targs->size()) pt = subst(pt, *cls_gparams, *cls_targs);
          ps.push(pt);
        }
        ret = fi->ret;
        if (cls_gparams && cls_targs && cls_targs->size()) ret = subst(ret, *cls_gparams, *cls_targs);
        if (fi->is_generic) gnames = &fi->gparams;
      } else {
        const NativeEntry& ne = reg_[idx];
        if (!ne.typed) continue;
        for (int i = 0; i < ne.params.size(); i++) ps.push(ne.params[i]);
        ret = ne.ret;
      }
      if (ps.size() != nargs) continue;

      Vec<Type*> targs;
      if (gnames) {
        for (int i = 0; i < gnames->size(); i++) targs.push(0);
        // 書いてある型引数があれば先に使う
        for (int i = 0; i < call->targs.size() && i < targs.size(); i++)
          targs[i] = resolve_type(call->targs[i], unit_, fc_, fc_->cls);
        bool ok = true;
        for (int i = 0; i < nargs; i++)
          if (!unify(ps[i], args[i]->type, *gnames, targs)) { ok = false; break; }
        if (!ok) continue;
        for (int i = 0; i < targs.size(); i++) if (!targs[i]) targs[i] = t_.t_unknown();
        Vec<Type*> ps2;
        for (int i = 0; i < ps.size(); i++) ps2.push(subst(ps[i], *gnames, targs));
        ps = ps2;
        ret = subst(ret, *gnames, targs);
      }

      int score = 2;
      bool ok = true;
      for (int i = 0; i < nargs; i++) {
        Type* at = args[i]->type;
        if (type_same(ps[i], at)) continue;
        if (type_assignable(ps[i], at)) { score = 1; continue; }
        ok = false;
        break;
      }
      if (!ok) continue;
      if (gnames) score -= 1;  // 普通の関数を優先する
      if (score > best_score) {
        best_score = score;
        best = idx;
        best_native = (pass == 1);
        best_targs = targs;
        best_ret = ret;
        ambiguous = false;
      } else if (score == best_score && best >= 0) {
        ambiguous = true;
      }
    }
  }

  if (best < 0) {
    Str want;
    want += "(";
    for (int i = 0; i < nargs; i++) { if (i) want += ", "; want += type_name(args[i]->type); }
    want += ")";
    Diagnostic& d = diag_.error("E0138", diag_.L(shown + want + " に当てはまる関数がありません",
                                                 Str("no function matches ") + shown + want));
    d.spans.push(Span(call->line, call->col, call->len));
    Str cands;
    for (int i = 0; i < cand_funcs.size(); i++) {
      FuncInfo* f = prog_.funcs[cand_funcs[i]];
      Vec<Type*> ps;
      for (int k = 0; k < f->params.size(); k++) ps.push(f->params[k].type);
      if (cands.size()) cands += "\n  ";
      collect_sig(cands, shown, ps);
    }
    for (int i = 0; i < cand_natives.size(); i++) {
      const NativeEntry& ne = reg_[cand_natives[i]];
      if (!ne.typed) continue;
      if (cands.size()) cands += "\n  ";
      collect_sig(cands, shown, ne.params);
    }
    if (cands.size())
      d.help.push(diag_.L(Str("あるのは次のものです\n  ") + cands, Str("candidates:\n  ") + cands));
    else
      d.help.push(diag_.L("名前の綴りを確かめます", "check the spelling"));
    call->type = t_.t_unknown();
    return false;
  }
  if (ambiguous) {
    Diagnostic& d = diag_.error("E0139", diag_.L(shown + " はどれを呼ぶか決められません",
                                                 Str("ambiguous call to ") + shown));
    d.spans.push(Span(call->line, call->col, call->len));
    d.help.push(diag_.L("引数の型をはっきりさせます", "make the argument types explicit"));
  }

  // 引数の確かめと ref の規則
  const Vec<ParamInfo>* fps = 0;
  Vec<Type*> ps;
  if (!best_native) {
    FuncInfo* f = prog_.funcs[best];
    fps = &f->params;
    for (int i = 0; i < f->params.size(); i++) {
      Type* pt = f->params[i].type;
      if (cls_gparams && cls_targs && cls_targs->size()) pt = subst(pt, *cls_gparams, *cls_targs);
      if (f->is_generic) pt = subst(pt, f->gparams, best_targs);
      ps.push(pt);
    }
    call->opcode = CK_Func;
    call->resolved = f->index;
    call->rfunc = f;
  } else {
    const NativeEntry& ne = reg_[best];
    for (int i = 0; i < ne.params.size(); i++) ps.push(ne.params[i]);
    call->opcode = CK_Native;
    call->resolved = best;
  }
  for (int i = 0; i < nargs; i++) {
    bool want_ref = fps ? (*fps)[i].is_ref : (i == 0 && reg_[best].ref0);
    bool given_ref = args[i]->kind == E_Ref;
    if (want_ref && !given_ref) {
      Diagnostic& d = diag_.error("E0301", diag_.L("ここは ref を付けて渡します",
                                                   "this argument must be passed with ref"));
      d.spans.push(Span(args[i]->line, args[i]->col, args[i]->len));
      Str at = expr_text(args[i]);
      d.help.push(diag_.L(Str("呼ぶ側にも ref と書きます: ") + shown + "(ref " + (at.size() ? at : Str("...")) + ")",
                          Str("write ref at the call site too: ") + shown + "(ref " +
                              (at.size() ? at : Str("...")) + ")"));
    } else if (!want_ref && given_ref) {
      Diagnostic& d = diag_.error("E0301", diag_.L("この引数は ref を受け取りません",
                                                   "this parameter does not take ref"));
      d.spans.push(Span(args[i]->line, args[i]->col, args[i]->len));
      d.help.push(diag_.L("ref を外します。値はコピーで渡ります",
                          "drop the ref; the value is passed as a copy"));
    }
    if (given_ref) {
      Node* inner = args[i]->a;
      if (inner->kind == E_Ident) {
        Local* l = find_local(inner->name);
        if (l && l->is_const)
          err("E0113", diag_.L("const は ref で渡せません", "a const cannot be passed by ref"), args[i]);
      }
    }
    need_assign(ps[i], args[i]->type, args[i], "引数", "argument");
  }
  call->type = best_ret ? best_ret : t_.t_void();
  return true;
}

// ------------------------------------------------------------------ 型変換
bool Checker::resolve_convert(Node* call, const Str& name, Vec<Node*>& args) {
  if (args.size() != 1) {
    Diagnostic& d = diag_.error("E0140", diag_.L(name + "(...) には値を1つ渡します",
                                                 name + "(...) takes exactly one value"));
    d.spans.push(Span(call->line, call->col, call->len));
    call->type = t_.t_unknown();
    return false;
  }
  Type* at = args[0]->type;
  call->opcode = CK_Native;
  if (name == "int") {
    if (at->kind == T_Int) { call->opcode = CK_Convert; call->resolved = 0; call->type = t_.t_int(); return true; }
    if (at->kind == T_Float) { call->resolved = reg_.find("conv.int_from_float"); call->type = t_.t_int(); return true; }
    if (at->kind == T_String) { call->resolved = reg_.find("conv.int_from_string"); call->type = t_.optional_of(t_.t_int()); return true; }
    if (at->kind == T_Bool) { call->resolved = reg_.find("conv.int_from_bool"); call->type = t_.t_int(); return true; }
  } else if (name == "float") {
    if (at->kind == T_Float) { call->opcode = CK_Convert; call->resolved = 0; call->type = t_.t_float(); return true; }
    if (at->kind == T_Int) { call->resolved = reg_.find("conv.float_from_int"); call->type = t_.t_float(); return true; }
    if (at->kind == T_String) { call->resolved = reg_.find("conv.float_from_string"); call->type = t_.optional_of(t_.t_float()); return true; }
  } else if (name == "bool") {
    if (at->kind == T_Bool) { call->opcode = CK_Convert; call->resolved = 0; call->type = t_.t_bool(); return true; }
    if (at->kind == T_String) { call->resolved = reg_.find("conv.bool_from_string"); call->type = t_.optional_of(t_.t_bool()); return true; }
  } else if (name == "string") {
    if (at->kind == T_Class) {
      // to_string() を使う
      Node* fld = arena_.make<Node>();
      fld->kind = E_Field;
      fld->name = Str("to_string");
      fld->line = call->line; fld->col = call->col; fld->len = call->len;
      fld->a = args[0];
      call->a = fld;
      call->list.clear();
      call->slot2 = 1;
      Vec<Node*> none;
      Type* rt = check_expr(args[0]);
      (void)rt;
      if (!resolve_class_method(call, args[0]->type, Str("to_string"), none, false)) {
        Diagnostic& d = diag_.error("E0127", diag_.L(type_name(at) + " を文字列にするには to_string() が要ります",
                                                     Str("to_string() is required to convert ") + type_name(at)));
        d.spans.push(Span(call->line, call->col, call->len));
        d.help.push(diag_.L("クラスに func to_string() -> string { } を足します",
                            "add func to_string() -> string { } to the class"));
        call->type = t_.t_string();
      }
      return true;
    }
    if (is_optional(at)) {
      Diagnostic& d = diag_.error("E0201", diag_.L(type_name(at) + " はそのままでは文字列にできません",
                                                   Str("cannot convert ") + type_name(at) + " directly"));
      d.spans.push(Span(args[0]->line, args[0]->col, args[0]->len));
      note_optional(d, expr_text(args[0]));
      call->type = t_.t_string();
      return true;
    }
    call->resolved = reg_.find("conv.string_from");
    call->type = t_.t_string();
    return true;
  }
  if (at->kind == T_Unknown) { call->type = t_.t_unknown(); call->resolved = 0; call->opcode = CK_Convert; return true; }
  Diagnostic& d = diag_.error("E0141", diag_.L(type_name(at) + " から " + name + " への変換はありません",
                                               Str("there is no conversion from ") + type_name(at) + " to " + name));
  d.spans.push(Span(call->line, call->col, call->len));
  d.help.push(diag_.L("使えるのは int(float) int(string) float(int) float(string) string(値) bool(string) です",
                      "available: int(float), int(string), float(int), float(string), string(v), bool(string)"));
  call->type = t_.t_unknown();
  return false;
}

// ------------------------------------------------------------------ クラスの生成
bool Checker::resolve_ctor(Node* call, ClassInfo* c, Vec<Node*>& args, const Vec<Type*>& targs) {
  layout_class(c);
  if (c->is_abstract) {
    Str missing;
    for (int s = 0; s < c->vtable.size(); s++) {
      if (c->vtable[s] != -1) continue;
      if (missing.size()) missing += ", ";
      missing += vkeys_[s];
    }
    Diagnostic& d = diag_.error("E0403", diag_.L(c->name + " は抽象クラスなので、そのままでは作れません",
                                                 c->name + " is abstract and cannot be instantiated"));
    d.spans.push(Span(call->line, call->col, call->len));
    d.help.push(diag_.L(Str("実装されていないメソッド: ") + missing +
                            "\n  これらを override した子クラスを作ります",
                        Str("unimplemented: ") + missing + "\n  create a subclass that overrides them"));
    call->type = t_.class_type(c);
    return false;
  }
  Vec<int> inits;
  for (ClassInfo* p = c; p; p = p->base) {
    for (int i = 0; i < p->methods.size(); i++) {
      FuncInfo* f = prog_.funcs[p->methods[i].func];
      if (f->is_init) inits.push(f->index);
    }
    if (inits.size()) break;  // 一番近い階層のものを使う
  }
  call->rcls = c;
  call->opcode = CK_Ctor;
  Type* ct = targs.size() ? t_.class_type_args(c, targs) : t_.class_type(c);
  if (inits.size() == 0) {
    if (args.size() != 0) {
      Diagnostic& d = diag_.error("E0406", diag_.L(c->name + " には init が無いので、引数は渡せません",
                                                   c->name + " has no init, so it takes no arguments"));
      d.spans.push(Span(call->line, call->col, call->len));
      d.help.push(diag_.L(Str("func init(...) を ") + c->name + " に足します",
                          Str("add func init(...) to ") + c->name));
    }
    call->resolved = -1;
    call->type = ct;
    return true;
  }
  Vec<int> no_natives;
  if (!resolve_overload(call, inits, no_natives, args, c->name, &c->gparams, &targs)) {
    call->type = ct;
    return false;
  }
  call->opcode = CK_Ctor;   // resolve_overload が CK_Func にしてしまうので戻す
  call->rcls = c;
  call->type = ct;
  return true;
}

// ------------------------------------------------------------------ クラスのメソッド
bool Checker::resolve_class_method(Node* call, Type* recv, const Str& name, Vec<Node*>& args,
                                   bool via_super) {
  if (!recv || recv->kind != T_Class || !recv->cls) return false;
  ClassInfo* c = recv->cls;
  Vec<int> cand;
  for (ClassInfo* p = c; p; p = p->base) {
    for (int i = 0; i < p->methods.size(); i++) {
      FuncInfo* f = prog_.funcs[p->methods[i].func];
      if (!(f->name == name)) continue;
      bool dup = false;
      for (int k = 0; k < cand.size(); k++) {
        FuncInfo* g = prog_.funcs[cand[k]];
        if (g->params.size() != f->params.size()) continue;
        bool same = true;
        for (int m = 0; m < f->params.size(); m++)
          if (!type_same(g->params[m].type, f->params[m].type)) { same = false; break; }
        if (same) { dup = true; break; }
      }
      if (!dup) cand.push(f->index);
    }
  }
  // インタフェースのメソッド
  for (int i = 0; i < c->interfaces.size(); i++) {
    ClassInfo* ic = c->interfaces[i];
    for (int k = 0; k < ic->methods.size(); k++) {
      FuncInfo* f = prog_.funcs[ic->methods[k].func];
      if (!(f->name == name)) continue;
      bool dup = false;
      for (int m = 0; m < cand.size(); m++) if (cand[m] == f->index) dup = true;
      if (!dup) cand.push(f->index);
    }
  }
  if (cand.size() == 0) return false;

  // 見えるか
  FuncInfo* first = prog_.funcs[cand[0]];
  if (!first->is_public && !(fc_->cls && class_is(fc_->cls, first->owner))) {
    Diagnostic& d = diag_.error("E0136", diag_.L(name + "() は " + c->name + " の中からしか呼べません",
                                                 name + "() is private to " + c->name));
    d.spans.push(Span(call->line, call->col, call->len));
    d.help.push(diag_.L("外から呼ばせたいときは public を付けます", "add public to expose it"));
  }

  Vec<int> no_natives;
  if (!resolve_overload(call, cand, no_natives, args, name)) return true;

  FuncInfo* f = call->rfunc;
  if (f && f->is_virtual && !via_super) {
    call->opcode = CK_Virtual;
    call->resolved2 = f->vslot;
  }
  // ジェネリッククラスの型引数を当てはめる
  if (recv->targs.size() && c->gparams.size() && call->type)
    call->type = subst(call->type, c->gparams, recv->targs);
  return true;
}

// ------------------------------------------------------------------ 型ごとのメソッド
bool Checker::resolve_builtin_method(Node* call, Type* recv, const Str& name, Vec<Node*>& args) {
  int n = args.size();
  Type* ti = t_.t_int();
  Type* tf = t_.t_float();
  Type* ts = t_.t_string();
  Type* tb = t_.t_bool();
  Type* tv = t_.t_void();
  const char* nat = 0;
  Type* ret = 0;
  bool by_ref = false;
  Vec<Type*> want;

  switch (recv->kind) {
    case T_String:
      if (name == "len" && n == 0) { nat = "string.len"; ret = ti; }
      else if (name == "sub" && n == 2) { nat = "string.sub"; ret = ts; want.push(ti); want.push(ti); }
      else if (name == "find" && n == 1) { nat = "string.find"; ret = t_.optional_of(ti); want.push(ts); }
      else if (name == "contains" && n == 1) { nat = "string.contains"; ret = tb; want.push(ts); }
      else if (name == "starts_with" && n == 1) { nat = "string.starts_with"; ret = tb; want.push(ts); }
      else if (name == "ends_with" && n == 1) { nat = "string.ends_with"; ret = tb; want.push(ts); }
      else if (name == "split" && n == 1) { nat = "string.split"; ret = t_.list_of(ts); want.push(ts); }
      else if (name == "trim" && n == 0) { nat = "string.trim"; ret = ts; }
      else if (name == "upper" && n == 0) { nat = "string.upper"; ret = ts; }
      else if (name == "lower" && n == 0) { nat = "string.lower"; ret = ts; }
      else if (name == "replace" && n == 2) { nat = "string.replace"; ret = ts; want.push(ts); want.push(ts); }
      else if (name == "bytes" && n == 0) { nat = "string.bytes"; ret = t_.t_bytes(); }
      else if (name == "chars" && n == 0) { nat = "string.chars"; ret = t_.list_of(ts); }
      else if (name == "compare" && n == 1) { want.push(ts); ret = ti; call->opcode = CK_CmpDyn; }
      break;
    case T_Bytes:
      if (name == "len" && n == 0) { nat = "bytes.len"; ret = ti; }
      else if (name == "push" && n == 1) { nat = "bytes.push"; ret = tv; want.push(ti); by_ref = true; }
      else if (name == "to_string" && n == 0) { nat = "bytes.to_string"; ret = t_.optional_of(ts); }
      else if (name == "list" && n == 0) { nat = "bytes.list"; ret = t_.list_of(ti); }
      else if (name == "sub" && n == 2) { nat = "bytes.sub"; ret = t_.t_bytes(); want.push(ti); want.push(ti); }
      else if (name == "compare" && n == 1) { want.push(t_.t_bytes()); ret = ti; call->opcode = CK_CmpDyn; }
      break;
    case T_List: {
      Type* et = recv->a;
      if (name == "push" && n == 1) { nat = "list.push"; ret = tv; want.push(et); by_ref = true; }
      else if (name == "pop" && n == 0) { nat = "list.pop"; ret = t_.optional_of(et); by_ref = true; }
      else if (name == "len" && n == 0) { nat = "list.len"; ret = ti; }
      else if (name == "insert" && n == 2) { nat = "list.insert"; ret = tv; want.push(ti); want.push(et); by_ref = true; }
      else if (name == "remove" && n == 1) { nat = "list.remove"; ret = tv; want.push(ti); by_ref = true; }
      else if (name == "contains" && n == 1) { nat = "list.contains"; ret = tb; want.push(et); }
      else if (name == "sort" && n == 0) {
        // 並べ替えは Shark で書いた __sort を呼ぶ。基本型もクラスも同じコードで動く
        if (!satisfies(et, c_comparable_)) {
          Diagnostic& d = diag_.error("E0142", diag_.L(type_name(et) + " は並べ替えられません（Comparable ではありません）",
                                                       type_name(et) + " is not Comparable"));
          d.spans.push(Span(call->line, call->col, call->len));
          d.help.push(diag_.L("クラスに Comparable を実装し、compare(other: This) -> int を書きます",
                              "implement Comparable with compare(other: This) -> int"));
        }
        int fi = -1;
        for (int i = 0; i < prog_.funcs.size(); i++)
          if (!prog_.funcs[i]->owner && prog_.funcs[i]->name == "__sort") { fi = i; break; }
        Node* obj = call->a->a;
        if (!(obj->kind == E_Ident || obj->kind == E_Field || obj->kind == E_Index)) {
          Diagnostic& d = diag_.error("E0145", diag_.L("sort() は変数に対して呼びます",
                                                       "sort() must be called on a variable"));
          d.spans.push(Span(call->line, call->col, call->len));
        }
        call->opcode = CK_Func;
        call->resolved = fi;
        call->resolved2 = 1;   // 受け手は借用で渡す
        call->rfunc = fi >= 0 ? prog_.funcs[fi] : 0;
        call->type = tv;
        return true;
      }
      else if (name == "join" && n == 1) {
        nat = "list.join"; ret = ts; want.push(ts);
        if (et && et->kind != T_String && et->kind != T_Unknown) {
          Diagnostic& d = diag_.error("E0143", diag_.L("join でつなげるのは list<string> だけです",
                                                       "join is only available on list<string>"));
          d.spans.push(Span(call->line, call->col, call->len));
          d.help.push(diag_.L("string(...) で文字列の配列に直してから使います",
                              "convert the elements to string first"));
        }
      }
      break;
    }
    case T_Map: {
      Type* kt = recv->a;
      Type* vt = recv->b;
      if (name == "get" && n == 1) { nat = "map.get"; ret = t_.optional_of(vt); want.push(kt); }
      else if (name == "has" && n == 1) { nat = "map.has"; ret = tb; want.push(kt); }
      else if (name == "remove" && n == 1) { nat = "map.remove"; ret = tv; want.push(kt); by_ref = true; }
      else if (name == "len" && n == 0) { nat = "map.len"; ret = ti; }
      else if (name == "keys" && n == 0) { nat = "map.keys"; ret = t_.list_of(kt); }
      else if (name == "values" && n == 0) { nat = "map.values"; ret = t_.list_of(vt); }
      break;
    }
    case T_Result: {
      Type* vt = recv->a;
      if (name == "ok" && n == 0) { nat = "result.ok"; ret = tb; }
      else if (name == "value" && n == 0) { nat = "result.value"; ret = vt; }
      else if (name == "error" && n == 0) { nat = "result.error"; ret = t_.class_type(c_error_); }
      break;
    }
    case T_Int:
      if (name == "wrapping_add" && n == 1) { nat = "int.wrapping_add"; ret = ti; want.push(ti); }
      else if (name == "wrapping_sub" && n == 1) { nat = "int.wrapping_sub"; ret = ti; want.push(ti); }
      else if (name == "wrapping_mul" && n == 1) { nat = "int.wrapping_mul"; ret = ti; want.push(ti); }
      else if (name == "to_bytes" && n == 0) { nat = "int.to_bytes"; ret = t_.t_bytes(); }
      else if (name == "compare" && n == 1) { want.push(ti); ret = ti; call->opcode = CK_CmpDyn; }
      break;
    case T_Float:
      if (name == "compare" && n == 1) { want.push(tf); ret = ti; call->opcode = CK_CmpDyn; }
      break;
    case T_Bool:
      if (name == "compare" && n == 1) { want.push(tb); ret = ti; call->opcode = CK_CmpDyn; }
      break;
    case T_Time:
      if (name == "format" && n == 1) { nat = "time.Time.format"; ret = ts; want.push(ts); }
      else if (name == "year" && n == 0) { nat = "time.Time.year"; ret = ti; }
      else if (name == "month" && n == 0) { nat = "time.Time.month"; ret = ti; }
      else if (name == "day" && n == 0) { nat = "time.Time.day"; ret = ti; }
      else if (name == "hour" && n == 0) { nat = "time.Time.hour"; ret = ti; }
      else if (name == "minute" && n == 0) { nat = "time.Time.minute"; ret = ti; }
      else if (name == "second" && n == 0) { nat = "time.Time.second"; ret = ti; }
      else if (name == "weekday" && n == 0) { nat = "time.Time.weekday"; ret = ti; }
      else if (name == "to_local" && n == 0) { nat = "time.Time.to_local"; ret = t_.simple(T_Time); }
      else if (name == "to_utc" && n == 0) { nat = "time.Time.to_utc"; ret = t_.simple(T_Time); }
      else if (name == "compare" && n == 1) { want.push(t_.simple(T_Time)); ret = ti; call->opcode = CK_CmpDyn; }
      break;
    case T_Duration:
      if (name == "seconds" && n == 0) { nat = "time.Duration.seconds"; ret = tf; }
      else if (name == "minutes" && n == 0) { nat = "time.Duration.minutes"; ret = tf; }
      else if (name == "hours" && n == 0) { nat = "time.Duration.hours"; ret = tf; }
      else if (name == "days" && n == 0) { nat = "time.Duration.days"; ret = tf; }
      else if (name == "compare" && n == 1) { want.push(t_.simple(T_Duration)); ret = ti; call->opcode = CK_CmpDyn; }
      break;
    case T_File:
      if (name == "read_line" && n == 0) { nat = "file.File.read_line"; ret = t_.optional_of(ts); }
      else if (name == "read" && n == 1) { nat = "file.File.read"; ret = t_.result_of(t_.t_bytes()); want.push(ti); }
      else if (name == "write" && n == 1) { nat = "file.File.write"; ret = t_.result_of(tv); want.push(ts); }
      else if (name == "close" && n == 0) { nat = "file.File.close"; ret = tv; }
      break;
    case T_Task: {
      Type* vt = recv->a;
      if (name == "wait" && n == 0) { nat = "task.Task.wait"; ret = vt; }
      else if (name == "wait_timeout" && n == 1) { nat = "task.Task.wait_timeout"; ret = t_.optional_of(vt); want.push(t_.simple(T_Duration)); }
      else if (name == "done" && n == 0) { nat = "task.Task.done"; ret = tb; }
      else if (name == "cancel" && n == 0) { nat = "task.Task.cancel"; ret = tv; }
      else if (name == "id" && n == 0) { nat = "task.Task.id"; ret = ti; }
      break;
    }
    case T_Channel: {
      Type* vt = recv->a;
      if (name == "send" && n == 1) { nat = "task.channel.send"; ret = t_.result_of(tv); want.push(vt); }
      else if (name == "recv" && n == 0) { nat = "task.channel.recv"; ret = t_.optional_of(vt); }
      else if (name == "try_recv" && n == 0) { nat = "task.channel.try_recv"; ret = t_.optional_of(vt); }
      else if (name == "close" && n == 0) { nat = "task.channel.close"; ret = tv; }
      else if (name == "len" && n == 0) { nat = "task.channel.len"; ret = ti; }
      break;
    }
    case T_Json:
      if (name == "string" && n == 0) { nat = "json.Json.string"; ret = t_.optional_of(ts); }
      else if (name == "int" && n == 0) { nat = "json.Json.int"; ret = t_.optional_of(ti); }
      else if (name == "float" && n == 0) { nat = "json.Json.float"; ret = t_.optional_of(tf); }
      else if (name == "bool" && n == 0) { nat = "json.Json.bool"; ret = t_.optional_of(tb); }
      else if (name == "list" && n == 0) { nat = "json.Json.list"; ret = t_.list_of(t_.simple(T_Json)); }
      else if (name == "keys" && n == 0) { nat = "json.Json.keys"; ret = t_.list_of(ts); }
      else if (name == "exists" && n == 0) { nat = "json.Json.exists"; ret = tb; }
      else if (name == "kind" && n == 0) { nat = "json.Json.kind"; ret = ts; }
      break;
    case T_Regex:
      if (name == "find" && n == 1) { nat = "text.Regex.find"; ret = t_.optional_of(t_.simple(T_Match)); want.push(ts); }
      else if (name == "find_all" && n == 1) { nat = "text.Regex.find_all"; ret = t_.list_of(t_.simple(T_Match)); want.push(ts); }
      else if (name == "matches" && n == 1) { nat = "text.Regex.matches"; ret = tb; want.push(ts); }
      else if (name == "replace" && n == 2) { nat = "text.Regex.replace"; ret = ts; want.push(ts); want.push(ts); }
      else if (name == "split" && n == 1) { nat = "text.Regex.split"; ret = t_.list_of(ts); want.push(ts); }
      break;
    case T_Match:
      if (name == "text" && n == 0) { nat = "text.Match.text"; ret = ts; }
      else if (name == "start" && n == 0) { nat = "text.Match.start"; ret = ti; }
      else if (name == "end" && n == 0) { nat = "text.Match.end"; ret = ti; }
      else if (name == "group" && n == 1) { nat = "text.Match.group"; ret = t_.optional_of(ts); want.push(ti); }
      break;
    case T_Output:
      if (name == "code" && n == 0) { nat = "os.Output.code"; ret = ti; }
      else if (name == "out" && n == 0) { nat = "os.Output.out"; ret = ts; }
      else if (name == "err" && n == 0) { nat = "os.Output.err"; ret = ts; }
      break;
    case T_Generic: {
      // 制約に書いたインタフェースのメソッドだけ呼べる
      for (int i = 0; i < recv->constraints.size(); i++) {
        ClassInfo* ic = recv->constraints[i];
        if (ic == c_comparable_ && name == "compare" && n == 1) {
          want.push(recv);
          ret = ti;
          call->opcode = CK_CmpDyn;
          break;
        }
        for (int k = 0; k < ic->methods.size(); k++) {
          FuncInfo* f = prog_.funcs[ic->methods[k].func];
          if (!(f->name == name) || f->params.size() != n) continue;
          call->opcode = CK_Virtual;
          call->resolved = f->index;
          call->resolved2 = f->vslot;
          call->rfunc = f;
          for (int m = 0; m < f->params.size(); m++) {
            Type* pt = f->params[m].type;
            if (pt->kind == T_Generic && pt->name == "This") pt = recv;
            need_assign(pt, args[m]->type, args[m], "引数", "argument");
          }
          call->type = f->ret;
          return true;
        }
      }
      if (!ret && call->opcode != CK_CmpDyn) {
        Diagnostic& d = diag_.error("E0144", diag_.L(Str("型引数 ") + recv->name + " に " + name + "() があるとは限りません",
                                                     Str("no guarantee that ") + recv->name + " has " + name + "()"));
        d.spans.push(Span(call->line, call->col, call->len));
        d.help.push(diag_.L(Str("制約を書きます: <") + recv->name + ": インタフェース名>\n"
                            "  制約の無い型引数には、代入とコピーしかできません",
                            Str("add a constraint: <") + recv->name + ": Interface>"));
        call->type = t_.t_unknown();
        return true;
      }
      break;
    }
    case T_Unknown:
      call->type = t_.t_unknown();
      call->opcode = CK_None;
      return true;
    default:
      break;
  }

  if (!nat && call->opcode != CK_CmpDyn) return false;

  for (int i = 0; i < want.size() && i < n; i++)
    need_assign(want[i], args[i]->type, args[i], "引数", "argument");

  if (call->opcode == CK_CmpDyn) {
    call->type = ti;
    return true;
  }
  int id = reg_.find(nat);
  if (id < 0) {
    // モジュールがビルドに入っていない
    Diagnostic& d = diag_.error("E0501", diag_.L(Str("この処理系は ") + nat + " を持っていません",
                                                 Str("this runtime does not include ") + nat));
    d.spans.push(Span(call->line, call->col, call->len));
    call->type = ret ? ret : t_.t_unknown();
    return true;
  }
  if (by_ref) {
    Node* obj = call->a->a;  // 受け手の式
    if (!(obj->kind == E_Ident || obj->kind == E_Field || obj->kind == E_Index)) {
      Diagnostic& d = diag_.error("E0145", diag_.L(name + "() は変数に対して呼びます",
                                                   name + "() must be called on a variable"));
      d.spans.push(Span(call->line, call->col, call->len));
      d.help.push(diag_.L("いったん変数に入れてから呼びます", "assign it to a variable first"));
    }
    if (obj->kind == E_Ident) {
      Local* l = find_local(obj->name);
      if (l && l->is_const) {
        Diagnostic& d = diag_.error("E0113", diag_.L(Str("const の ") + l->name + " は書き換えられません",
                                                     Str("cannot modify const ") + l->name));
        d.spans.push(Span(call->line, call->col, call->len));
      }
    }
  }
  call->opcode = CK_Native;
  call->resolved = id;
  call->resolved2 = by_ref ? 1 : 0;
  call->type = ret ? ret : tv;
  return true;
}

// ------------------------------------------------------------------ 呼び出し全体
Type* Checker::check_call(Node* e) {
  Node* callee = e->a;
  Vec<Node*> args;
  for (int i = 0; i < e->list.size(); i++) args.push(e->list[i]);
  for (int i = 0; i < args.size(); i++) check_expr(args[i]);
  int n = args.size();

  // --- 名前だけの呼び出し ---
  if (callee->kind == E_Ident) {
    const Str& name = callee->name;
    if (name == "int" || name == "float" || name == "string" || name == "bool") {
      resolve_convert(e, name, args);
      return e->type;
    }
    if (name == "channel") {
      Type* vt = e->targs.size() ? resolve_type(e->targs[0], unit_, fc_, fc_->cls) : t_.t_unknown();
      if (!e->targs.size()) {
        Diagnostic& d = diag_.error("E0146", diag_.L("channel には型を書きます: channel<int>()",
                                                     "channel needs a type: channel<int>()"));
        d.spans.push(Span(e->line, e->col, e->len));
      }
      if (n > 1) {
        Diagnostic& d = diag_.error("E0146", diag_.L("channel<T>() か channel<T>(ためられる件数) と書きます",
                                                     "channel<T>() or channel<T>(capacity)"));
        d.spans.push(Span(e->line, e->col, e->len));
      }
      if (n == 1) need_assign(t_.t_int(), args[0]->type, args[0], "引数", "argument");
      e->opcode = CK_Native;
      e->resolved = reg_.find(n == 0 ? "task.channel" : "task.channel_cap");
      if (e->resolved < 0) {
        Diagnostic& d = diag_.error("E0501", diag_.L("この処理系は task を持っていません",
                                                     "this runtime does not include task"));
        d.spans.push(Span(e->line, e->col, e->len));
      }
      return t_.channel_of(vt);
    }
    if (name == "len") {
      if (n != 1) {
        err("E0140", diag_.L("len(...) には値を1つ渡します", "len(...) takes one value"), e);
        return t_.t_int();
      }
      Type* at = args[0]->type;
      bool ok = at && (at->kind == T_String || at->kind == T_Bytes || at->kind == T_List ||
                       at->kind == T_Map || at->kind == T_Unknown);
      if (!ok) {
        Diagnostic& d = diag_.error("E0147", diag_.L(type_name(at) + " に長さはありません",
                                                     type_name(at) + " has no length"));
        d.spans.push(Span(args[0]->line, args[0]->col, args[0]->len));
        d.help.push(diag_.L("len が使えるのは string bytes list map です",
                            "len works on string, bytes, list and map"));
      }
      e->opcode = CK_Native;
      e->resolved = reg_.find("len");
      return t_.t_int();
    }
    if (name == "print" || name == "write") {
      if (n != 1) {
        err("E0140", diag_.L(name + "(...) には値を1つ渡します", name + "(...) takes one value"), e);
        return t_.t_void();
      }
      Type* at = args[0]->type;
      bool ok = at && (at->kind == T_Int || at->kind == T_Float || at->kind == T_Bool ||
                       at->kind == T_String || at->kind == T_Bytes || at->kind == T_List ||
                       at->kind == T_Map || at->kind == T_Unknown);
      if (!ok) {
        Diagnostic& d = diag_.error("E0148", diag_.L(name + " に " + type_name(at) + " はそのままでは渡せません",
                                                     name + " cannot print " + type_name(at)));
        d.spans.push(Span(args[0]->line, args[0]->col, args[0]->len));
        if (at && at->kind == T_Class)
          d.help.push(diag_.L(Str("string(...) で文字列にしてから渡します: ") + name + "(string(v))\n"
                              "  クラスには to_string() を定義します",
                              Str("convert first: ") + name + "(string(v))"));
        else if (at && is_optional(at))
          note_optional(d, Str());
        else
          d.help.push(diag_.L("string(...) で文字列にしてから渡します", "convert with string(...) first"));
      }
      e->opcode = CK_Native;
      e->resolved = reg_.find(name == "print" ? "print" : "write");
      return t_.t_void();
    }
    // クラスの生成
    ClassInfo* c = find_class(name, unit_);
    if (c) {
      Vec<Type*> targs;
      for (int i = 0; i < e->targs.size(); i++) targs.push(resolve_type(e->targs[i], unit_, fc_, fc_->cls));
      if (targs.size() == 0 && c->gparams.size() > 0) {
        // 引数から型を決める（init の引数と突き合わせる）
        for (int i = 0; i < c->gparams.size(); i++) targs.push(0);
        for (int i = 0; i < c->methods.size(); i++) {
          FuncInfo* f = prog_.funcs[c->methods[i].func];
          if (!f->is_init || f->params.size() != (int)n) continue;
          for (int k = 0; k < n; k++) unify(f->params[k].type, args[k]->type, c->gparams, targs);
          break;
        }
        for (int i = 0; i < targs.size(); i++) if (!targs[i]) targs[i] = t_.t_unknown();
      }
      resolve_ctor(e, c, args, targs);
      return e->type;
    }
    // 関数を値として持っている変数
    Local* l = find_local(name);
    if (l && l->type && l->type->kind == T_Func) {
      l->used = true;
      callee->slot = l->slot;
      callee->type = l->type;
      e->opcode = CK_Value;
      Type* ft = l->type;
      if (ft->params.size() != n) {
        Diagnostic& d = diag_.error("E0149", diag_.L(Str("引数の数が違います: ") + str_from_int(ft->params.size()) +
                                                         " 個必要です",
                                                     Str("wrong number of arguments: expected ") +
                                                         str_from_int(ft->params.size())));
        d.spans.push(Span(e->line, e->col, e->len));
      } else {
        for (int i = 0; i < n; i++) need_assign(ft->params[i], args[i]->type, args[i], "引数", "argument");
      }
      return ft->ret;
    }
    // 普通の関数とネイティブ
    Vec<int> cf, cn;
    for (int i = 0; i < prog_.funcs.size(); i++) {
      FuncInfo* f = prog_.funcs[i];
      if (f->owner) continue;
      if (!(f->module == unit_->module)) continue;
      if (f->name == name) cf.push(i);
    }
    reg_.find_all(name, &cn);
    // 自分のクラスのメソッドは this. を付けて呼ぶ
    if (cf.size() == 0 && cn.size() == 0 && fc_->cls) {
      for (int i = 0; i < fc_->cls->methods.size(); i++) {
        if (!(fc_->cls->methods[i].name == name)) continue;
        Diagnostic& d = diag_.error("E0150", diag_.L(name + "() はこのクラスのメソッドです",
                                                     name + "() is a method of this class"));
        d.spans.push(Span(e->line, e->col, e->len));
        d.help.push(diag_.L(Str("this. を付けて呼びます: this.") + name + "(...)",
                            Str("call it with this: this.") + name + "(...)"));
        return t_.t_unknown();
      }
    }
    if (cf.size() == 0 && cn.size() == 0) {
      Diagnostic& d = diag_.error("E0151", diag_.L(Str("関数 ") + name + " が見つかりません",
                                                   Str("unknown function: ") + name));
      d.spans.push(Span(e->line, e->col, e->len));
      // モジュールのものではないか
      Str hint;
      for (int i = 0; i < reg_.size(); i++) {
        const Str& rn = reg_[i].name;
        int dot = -1;
        for (int k = 0; k < rn.size(); k++) if (rn[k] == '.') { dot = k; break; }
        if (dot < 0) continue;
        if (rn.sub(dot + 1, rn.size() - dot - 1) == name) {
          hint = rn.sub(0, dot);
          break;
        }
      }
      if (hint.size())
        d.help.push(diag_.L(Str("std.") + hint + " にあります。import std." + hint + "; を足して " + hint + "." +
                                name + "(...) と呼びます",
                            Str("it lives in std.") + hint + "; add import std." + hint + ";"));
      else
        d.help.push(diag_.L("名前の綴りを確かめます。定義より前でも後でも呼べます",
                            "check the spelling"));
      return t_.t_unknown();
    }
    resolve_overload(e, cf, cn, args, name);
    return e->type;
  }

  // --- メンバの呼び出し ---
  if (callee->kind == E_Field) {
    Node* obj = callee->a;
    const Str& mname = callee->name;

    // super.xxx(...)
    if (obj->kind == E_Super) {
      if (!fc_->cls || !fc_->cls->base) {
        err("E0123", diag_.L("super は親クラスを持つクラスの中だけで使えます",
                             "super may only be used in a class with a base"), e);
        return t_.t_unknown();
      }
      Type* bt = t_.class_type(fc_->cls->base);
      obj->type = bt;
      obj->slot = 0;
      e->slot2 = 1;
      if (!resolve_class_method(e, bt, mname, args, true)) {
        Diagnostic& d = diag_.error("E0137", diag_.L(fc_->cls->base->name + " に " + mname + "() はありません",
                                                     fc_->cls->base->name + " has no method " + mname + "()"));
        d.spans.push(Span(e->line, e->col, e->len));
        return t_.t_unknown();
      }
      return e->type;
    }

    // モジュールの関数
    if (obj->kind == E_Ident && !find_local(obj->name) && !find_global(obj->name, unit_)) {
      bool found = false;
      Str mod = module_of_alias(obj->name, unit_, &found);
      if (found) {
        Str full = short_module_name(mod) + "." + mname;
        Vec<int> cn, cf;
        reg_.find_all(full, &cn);
        for (int i = 0; i < prog_.funcs.size(); i++) {
          FuncInfo* f = prog_.funcs[i];
          if (f->owner || !(f->module == mod) || !(f->name == mname)) continue;
          if (!f->is_public) {
            Diagnostic& d = diag_.error("E0134", diag_.L(mname + " は " + mod + " の外からは呼べません",
                                                         mname + " is not public in " + mod));
            d.spans.push(Span(e->line, e->col, e->len));
            d.help.push(diag_.L("呼ばせたい関数には public を付けます", "mark the function public"));
          }
          cf.push(i);
        }
        // モジュールの中のクラス
        if (cn.size() == 0 && cf.size() == 0) {
          ClassInfo* c = 0;
          for (int i = 0; i < prog_.classes.size(); i++)
            if (prog_.classes[i]->module == mod && prog_.classes[i]->name == mname) c = prog_.classes[i];
          if (c) {
            if (!c->is_public) {
              Diagnostic& d = diag_.error("E0134", diag_.L(mname + " は " + mod + " の外からは使えません",
                                                           mname + " is not public in " + mod));
              d.spans.push(Span(e->line, e->col, e->len));
            }
            Vec<Type*> targs;
            for (int i = 0; i < e->targs.size(); i++)
              targs.push(resolve_type(e->targs[i], unit_, fc_, fc_->cls));
            resolve_ctor(e, c, args, targs);
            return e->type;
          }
          Diagnostic& d = diag_.error("E0135", diag_.L(mod + " に " + mname + " はありません",
                                                       mod + " has no member named " + mname));
          d.spans.push(Span(e->line, e->col, e->len));
          Str avail;
          for (int i = 0; i < reg_.size(); i++) {
            const Str& rn = reg_[i].name;
            Str pre = short_module_name(mod) + ".";
            if (rn.size() > pre.size() && rn.sub(0, pre.size()) == pre) {
              Str rest = rn.sub(pre.size(), rn.size() - pre.size());
              bool dotted = false;
              for (int k = 0; k < rest.size(); k++) if (rest[k] == '.') dotted = true;
              if (dotted) continue;
              if (avail.size()) avail += ", ";
              avail += rest;
            }
          }
          if (avail.size()) d.help.push(diag_.L(Str("あるのは: ") + avail, Str("available: ") + avail));
          return t_.t_unknown();
        }
        resolve_overload(e, cf, cn, args, full);
        return e->type;
      }
    }

    // ふつうのメソッド
    Type* rt = check_expr(obj);
    bool opt_chain = callee->optional_chain;
    if (opt_chain) {
      if (is_optional(rt)) rt = rt->a;
      else {
        Diagnostic& d = diag_.error("E0210", diag_.L(Str("?. を書けるのは T? にだけです（いまは ") + type_name(rt) + "）",
                                                     Str("?. applies to T?, found ") + type_name(rt)));
        d.spans.push(Span(callee->line, callee->col, callee->len));
      }
    } else if (is_optional(rt)) {
      Diagnostic& d = diag_.error("E0201", diag_.L(type_name(rt) + " のメソッドはそのままでは呼べません",
                                                   Str("cannot call a method on ") + type_name(rt)));
      d.spans.push(Span(obj->line, obj->col, obj->len, diag_.L("ここは値が無いかもしれません", "may hold no value")));
      d.help.push(diag_.L(Str("?. を使うと、値が無いときは呼ばずに none になります: ") + mname + " の前を ?. にします\n"
                          "  if var v = ... { } で取り出してから呼ぶこともできます",
                          "use ?. to skip the call when there is no value, or unwrap with if var"));
      rt = rt->a;
    }
    callee->type = rt;
    e->slot2 = 1;
    bool ok = false;
    if (rt && rt->kind == T_Class) {
      ok = resolve_class_method(e, rt, mname, args, false);
      if (!ok) {
        Diagnostic& d = diag_.error("E0137", diag_.L(type_name(rt) + " に " + mname + "() はありません",
                                                     type_name(rt) + " has no method " + mname + "()"));
        d.spans.push(Span(callee->line, callee->col, callee->len));
        Str avail;
        for (ClassInfo* p = rt->cls; p; p = p->base)
          for (int i = 0; i < p->methods.size(); i++) {
            if (avail.size()) avail += ", ";
            avail += p->methods[i].name;
            avail += "()";
          }
        if (avail.size()) d.help.push(diag_.L(Str("あるのは: ") + avail, Str("available: ") + avail));
        e->type = t_.t_unknown();
      }
    } else {
      ok = resolve_builtin_method(e, rt, mname, args);
      if (!ok) {
        Diagnostic& d = diag_.error("E0137", diag_.L(type_name(rt) + " に " + mname + "() はありません",
                                                     type_name(rt) + " has no method " + mname + "()"));
        d.spans.push(Span(callee->line, callee->col, callee->len));
        if (rt && rt->kind == T_Map && mname == "keys")
          d.help.push(diag_.L("keys() は引数なしで呼びます", "keys() takes no arguments"));
        e->type = t_.t_unknown();
      }
    }
    // ?. の先で中身を書き換えることはできない（取り出せるのはコピーだから）
    if (opt_chain && e->resolved2 == 1) {
      Diagnostic& d = diag_.error("E0211", diag_.L(Str("?. の先では ") + mname + "() のように中身を書き換えるメソッドは呼べません",
                                                   Str("cannot call the mutating method ") + mname + "() through ?."));
      d.spans.push(Span(callee->line, callee->col, callee->len));
      Str v = expr_text(obj);
      if (!v.size()) v = Str("xs");
      d.help.push(diag_.L(Str("値があるか先に確かめ、取り出したものを書き換えてから戻します\n") + "if var v = " + v +
                              " { v." + mname + "(...); " + v + " = v; }",
                          Str("check first: if var v = ") + v + " { v." + mname + "(...); " + v + " = v; }"));
      d.help.push(diag_.L(Str("値が必ずあるなら、? を付けずに宣言します"),
                          Str("if the value always exists, declare it without ?")));
      e->resolved2 = 0;
    }
    if (opt_chain && e->type) e->type = t_.optional_of(e->type);
    return e->type ? e->type : t_.t_unknown();
  }

  // --- 式が関数の値 ---
  Type* ct = check_expr(callee);
  if (ct && ct->kind == T_Func) {
    e->opcode = CK_Value;
    if (ct->params.size() != n) {
      Diagnostic& d = diag_.error("E0149", diag_.L("引数の数が違います", "wrong number of arguments"));
      d.spans.push(Span(e->line, e->col, e->len));
    } else {
      for (int i = 0; i < n; i++) need_assign(ct->params[i], args[i]->type, args[i], "引数", "argument");
    }
    return ct->ret;
  }
  if (ct && ct->kind != T_Unknown) {
    Diagnostic& d = diag_.error("E0152", diag_.L(type_name(ct) + " は呼び出せません",
                                                 type_name(ct) + " is not callable"));
    d.spans.push(Span(e->line, e->col, e->len));
  }
  return t_.t_unknown();
}

// ------------------------------------------------------------------ 関数の本体
void Checker::check_func_body(FuncInfo* fi, FuncDecl* fd, Unit* u, ClassInfo* cls) {
  if (!fd || !fd->body) return;
  unit_ = u;
  diag_.set_file(u->display);
  FuncCtx fc;
  fc.fi = fi;
  fc.cls = cls;
  fc.ret = fi->ret;
  for (int i = 0; i < fi->gparams.size(); i++) {
    fc.gnames.push(fi->gparams[i]);
    fc.gtypes.push(fi->gtypes[i]);
  }
  fc_ = &fc;
  push_scope();
  if (fi->is_method && cls) {
    int s = declare_local(Str("this"), t_.class_type(cls), true, 0);
    (void)s;
    fc.locals.back().used = true;
  }
  for (int i = 0; i < fi->params.size(); i++) {
    Node at;
    at.line = fd->params[i].line;
    at.col = fd->params[i].col;
    at.len = fd->params[i].len;
    declare_local(fi->params[i].name, fi->params[i].type, false, &at);
    fc.locals.back().is_ref = fi->params[i].is_ref;
    fc.locals.back().used = true;
  }
  check_block(fd->body);
  pop_scope();
  fi->nlocals = fc.max_slot;
  if (fi->ret && fi->ret->kind != T_Void && fi->ret->kind != T_Unknown && !always_returns(fd->body)) {
    Diagnostic& d = diag_.error("E0153", diag_.L(Str("この関数は ") + type_name(fi->ret) +
                                                     " を返しますが、返さずに終わる道があります",
                                                 Str("this function must return ") + type_name(fi->ret) +
                                                     " on every path"));
    d.spans.push(Span(fd->line, fd->col, fd->name.size() ? (int)fd->name.size() : 4));
    d.help.push(diag_.L("最後に return を書くか、else の側でも return します",
                        "add a return at the end, or in the else branch"));
  }
  fc_ = 0;
}

// ------------------------------------------------------------------ 全体
bool Checker::check_all() {
  for (int i = 0; i < units_.size(); i++) collect_class_bodies(units_[i]);
  for (int i = 0; i < prog_.classes.size(); i++) layout_class(prog_.classes[i]);

  // virtual / override の規則
  for (int ui = 0; ui < units_.size(); ui++) {
    Unit* u = units_[ui];
    unit_ = u;
    diag_.set_file(u->display);
    for (int ci = 0; ci < u->classes.size(); ci++) {
      ClassDecl* cd = u->classes[ci];
      ClassInfo* c = cd->info;
      for (int mi = 0; mi < cd->methods.size(); mi++) {
        FuncDecl* md = cd->methods[mi];
        FuncInfo* f = md->info;
        if (!f || f->is_init) continue;
        bool found_name = false;
        FuncInfo* parent = find_in_bases(prog_, c, f, &found_name);
        if (md->is_override) {
          if (!parent) {
            Diagnostic& d = diag_.error("E0402", diag_.L(Str("override と書かれていますが、親に ") + f->name +
                                                             "() の同じ引数のものがありません",
                                                         Str("override: no matching method ") + f->name +
                                                             "() in a base class"));
            d.spans.push(Span(md->line, md->col, (int)f->name.size()));
            d.help.push(found_name
                            ? diag_.L("引数の型と個数を親と同じにします", "match the parameter types of the parent")
                            : diag_.L("名前の綴りを確かめます。親に virtual を書き忘れていることもあります",
                                      "check the spelling, or add virtual in the parent"));
          } else if (!parent->is_virtual) {
            Diagnostic& d = diag_.error("E0401", diag_.L(parent->owner->name + " の " + f->name +
                                                             "() は virtual ではないので上書きできません",
                                                         parent->owner->name + "." + f->name +
                                                             "() is not virtual"));
            d.spans.push(Span(md->line, md->col, (int)f->name.size()));
            d.help.push(diag_.L(Str("親のメソッドに virtual を付けます: virtual func ") + f->name + "(...)",
                                Str("mark the parent method virtual: virtual func ") + f->name + "(...)"));
          } else if (!type_same(parent->ret, f->ret)) {
            Diagnostic& d = diag_.error("E0408", diag_.L(Str("上書きするメソッドの戻り値は親と同じ型にします（親は ") +
                                                             type_name(parent->ret) + "）",
                                                         Str("an override must return the same type as the parent (") +
                                                             type_name(parent->ret) + ")"));
            d.spans.push(Span(md->line, md->col, (int)f->name.size()));
          }
        } else if (parent) {
          if (parent->is_virtual) {
            Diagnostic& d = diag_.error("E0402", diag_.L(Str("親の virtual な ") + f->name +
                                                             "() を上書きするには override が要ります",
                                                         Str("override is required to override ") + f->name + "()"));
            d.spans.push(Span(md->line, md->col, (int)f->name.size()));
            d.help.push(diag_.L(Str("override func ") + f->name + "(...) と書きます",
                                Str("write: override func ") + f->name + "(...)"));
          } else {
            Diagnostic& d = diag_.error("E0401", diag_.L(parent->owner->name + " の " + f->name +
                                                             "() は virtual ではないので上書きできません",
                                                         parent->owner->name + "." + f->name +
                                                             "() is not virtual"));
            d.spans.push(Span(md->line, md->col, (int)f->name.size()));
            d.help.push(diag_.L("親のメソッドに virtual を付けるか、別の名前にします",
                                "mark the parent method virtual, or use a different name"));
          }
        }
      }
    }
  }

  // 自分自身を値として持つクラスは作れない
  for (int ui = 0; ui < units_.size(); ui++) {
    Unit* u = units_[ui];
    unit_ = u;
    diag_.set_file(u->display);
    for (int ci = 0; ci < u->classes.size(); ci++) {
      ClassDecl* cd = u->classes[ci];
      ClassInfo* c = cd->info;
      int off = c->fields.size() - cd->fields.size();
      for (int k = 0; k < cd->fields.size(); k++) {
        if (off + k < 0 || off + k >= c->fields.size()) continue;
        Type* ft = c->fields[off + k].type;
        if (!ft || ft->kind != T_Class || !ft->cls) continue;
        Vec<ClassInfo*> seen;
        seen.push(ft->cls);
        if (ft->cls != c && !holds_by_value(ft->cls, c, &seen)) continue;
        Diagnostic& d = diag_.error("E0409",
            diag_.L(c->name + " は自分自身を値として持てません（代入がコピーなので、大きさが決まりません）",
                    c->name + " cannot contain itself by value"));
        d.spans.push(Span(cd->fields[k].line, cd->fields[k].col,
                          cd->fields[k].len > 0 ? cd->fields[k].len : (int)cd->fields[k].name.size()));
        d.help.push(diag_.L(Str("? を付けると、値が無い状態を表せます: var ") + cd->fields[k].name + ": " +
                                type_name(ft) + "?;\n  並べて持ちたいときは list<" + type_name(ft) + "> にします",
                            Str("add ? to allow \"no value\": var ") + cd->fields[k].name + ": " +
                                type_name(ft) + "?;"));
      }
    }
  }

  for (int i = 0; i < units_.size(); i++) collect_funcs(units_[i]);
  for (int i = 0; i < units_.size(); i++) collect_globals(units_[i]);

  // 同じ名前・同じ引数の関数が2つある
  for (int i = 0; i < prog_.funcs.size(); i++) {
    FuncInfo* a = prog_.funcs[i];
    if (a->owner || !a->decl) continue;
    for (int k = i + 1; k < prog_.funcs.size(); k++) {
      FuncInfo* b = prog_.funcs[k];
      if (b->owner || !b->decl) continue;
      if (!(a->name == b->name) || !(a->module == b->module)) continue;
      if (a->params.size() != b->params.size()) continue;
      bool same = true;
      for (int m = 0; m < a->params.size(); m++)
        if (!type_same(a->params[m].type, b->params[m].type)) { same = false; break; }
      if (!same) continue;
      Diagnostic& d = diag_.error("E0154", diag_.L(Str("同じ引数の ") + a->name + "() が2つあります",
                                                   Str("duplicate definition of ") + a->name + "()"));
      d.spans.push(Span(b->decl->line, b->decl->col, (int)b->name.size()));
      d.help.push(diag_.L("引数の型か個数を変えると、同じ名前でも定義できます（戻り値だけ違うものは作れません）",
                          "overloads must differ in parameter types or count"));
    }
  }

  // トップレベルの初期化と本体
  for (int ui = 0; ui < units_.size(); ui++) {
    Unit* u = units_[ui];
    unit_ = u;
    diag_.set_file(u->display);

    if (u->globals.size() > 0) {
      FuncInfo* fi = new_func(prog_);
      fi->name = Str("@init");
      fi->module = u->module;
      fi->file = u->display;
      fi->file = u->display;
      fi->ret = t_.t_void();
      FuncCtx fc;
      fc.fi = fi;
      fc.ret = t_.t_void();
      fc_ = &fc;
      push_scope();
      Node* body = arena_.make<Node>();
      body->kind = S_Block;
      for (int gi = 0; gi < u->globals.size(); gi++) {
        GlobalDecl* gd = u->globals[gi];
        GlobalInfo* g = prog_.globals[gd->index];
        if (!gd->init) {
          if (!g->type) {
            err_at("E0112", diag_.L("初期値のない変数には型注釈が要ります",
                                    "a variable without an initializer needs a type"),
                   gd->line, gd->col, (int)gd->name.size());
            g->type = t_.t_unknown();
          }
          continue;
        }
        Type* it = check_expr(gd->init);
        if (!g->type) {
          if (it && ((it->kind == T_Optional && it->a && it->a->kind == T_Unknown) ||
                     (it->kind == T_List && it->a && it->a->kind == T_Unknown) ||
                     (it->kind == T_Map && it->a && it->a->kind == T_Unknown))) {
            err_at("E0104", diag_.L("この初期値だけでは型が決まりません。型を書きます",
                                    "this initializer does not determine the type"),
                   gd->line, gd->col, (int)gd->name.size());
            g->type = t_.t_unknown();
          } else {
            g->type = it;
          }
        } else {
          need_assign(g->type, it, gd->init, "変数", "variable");
        }
        Node* asn = arena_.make<Node>();
        asn->kind = S_Assign;
        asn->line = gd->line;
        asn->col = gd->col;
        asn->len = gd->len;
        Node* tgt = arena_.make<Node>();
        tgt->kind = E_Ident;
        tgt->name = gd->name;
        tgt->line = gd->line;
        tgt->col = gd->col;
        tgt->len = gd->len;
        tgt->is_global = true;
        tgt->slot = g->index;
        tgt->type = g->type;
        asn->a = tgt;
        asn->b = gd->init;
        asn->type = g->type;
        body->list.push(asn);
      }
      pop_scope();
      fi->nlocals = fc.max_slot;
      fi->decl = 0;
      fc_ = 0;
      // コード生成のために本体を持たせる
      FuncDecl* fd = arena_.make<FuncDecl>();
      fd->name = fi->name;
      fd->body = body;
      fd->info = fi;
      fi->decl = fd;
      prog_.inits.push(fi->index);
    }

    for (int ci = 0; ci < u->classes.size(); ci++) {
      ClassDecl* cd = u->classes[ci];
      for (int mi = 0; mi < cd->methods.size(); mi++)
        check_func_body(cd->methods[mi]->info, cd->methods[mi], u, cd->info);
    }
    for (int fi2 = 0; fi2 < u->funcs.size(); fi2++)
      check_func_body(u->funcs[fi2]->info, u->funcs[fi2], u, 0);

    // トップレベルに並べた文
    if (u->top_stmts.size() > 0) {
      if (!u->is_entry) {
        Node* s = u->top_stmts[0];
        Diagnostic& d = diag_.error("E0108", diag_.L("取り込まれるファイルには、トップレベルの文を書けません",
                                                     "an imported module cannot contain top-level statements"));
        d.spans.push(Span(s->line, s->col, s->len));
        d.help.push(diag_.L("関数の中に移します。実行するファイルだけが文を並べられます",
                            "move it into a function"));
      } else if (u->has_main) {
        Node* s = u->top_stmts[0];
        Diagnostic& d = diag_.error("E0107", diag_.L("main があるファイルには、トップレベルの文を書けません",
                                                     "a file with main cannot also have top-level statements"));
        d.spans.push(Span(s->line, s->col, s->len));
        d.help.push(diag_.L("その文を main の中に移します", "move the statement into main"));
      } else {
        FuncInfo* fi = new_func(prog_);
        fi->name = Str("@main");
        fi->module = u->module;
        fi->file = u->display;
        fi->file = u->display;
        fi->ret = t_.t_void();
        Node* body = arena_.make<Node>();
        body->kind = S_Block;
        for (int i = 0; i < u->top_stmts.size(); i++) body->list.push(u->top_stmts[i]);
        FuncDecl* fd = arena_.make<FuncDecl>();
        fd->name = fi->name;
        fd->body = body;
        fd->info = fi;
        fi->decl = fd;
        check_func_body(fi, fd, u, 0);
        prog_.entry = fi->index;
      }
    }
    if (u->is_entry && u->has_main) {
      for (int i = 0; i < prog_.funcs.size(); i++) {
        FuncInfo* f = prog_.funcs[i];
        if (f->owner || !(f->module == u->module) || !(f->name == "main")) continue;
        if (f->params.size() != 0) {
          Diagnostic& d = diag_.error("E0155", diag_.L("main に引数は書けません", "main takes no parameters"));
          d.spans.push(Span(f->decl->line, f->decl->col, 4));
          d.help.push(diag_.L("コマンド引数は std.os の os.args() で受け取ります",
                              "read command line arguments with os.args()"));
        }
        if (f->ret && f->ret->kind != T_Int && f->ret->kind != T_Void) {
          Diagnostic& d = diag_.error("E0155", diag_.L("main は int を返します", "main must return int"));
          d.spans.push(Span(f->decl->line, f->decl->col, 4));
          d.help.push(diag_.L("func main() -> int { ... return 0; }", "func main() -> int { ... return 0; }"));
        }
        prog_.entry = f->index;
        break;
      }
    }
  }
  // 入口が無い（test だけのファイルなど）ことは誤りではない。
  // 実行しようとした側が知らせる（frontend.md）
  return !diag_.has_error();
}

}  // namespace shark

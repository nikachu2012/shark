#include "codegen.h"

namespace shark {

int CodeGen::emit(uint8_t op) {
  int at = f_->code.size();
  f_->code.push(op);
  f_->lines.push((uint32_t)line_);
  return at;
}

void CodeGen::emit_i32(int v) {
  uint32_t u = (uint32_t)v;
  for (int i = 0; i < 4; i++) {
    f_->code.push((uint8_t)((u >> (i * 8)) & 0xff));
    f_->lines.push((uint32_t)line_);
  }
}

int CodeGen::emit_jump(uint8_t op) {
  emit(op);
  int at = f_->code.size();
  emit_i32(0);
  return at;
}

void CodeGen::patch(int at) { patch_to(at, f_->code.size()); }

void CodeGen::patch_to(int at, int target) {
  uint32_t u = (uint32_t)target;
  for (int i = 0; i < 4; i++) f_->code[at + i] = (uint8_t)((u >> (i * 8)) & 0xff);
}

int CodeGen::add_const(Value v) {
  for (int i = 0; i < f_->consts.size(); i++) {
    if (!(f_->consts[i].k == v.k && val_equal(f_->consts[i], v))) continue;
    val_release(v);   // 同じものが既にある。もらった分は捨てる
    return i;
  }
  f_->consts.push(v);
  return f_->consts.size() - 1;
}

void CodeGen::run() {
  for (int i = 0; i < prog_.funcs.size(); i++) {
    FuncInfo* f = prog_.funcs[i];
    if (f->is_native || f->is_pure || !f->decl || !f->decl->body) continue;
    gen_func(f);
  }
}

void CodeGen::gen_func(FuncInfo* f) {
  f_ = f;
  line_ = f->decl->line;
  breaks_.clear();
  continues_.clear();
  gen_stmt(f->decl->body);
  if (f->is_init) {
    // 作った実体をそのまま返す（呼び出し側が受け取る）
    emit(OP_LOAD_LOCAL);
    emit_i32(0);
    emit(OP_RET);
  } else if (f->ret && f->ret->kind != T_Void) {
    // 到達しないはずだが、安全のため
    gen_default(f->ret);
    emit(OP_RET);
  } else {
    emit(OP_RET_VOID);
  }
  f_ = 0;
}

void CodeGen::gen_default(Type* t) {
  if (!t) { emit(OP_VOID); return; }
  switch (t->kind) {
    case T_Int: { emit(OP_CONST); emit_i32(add_const(mk_int(0))); break; }
    case T_Float: { emit(OP_CONST); emit_i32(add_const(mk_float(0.0))); break; }
    case T_Bool: emit(OP_FALSE); break;
    case T_String: { emit(OP_CONST); emit_i32(add_const(mk_str(""))); break; }
    case T_Bytes: { emit(OP_CONST); emit_i32(add_const(mk_bytes(Str()))); break; }
    case T_List: { emit(OP_NEW_LIST); emit_i32(0); break; }
    case T_Map: { emit(OP_NEW_MAP); emit_i32(0); break; }
    case T_Optional: emit(OP_NONE); break;
    case T_Class: {
      int idx = -1;
      for (int i = 0; i < prog_.classes.size(); i++) if (prog_.classes[i] == t->cls) idx = i;
      emit(OP_NEW_INST);
      emit_i32(idx);
      break;
    }
    default: emit(OP_NONE); break;
  }
}

// ------------------------------------------------------------------ 文
void CodeGen::gen_stmt(Node* s) {
  if (!s) return;
  line_ = s->line;
  switch (s->kind) {
    case S_Block:
      for (int i = 0; i < s->list.size(); i++) gen_stmt(s->list[i]);
      break;

    case S_VarDecl:
      if (s->a) gen_expr(s->a);
      else gen_default(s->type);
      emit(OP_STORE_LOCAL);
      emit_i32(s->slot);
      break;

    case S_Assign: {
      if (s->opcode == 1) {  // _ = f();
        gen_expr(s->b);
        emit(OP_POP);   // 式はいつでも値を1つ積む（void も V_Void として積む）
        break;
      }
      Node* tgt = s->a;
      if (s->name.size() == 0) {
        // 単純な代入。右辺を先に作る
        if (tgt->kind == E_Ident) {
          gen_expr(s->b);
          emit(tgt->is_global ? OP_STORE_GLOBAL : OP_STORE_LOCAL);
          emit_i32(tgt->slot);
        } else {
          gen_place(tgt);
          gen_expr(s->b);
          emit(OP_PLACE_STORE);
        }
      } else {
        gen_place(tgt);
        emit(OP_PLACE_DUP);
        emit(OP_PLACE_LOAD);
        gen_expr(s->b);
        Type* ty = s->type;
        uint8_t op = OP_ADD_INT;
        if (ty && ty->kind == T_Float)
          op = s->name == "+" ? OP_ADD_FLOAT : s->name == "-" ? OP_SUB_FLOAT
               : s->name == "*" ? OP_MUL_FLOAT : OP_DIV_FLOAT;
        else if (ty && ty->kind == T_String)
          op = OP_CONCAT;
        else
          op = s->name == "+" ? OP_ADD_INT : s->name == "-" ? OP_SUB_INT
               : s->name == "*" ? OP_MUL_INT : OP_DIV_INT;
        emit(op);
        if (op == OP_CONCAT) emit_i32(2);
        emit(OP_PLACE_STORE);
      }
      break;
    }

    case S_Expr:
      gen_expr(s->a);
      emit(OP_POP);
      break;

    case S_Panic:
      if (s->a) gen_expr(s->a);
      else { emit(OP_CONST); emit_i32(add_const(mk_str("panic"))); }
      emit(OP_PANIC);
      break;

    case S_Return: {
      if (!s->a) {
        if (f_->is_init) {
          emit(OP_LOAD_LOCAL);
          emit_i32(0);
          emit(OP_RET);
        } else {
          emit(OP_RET_VOID);
        }
        break;
      }
      gen_expr(s->a);
      if (s->opcode == 1) emit(OP_MAKE_OK);
      else if (s->opcode == 2) emit(OP_MAKE_ERR);
      emit(OP_RET);
      break;
    }

    case S_If: {
      if (s->bind.size() > 0) {
        gen_expr(s->a);
        bool is_res = s->a->type && s->a->type->kind == T_Result;
        int to_else = emit_jump(is_res ? OP_JUMP_IF_ERR : OP_JUMP_IF_NONE);
        if (is_res) emit(OP_UNWRAP_OK);
        emit(OP_STORE_LOCAL);
        emit_i32(s->slot);
        gen_stmt(s->b);
        int to_end = emit_jump(OP_JUMP);
        patch(to_else);
        if (is_res) {
          if (s->slot2 >= 0) {
            emit(OP_UNWRAP_ERR);
            emit(OP_STORE_LOCAL);
            emit_i32(s->slot2);
          } else {
            emit(OP_POP);
          }
        }
        if (s->c) gen_stmt(s->c);
        patch(to_end);
        break;
      }
      gen_expr(s->a);
      int to_else = emit_jump(OP_JUMP_IF_FALSE);
      gen_stmt(s->b);
      if (s->c) {
        int to_end = emit_jump(OP_JUMP);
        patch(to_else);
        gen_stmt(s->c);
        patch(to_end);
      } else {
        patch(to_else);
      }
      break;
    }

    case S_While: {
      int mark_b = breaks_.size(), mark_c = continues_.size();
      int top = here();
      int to_end;
      if (s->bind.size() > 0) {
        gen_expr(s->a);
        bool is_res = s->a->type && s->a->type->kind == T_Result;
        to_end = emit_jump(is_res ? OP_JUMP_IF_ERR : OP_JUMP_IF_NONE);
        if (is_res) emit(OP_UNWRAP_OK);
        emit(OP_STORE_LOCAL);
        emit_i32(s->slot);
      } else {
        gen_expr(s->a);
        to_end = emit_jump(OP_JUMP_IF_FALSE);
      }
      gen_stmt(s->b);
      int back = emit_jump(OP_JUMP);
      patch_to(back, top);
      patch(to_end);
      // Result の失敗は残っているので捨てる
      if (s->bind.size() > 0 && s->a->type && s->a->type->kind == T_Result) emit(OP_POP);
      for (int i = continues_.size() - 1; i >= mark_c; i--) { patch_to(continues_[i], top); continues_.pop(); }
      for (int i = breaks_.size() - 1; i >= mark_b; i--) { patch(breaks_[i]); breaks_.pop(); }
      break;
    }

    case S_For: {
      int mark_b = breaks_.size(), mark_c = continues_.size();
      gen_expr(s->a);
      emit(OP_ITER_NEW);
      int top = here();
      int to_end = emit_jump(OP_ITER_NEXT);
      emit(OP_STORE_LOCAL);
      emit_i32(s->slot);
      gen_stmt(s->b);
      int back = emit_jump(OP_JUMP);
      patch_to(back, top);
      // break で抜けたときは、回している途中の値がスタックに残っているので捨てる
      int bt = here();
      emit(OP_POP);
      emit(OP_POP);
      patch(to_end);
      for (int i = continues_.size() - 1; i >= mark_c; i--) { patch_to(continues_[i], top); continues_.pop(); }
      for (int i = breaks_.size() - 1; i >= mark_b; i--) { patch_to(breaks_[i], bt); breaks_.pop(); }
      break;
    }

    case S_Break: breaks_.push(emit_jump(OP_JUMP)); break;
    case S_Continue: continues_.push(emit_jump(OP_JUMP)); break;

    default:
      gen_expr(s);
      emit(OP_POP);
      break;
  }
}

// ------------------------------------------------------------------ 代入先と借用
void CodeGen::gen_place(Node* e) {
  switch (e->kind) {
    case E_This:
      emit(OP_PLACE_LOCAL);
      emit_i32(0);
      break;
    case E_Ident:
      emit(e->is_global ? OP_PLACE_GLOBAL : OP_PLACE_LOCAL);
      emit_i32(e->slot);
      break;
    case E_Field:
      gen_place(e->a);
      emit(OP_PLACE_FIELD);
      emit_i32(e->resolved);
      break;
    case E_Index:
      gen_place(e->a);
      gen_expr(e->b);
      emit(OP_PLACE_INDEX);
      break;
    default:
      // ここには来ない（型検査で弾いてある）
      emit(OP_PLACE_LOCAL);
      emit_i32(0);
      break;
  }
}

void CodeGen::gen_ref(Node* e) {
  if (e->kind == E_Ident && !e->is_global) {
    emit(OP_REF_LOCAL);
    emit_i32(e->slot);
    return;
  }
  gen_place(e);
  emit(OP_PLACE_REF);
}

// ------------------------------------------------------------------ 呼び出し
void CodeGen::gen_call(Node* e) {
  Node* callee = e->a;
  bool has_recv = (e->slot2 == 1);
  bool opt = (callee->kind == E_Field && callee->optional_chain);
  int n = e->list.size();

  if (e->opcode == CK_Convert) {  // 同じ型への変換は何もしない
    if (n > 0) gen_expr(e->list[0]);
    return;
  }
  if (e->opcode == CK_Ctor) {
    int idx = -1;
    for (int i = 0; i < prog_.classes.size(); i++) if (prog_.classes[i] == e->rcls) idx = i;
    emit(OP_NEW_INST);
    emit_i32(idx);
    if (e->resolved >= 0) {
      for (int i = 0; i < n; i++) gen_expr(e->list[i]);
      emit(OP_CALL);
      emit_i32(e->resolved);
      emit_i32(n + 1);   // init は this を返す
    }
    return;
  }

  int skip = -1;
  if (has_recv) {
    Node* recv = callee->a;
    bool lvalue = recv->kind == E_Ident || recv->kind == E_Field || recv->kind == E_Index ||
                  recv->kind == E_This || recv->kind == E_Super;
    bool method = (e->opcode == CK_Func || e->opcode == CK_Virtual);
    if (recv->kind == E_This || recv->kind == E_Super) {
      // this は借用で渡す（メソッドの中の書き換えが、呼んだ側の変数に届く）
      if (e->resolved2 == 1 || method) { emit(OP_REF_LOCAL); emit_i32(0); }
      else { emit(OP_LOAD_LOCAL); emit_i32(0); }
    } else if (e->resolved2 == 1 || (method && lvalue && !opt)) {
      gen_ref(recv);
    } else {
      gen_expr(recv);
    }
    if (opt) skip = emit_jump(OP_JUMP_IF_NONE);
  }
  if (e->opcode == CK_Value) gen_expr(callee);
  for (int i = 0; i < n; i++) gen_expr(e->list[i]);
  int total = n + (has_recv ? 1 : 0);
  switch (e->opcode) {
    case CK_Func:
      emit(OP_CALL);
      emit_i32(e->resolved);
      emit_i32(total);
      break;
    case CK_Virtual:
      emit(OP_CALL_VIRTUAL);
      emit_i32(e->resolved2);
      emit_i32(total);
      break;
    case CK_Native:
      emit(OP_CALL_NATIVE);
      emit_i32(e->resolved);
      emit_i32(total);
      break;
    case CK_Value:
      emit(OP_CALL_VALUE);
      emit_i32(n);
      break;
    case CK_CmpDyn:
      emit(OP_CMP_DYN);
      break;
    default:
      emit(OP_NONE);
      break;
  }
  if (opt) {
    int end = emit_jump(OP_JUMP);
    patch(skip);
    emit(OP_NONE);
    patch(end);
  }
}

// ------------------------------------------------------------------ 式
void CodeGen::gen_expr(Node* e) {
  if (!e) { emit(OP_VOID); return; }
  line_ = e->line;
  switch (e->kind) {
    case E_Int: emit(OP_CONST); emit_i32(add_const(mk_int(e->ival))); break;
    case E_Float: emit(OP_CONST); emit_i32(add_const(mk_float(e->dval))); break;
    case E_Bool: emit(e->ival ? OP_TRUE : OP_FALSE); break;
    case E_Str: emit(OP_CONST); emit_i32(add_const(mk_str(e->name))); break;
    case E_Bytes: emit(OP_CONST); emit_i32(add_const(mk_bytes(e->name))); break;
    case E_None: emit(OP_NONE); break;

    case E_Ident:
      if (e->opcode == CK_Func && e->resolved >= 0) {
        emit(OP_CONST);
        emit_i32(add_const(mk_func(e->resolved)));
      } else {
        emit(e->is_global ? OP_LOAD_GLOBAL : OP_LOAD_LOCAL);
        emit_i32(e->slot);
      }
      break;

    case E_This:
    case E_Super:
      emit(OP_LOAD_LOCAL);
      emit_i32(0);
      break;

    case E_ListLit: {
      for (int i = 0; i < e->list.size(); i++) gen_expr(e->list[i]);
      emit(OP_NEW_LIST);
      emit_i32(e->list.size());
      break;
    }
    case E_MapLit: {
      for (int i = 0; i < e->pairs.size(); i++) {
        gen_expr(e->pairs[i].key);
        gen_expr(e->pairs[i].val);
      }
      emit(OP_NEW_MAP);
      emit_i32(e->pairs.size());
      break;
    }

    case E_Field: {
      if (e->opcode == CK_Native && e->resolved >= 0) {  // モジュールの定数
        emit(OP_CALL_NATIVE);
        emit_i32(e->resolved);
        emit_i32(0);
        break;
      }
      if (e->opcode == CK_Func && e->resolved >= 0) {  // 関数を値として
        emit(OP_CONST);
        emit_i32(add_const(mk_func(e->resolved)));
        break;
      }
      if (e->is_global) {
        emit(OP_LOAD_GLOBAL);
        emit_i32(e->slot);
        break;
      }
      gen_expr(e->a);
      if (e->optional_chain) {
        int skip = emit_jump(OP_JUMP_IF_NONE);
        emit(OP_LOAD_FIELD);
        emit_i32(e->resolved);
        int end = emit_jump(OP_JUMP);
        patch(skip);
        emit(OP_NONE);
        patch(end);
      } else {
        emit(OP_LOAD_FIELD);
        emit_i32(e->resolved);
      }
      break;
    }

    case E_Index:
      gen_expr(e->a);
      gen_expr(e->b);
      emit(OP_INDEX_GET);
      break;

    case E_Call: gen_call(e); break;

    case E_Unary:
      gen_expr(e->a);
      emit((uint8_t)e->opcode);
      break;

    case E_Binary: {
      if (e->name == "&&") {
        gen_expr(e->a);
        int end = emit_jump(OP_JUMP_IF_FALSE_KEEP);
        emit(OP_POP);
        gen_expr(e->b);
        patch(end);
        break;
      }
      if (e->name == "||") {
        gen_expr(e->a);
        int end = emit_jump(OP_JUMP_IF_TRUE_KEEP);
        emit(OP_POP);
        gen_expr(e->b);
        patch(end);
        break;
      }
      if (e->name == "??") {
        gen_expr(e->a);
        if (e->a->type && e->a->type->kind == T_Result) {
          int to_err = emit_jump(OP_JUMP_IF_ERR);
          emit(OP_UNWRAP_OK);
          int end = emit_jump(OP_JUMP);
          patch(to_err);
          emit(OP_POP);
          gen_expr(e->b);
          patch(end);
        } else {
          int end = emit_jump(OP_JUMP_IF_NOT_NONE);
          gen_expr(e->b);
          patch(end);
        }
        break;
      }
      gen_expr(e->a);
      gen_expr(e->b);
      emit((uint8_t)e->opcode);
      if (e->opcode == OP_CONCAT) emit_i32(2);
      break;
    }

    case E_FStr: {
      int n = 0;
      int fmt_id = reg_.find("__fmt");
      for (int i = 0; i < e->parts.size(); i++) {
        if (!e->parts[i].is_expr) {
          emit(OP_CONST);
          emit_i32(add_const(mk_str(e->parts[i].text)));
        } else {
          gen_expr(e->parts[i].expr);
          emit(OP_CONST);
          emit_i32(add_const(mk_str(e->parts[i].spec)));
          emit(OP_CALL_NATIVE);
          emit_i32(fmt_id);
          emit_i32(2);
        }
        n++;
      }
      if (n == 0) { emit(OP_CONST); emit_i32(add_const(mk_str(""))); break; }
      if (n > 1) { emit(OP_CONCAT); emit_i32(n); }
      break;
    }

    case E_Force:
      gen_expr(e->a);
      emit(OP_FORCE);
      break;

    case E_Try: {
      gen_expr(e->a);
      int to_err = emit_jump(OP_JUMP_IF_ERR);
      emit(OP_UNWRAP_OK);
      int end = emit_jump(OP_JUMP);
      patch(to_err);
      emit(OP_RET);   // 失敗はそのまま呼び出し元へ返す
      patch(end);
      break;
    }

    case E_Task: {
      Node* call = e->a;
      for (int i = 0; i < call->list.size(); i++) gen_expr(call->list[i]);
      emit(OP_TASK);
      emit_i32(call->resolved);
      emit_i32(call->list.size());
      break;
    }

    case E_Parallel: {
      for (int i = 0; i < e->list.size(); i++) gen_expr(e->list[i]);
      emit(OP_PARALLEL);
      emit_i32(e->list.size());
      break;
    }

    case E_Ref:
      gen_ref(e->a);
      break;

    case S_Panic:
      if (e->a) gen_expr(e->a);
      else { emit(OP_CONST); emit_i32(add_const(mk_str("panic"))); }
      emit(OP_PANIC);
      break;

    default:
      emit(OP_NONE);
      break;
  }
}

}  // namespace shark

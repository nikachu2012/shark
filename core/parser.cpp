#include "parser.h"

namespace shark {

Parser::Parser(const Str& src, const Str& module, Arena& arena, DiagBag& diag)
    : p_(0), arena_(arena), diag_(diag), unit_(0), panic_depth_(0) {
  Lexer lex(src, diag);
  lex.run(&toks_);
  unit_ = arena_.make<Unit>();
  unit_->module = module;
}

Node* Parser::node(NodeKind k) { return node_at(k, cur()); }

Node* Parser::node_at(NodeKind k, const Token& t) {
  Node* n = arena_.make<Node>();
  n->kind = k;
  n->line = t.line;
  n->col = t.col;
  n->len = t.len > 0 ? t.len : 1;
  return n;
}

void Parser::error_here(const Str& msg, const Str& help) {
  if (panic_depth_ > 0) return;  // 同じ原因の派生は出さない
  Diagnostic& d = diag_.error("E0001", msg);
  d.spans.push(Span(cur().line, cur().col, cur().len > 0 ? cur().len : 1));
  if (help.size()) d.help.push(help);
  panic_depth_ = 1;
}

bool Parser::expect(TokKind k, const char* what_ja, const char* what_en) {
  if (eat(k)) { panic_depth_ = 0; return true; }
  error_here(diag_.L(what_ja, what_en),
             diag_.L(Str("ここに ") + tok_name(k) + " が要ります", Str("expected ") + tok_name(k)));
  return false;
}

void Parser::sync_to_statement() {
  // 誤りの後、次の文の頭まで読み飛ばす
  int depth = 0;
  while (!at(TK_EOF)) {
    if (at(TK_LBrace)) depth++;
    if (at(TK_RBrace)) { if (depth == 0) return; depth--; }
    if (at(TK_Semi) && depth == 0) { p_++; panic_depth_ = 0; return; }
    if (depth == 0 && (at(TK_Func) || at(TK_Class) || at(TK_Var) || at(TK_Const) ||
                       at(TK_If) || at(TK_While) || at(TK_For) || at(TK_Return))) {
      panic_depth_ = 0;
      return;
    }
    p_++;
  }
}

// ------------------------------------------------------------------ 型
TypeExpr* Parser::parse_type() {
  TypeExpr* t = arena_.make<TypeExpr>();
  t->line = cur().line; t->col = cur().col; t->len = cur().len;
  if (at(TK_Func)) {
    p_++;
    t->name = Str("func");
    expect(TK_LParen, "func 型は func(型, ...) と書きます", "function type needs (");
    if (!at(TK_RParen)) {
      do { t->fn_params.push(parse_type()); } while (eat(TK_Comma));
    }
    expect(TK_RParen, ") が要ります", "expected )");
    if (eat(TK_Arrow)) t->fn_ret = parse_type();
    return t;
  }
  if (at(TK_Ident) || at(TK_ThisType) ||
      (cur().kind >= TK_Func && cur().kind <= TK_None && cur().text.size() > 0)) {
    t->name = cur().text;
    p_++;
    while (at(TK_Dot) && peek(1).kind == TK_Ident) {
      p_++;
      t->name += ".";
      t->name += cur().text;
      p_++;
    }
  } else {
    error_here(diag_.L("型の名前がここに要ります", "expected a type name"),
               diag_.L("int float bool string などの型名か、クラス名を書きます",
                       "write a type such as int, float, string, or a class name"));
    t->name = Str("?");
    return t;
  }
  if (at(TK_Lt)) {
    p_++;
    do { t->args.push(parse_type()); } while (eat(TK_Comma));
    expect(TK_Gt, "型引数は > で閉じます", "expected > to close type arguments");
  }
  if (eat(TK_Question)) t->optional = true;
  return t;
}

// `<int, string>` を読めたら true。読めなければ位置を戻す
bool Parser::try_parse_type_args(Vec<TypeExpr*>* out) {
  int save = p_;
  int save_errs = diag_.size();
  if (!eat(TK_Lt)) return false;
  Vec<TypeExpr*> args;
  do {
    if (!(at(TK_Ident) || at(TK_ThisType) || at(TK_Func))) { p_ = save; return false; }
    args.push(parse_type());
  } while (eat(TK_Comma));
  if (!eat(TK_Gt)) { p_ = save; while (diag_.size() > save_errs) diag_.items().pop(); return false; }
  if (!at(TK_LParen)) { p_ = save; while (diag_.size() > save_errs) diag_.items().pop(); return false; }
  while (diag_.size() > save_errs) diag_.items().pop();
  *out = args;
  return true;
}

void Parser::parse_generic_params(Vec<GenericParam>* out) {
  if (!eat(TK_Lt)) return;
  do {
    GenericParam g;
    if (at(TK_Ident)) { g.name = cur().text; p_++; }
    else { error_here(diag_.L("型引数の名前が要ります", "expected a type parameter name"), Str()); p_++; }
    if (eat(TK_Colon)) {
      do {
        if (at(TK_Ident)) { g.constraints.push(cur().text); p_++; }
        else { error_here(diag_.L("制約にはインタフェース名を書きます", "expected an interface name"), Str()); p_++; }
      } while (eat(TK_Plus));
    }
    out->push(g);
  } while (eat(TK_Comma));
  expect(TK_Gt, "型引数は > で閉じます", "expected > to close type parameters");
}

// ------------------------------------------------------------------ 宣言
void Parser::parse_import() {
  ImportDecl im;
  im.line = cur().line; im.col = cur().col;
  p_++;  // import
  Str path;
  int start_col = cur().col;
  // ./ ../ の並び
  while (at(TK_Dot) || at(TK_Slash)) { path += (at(TK_Dot) ? "." : "/"); p_++; }
  // std.task のように、予約語と同じ綴りの区切りも受け取る
  bool segment = at(TK_Ident) || (cur().kind >= TK_Func && cur().kind <= TK_None && cur().text.size() > 0);
  if (!segment) {
    error_here(diag_.L("import の後ろにモジュール名が要ります", "expected a module name after import"),
               diag_.L("例: import std.time;  /  import ./util;", "example: import std.time;"));
    sync_to_statement();
    return;
  }
  path += cur().text;
  p_++;
  while (at(TK_Dot) || at(TK_Slash)) {
    path += (at(TK_Dot) ? "." : "/");
    p_++;
    if (at(TK_Dot) || at(TK_Slash)) continue;
    if (!(at(TK_Ident) || (cur().kind >= TK_Func && cur().kind <= TK_None && cur().text.size() > 0))) break;
    path += cur().text;
    p_++;
  }
  im.path = path;
  im.len = cur().col - start_col;
  if (im.len < 1) im.len = (int)path.size();
  im.col = start_col;
  if (eat(TK_As)) {
    if (at(TK_Ident)) { im.alias = cur().text; p_++; }
    else error_here(diag_.L("as の後ろに別名が要ります", "expected an alias after as"), Str());
  }
  expect(TK_Semi, "import の終わりに ; が要ります", "expected ; after import");
  unit_->imports.push(im);
}

FuncDecl* Parser::parse_func(bool is_public, bool is_virtual, bool is_override, bool in_class) {
  FuncDecl* f = arena_.make<FuncDecl>();
  f->is_public = is_public;
  f->is_virtual = is_virtual;
  f->is_override = is_override;
  f->line = cur().line; f->col = cur().col;
  p_++;  // func
  if (at(TK_Ident)) {
    // 位置は関数名に合わせる（下線が名前に付く）
    f->line = cur().line;
    f->col = cur().col;
    f->name = cur().text;
    f->len = cur().len;
    p_++;
  }
  else {
    error_here(diag_.L("func の後ろに関数名が要ります", "expected a function name"),
               diag_.L("例: func add(a: int, b: int) -> int { }", "example: func add(a: int) -> int { }"));
  }
  if (at(TK_Lt)) parse_generic_params(&f->gparams);
  expect(TK_LParen, "引数は ( で始めます", "expected ( for the parameter list");
  if (!at(TK_RParen)) {
    do {
      ParamDecl pd;
      pd.line = cur().line; pd.col = cur().col; pd.len = cur().len;
      if (eat(TK_Ref)) pd.is_ref = true;
      if (at(TK_Ident)) { pd.name = cur().text; p_++; }
      else { error_here(diag_.L("引数の名前が要ります", "expected a parameter name"), Str()); p_++; }
      if (!expect(TK_Colon, "引数には型注釈が要ります", "a parameter needs a type annotation")) break;
      pd.type = parse_type();
      f->params.push(pd);
    } while (eat(TK_Comma));
  }
  expect(TK_RParen, "引数は ) で閉じます", "expected ) to close the parameter list");
  if (eat(TK_Arrow)) f->ret = parse_type();
  if (in_class && at(TK_Semi)) { p_++; return f; }  // 純粋仮想
  f->body = parse_block();
  return f;
}

ClassDecl* Parser::parse_class(bool is_public) {
  ClassDecl* c = arena_.make<ClassDecl>();
  c->is_public = is_public;
  c->line = cur().line; c->col = cur().col;
  p_++;  // class
  if (at(TK_Ident)) { c->name = cur().text; c->len = cur().len; p_++; }
  else error_here(diag_.L("class の後ろにクラス名が要ります", "expected a class name"), Str());
  if (at(TK_Lt)) parse_generic_params(&c->gparams);
  if (eat(TK_Colon)) {
    do {
      if (at(TK_Ident)) {
        c->bases.push(cur().text);
        c->base_lines.push(cur().line);
        c->base_cols.push(cur().col);
        c->base_lens.push(cur().len);
        p_++;
      } else {
        error_here(diag_.L("親クラスかインタフェースの名前が要ります", "expected a base class name"), Str());
        p_++;
      }
    } while (eat(TK_Comma));
  }
  expect(TK_LBrace, "クラスの中身は { で始めます", "expected { to open the class body");
  while (!at(TK_RBrace) && !at(TK_EOF)) {
    bool mpub = false, mvirt = false, movr = false;
    for (;;) {
      if (eat(TK_Public)) { mpub = true; continue; }
      if (eat(TK_Private)) { mpub = false; continue; }
      if (eat(TK_Virtual)) { mvirt = true; continue; }
      if (eat(TK_Override)) { movr = true; continue; }
      break;
    }
    if (at(TK_Var)) {
      FieldDecl fd;
      fd.is_public = mpub;
      fd.line = cur().line; fd.col = cur().col;
      p_++;
      if (at(TK_Ident)) {
        fd.line = cur().line;
        fd.col = cur().col;
        fd.name = cur().text;
        fd.len = cur().len;
        p_++;
      }
      else { error_here(diag_.L("メンバ変数の名前が要ります", "expected a field name"), Str()); p_++; }
      if (expect(TK_Colon, "メンバ変数には型注釈が要ります（省略できません）",
                 "a field needs a type annotation")) {
        fd.type = parse_type();
      }
      expect(TK_Semi, "メンバ変数の終わりに ; が要ります", "expected ; after the field");
      c->fields.push(fd);
      continue;
    }
    if (at(TK_Func)) {
      FuncDecl* m = parse_func(mpub, mvirt, movr, true);
      c->methods.push(m);
      continue;
    }
    error_here(diag_.L("クラスの中に書けるのは var と func だけです", "only var and func may appear in a class"),
               diag_.L("メンバ変数は var name: 型; 、メソッドは func name() { } と書きます",
                       "fields: var name: T;  methods: func name() { }"));
    sync_to_statement();
    if (at(TK_Semi)) p_++;
    if (!at(TK_RBrace) && !at(TK_Func) && !at(TK_Var)) p_++;
  }
  expect(TK_RBrace, "クラスは } で閉じます", "expected } to close the class");
  return c;
}

GlobalDecl* Parser::parse_global(bool is_public, bool is_const) {
  GlobalDecl* g = arena_.make<GlobalDecl>();
  g->is_public = is_public;
  g->is_const = is_const;
  g->line = cur().line; g->col = cur().col;
  p_++;  // var / const
  if (at(TK_Ident)) { g->name = cur().text; g->len = cur().len; p_++; }
  else error_here(diag_.L("変数の名前が要ります", "expected a variable name"), Str());
  if (eat(TK_Colon)) g->type = parse_type();
  if (eat(TK_Assign)) g->init = parse_expr();
  expect(TK_Semi, "宣言の終わりに ; が要ります", "expected ; after the declaration");
  return g;
}

// ------------------------------------------------------------------ 文
Node* Parser::parse_block() {
  Node* b = node(S_Block);
  if (!expect(TK_LBrace, "ここは { で始めます（1文でも { } は省略できません）",
              "expected { (braces are required even for a single statement)"))
    return b;
  while (!at(TK_RBrace) && !at(TK_EOF)) {
    Node* s = parse_statement();
    if (s) b->list.push(s);
  }
  expect(TK_RBrace, "} が要ります", "expected }");
  return b;
}

Node* Parser::parse_var_stmt(bool is_const) {
  Node* n = node(is_const ? S_VarDecl : S_VarDecl);
  n->is_const = is_const;
  p_++;  // var / const
  if (at(TK_Ident)) { n->name = cur().text; n->len = cur().len; n->col = cur().col; p_++; }
  else error_here(diag_.L("変数の名前が要ります", "expected a variable name"), Str());
  if (eat(TK_Colon)) n->tann = parse_type();
  if (eat(TK_Assign)) n->a = parse_expr();
  expect(TK_Semi, "文の終わりに ; が要ります", "expected ; at the end of the statement");
  return n;
}

Node* Parser::parse_if() {
  Node* n = node(S_If);
  p_++;  // if
  if (at(TK_Var)) {
    p_++;
    if (at(TK_Ident)) { n->bind = cur().text; n->col = cur().col; n->len = cur().len; p_++; }
    else error_here(diag_.L("if var の後ろに変数名が要ります", "expected a name after if var"), Str());
    expect(TK_Assign, "if var 名 = 式 と書きます", "expected = in if var");
    n->a = parse_expr();
  } else {
    n->a = parse_expr();
  }
  n->b = parse_block();
  if (at(TK_Else)) {
    p_++;
    if (at(TK_If)) {
      n->c = parse_if();
    } else {
      if (at(TK_Var)) {
        p_++;
        if (at(TK_Ident)) { n->bind2 = cur().text; p_++; }
        else error_here(diag_.L("else var の後ろに変数名が要ります", "expected a name after else var"), Str());
      }
      n->c = parse_block();
    }
  }
  return n;
}

Node* Parser::parse_while() {
  Node* n = node(S_While);
  p_++;
  if (at(TK_Var)) {
    p_++;
    if (at(TK_Ident)) { n->bind = cur().text; n->col = cur().col; n->len = cur().len; p_++; }
    else error_here(diag_.L("while var の後ろに変数名が要ります", "expected a name after while var"), Str());
    expect(TK_Assign, "while var 名 = 式 と書きます", "expected = in while var");
    n->a = parse_expr();
  } else {
    n->a = parse_expr();
  }
  n->b = parse_block();
  return n;
}

Node* Parser::parse_for() {
  Node* n = node(S_For);
  p_++;  // for
  if (!eat(TK_Var)) {
    error_here(diag_.L("for var 名 in 式 { } と書きます", "write: for var x in xs { }"),
               diag_.L("繰り返しの変数には var が要ります", "the loop variable needs var"));
  }
  if (at(TK_Ident)) { n->bind = cur().text; n->col = cur().col; n->len = cur().len; p_++; }
  else error_here(diag_.L("繰り返しの変数名が要ります", "expected a loop variable name"), Str());
  expect(TK_In, "for var 名 in 式 と書きます", "expected in");
  n->a = parse_expr();
  n->b = parse_block();
  return n;
}

Node* Parser::parse_statement() {
  switch (cur().kind) {
    case TK_Var: return parse_var_stmt(false);
    case TK_Const: return parse_var_stmt(true);
    case TK_If: return parse_if();
    case TK_While: return parse_while();
    case TK_For: return parse_for();
    case TK_LBrace: return parse_block();
    case TK_Break: {
      Node* n = node(S_Break);
      p_++;
      expect(TK_Semi, "break の後ろに ; が要ります", "expected ; after break");
      return n;
    }
    case TK_Continue: {
      Node* n = node(S_Continue);
      p_++;
      expect(TK_Semi, "continue の後ろに ; が要ります", "expected ; after continue");
      return n;
    }
    case TK_Return: {
      Node* n = node(S_Return);
      p_++;
      if (!at(TK_Semi)) n->a = parse_expr();
      expect(TK_Semi, "return の後ろに ; が要ります", "expected ; after return");
      return n;
    }
    case TK_Func: {
      error_here(diag_.L("関数の中に関数は定義できません", "functions cannot be nested"),
                 diag_.L("トップレベル（ファイルの一番外側）に書きます", "define it at the top level"));
      FuncDecl* f = parse_func(false, false, false, false);
      (void)f;
      return 0;
    }
    case TK_Class: {
      error_here(diag_.L("関数の中にクラスは定義できません", "classes cannot be nested"),
                 diag_.L("トップレベル（ファイルの一番外側）に書きます", "define it at the top level"));
      parse_class(false);
      return 0;
    }
    default: break;
  }
  // 代入か式文
  Node* e = parse_expr();
  if (at(TK_Assign) || at(TK_PlusAssign) || at(TK_MinusAssign) || at(TK_StarAssign) ||
      at(TK_SlashAssign) || at(TK_AmpAssign) || at(TK_PipeAssign) || at(TK_CaretAssign) ||
      at(TK_ShlAssign) || at_shr_assign()) {
    Node* n = node_at(S_Assign, cur());
    n->line = e->line; n->col = e->col;
    bool shr_assign = at_shr_assign();
    switch (cur().kind) {
      case TK_PlusAssign: n->name = Str("+"); break;
      case TK_MinusAssign: n->name = Str("-"); break;
      case TK_StarAssign: n->name = Str("*"); break;
      case TK_SlashAssign: n->name = Str("/"); break;
      case TK_AmpAssign: n->name = Str("&"); break;
      case TK_PipeAssign: n->name = Str("|"); break;
      case TK_CaretAssign: n->name = Str("^"); break;
      case TK_ShlAssign: n->name = Str("<<"); break;
      default: n->name = Str(shr_assign ? ">>" : ""); break;
    }
    p_ += shr_assign ? 1 : 0;   // >>= は > と >= の2つに分かれている
    p_++;
    n->a = e;
    n->b = parse_expr();
    expect(TK_Semi, "文の終わりに ; が要ります", "expected ; at the end of the statement");
    return n;
  }
  Node* n = node_at(S_Expr, cur());
  n->line = e->line; n->col = e->col; n->len = e->len;
  n->a = e;
  expect(TK_Semi, "文の終わりに ; が要ります", "expected ; at the end of the statement");
  return n;
}

// ------------------------------------------------------------------ 式
Node* Parser::parse_coalesce() {
  Node* l = parse_or();
  if (at(TK_QQ)) {
    Node* n = node(E_Binary);
    n->name = Str("??");
    n->line = l->line; n->col = l->col;
    p_++;
    n->a = l;
    n->b = parse_coalesce();  // 右結合
    return n;
  }
  return l;
}

Node* Parser::parse_or() {
  Node* l = parse_and();
  while (at(TK_OrOr)) {
    Node* n = node(E_Binary);
    n->name = Str("||");
    n->line = l->line; n->col = l->col;
    p_++;
    n->a = l;
    n->b = parse_and();
    l = n;
  }
  return l;
}

Node* Parser::parse_and() {
  Node* l = parse_bit_or();
  while (at(TK_AndAnd)) {
    Node* n = node(E_Binary);
    n->name = Str("&&");
    n->line = l->line; n->col = l->col;
    p_++;
    n->a = l;
    n->b = parse_bit_or();
    l = n;
  }
  return l;
}

// ビット演算の強さは C と同じ。| より ^、^ より & が強く、
// どれも == や != より弱い（a & b == c は a & (b == c) になる）
Node* Parser::parse_bit_or() {
  Node* l = parse_bit_xor();
  while (at(TK_Pipe)) {
    Node* n = node(E_Binary);
    n->name = Str("|");
    n->line = cur().line; n->col = cur().col; n->len = 1;
    p_++;
    n->a = l;
    n->b = parse_bit_xor();
    l = n;
  }
  return l;
}

Node* Parser::parse_bit_xor() {
  Node* l = parse_bit_and();
  while (at(TK_Caret)) {
    Node* n = node(E_Binary);
    n->name = Str("^");
    n->line = cur().line; n->col = cur().col; n->len = 1;
    p_++;
    n->a = l;
    n->b = parse_bit_and();
    l = n;
  }
  return l;
}

Node* Parser::parse_bit_and() {
  Node* l = parse_equality();
  while (at(TK_Amp)) {
    Node* n = node(E_Binary);
    n->name = Str("&");
    n->line = cur().line; n->col = cur().col; n->len = 1;
    p_++;
    n->a = l;
    n->b = parse_equality();
    l = n;
  }
  return l;
}

Node* Parser::parse_equality() {
  Node* l = parse_relational();
  while (at(TK_Eq) || at(TK_Ne)) {
    Node* n = node(E_Binary);
    n->name = Str(at(TK_Eq) ? "==" : "!=");
    n->line = l->line; n->col = l->col;
    p_++;
    n->a = l;
    n->b = parse_relational();
    l = n;
  }
  return l;
}

// > が2つ並んでいれば >>（list<list<int>> と紛れないよう、記号にはしていない）
bool Parser::at_shr() const {
  return at(TK_Gt) && peek(1).kind == TK_Gt && peek(1).offset == cur().offset + 1;
}
bool Parser::at_shr_assign() const {
  return at(TK_Gt) && peek(1).kind == TK_Ge && peek(1).offset == cur().offset + 1;
}

Node* Parser::parse_relational() {
  Node* l = parse_shift();
  // >> と >>= は比較ではない
  while (!at_shr() && !at_shr_assign() && (at(TK_Lt) || at(TK_Le) || at(TK_Gt) || at(TK_Ge))) {
    Node* n = node(E_Binary);
    n->name = Str(at(TK_Lt) ? "<" : at(TK_Le) ? "<=" : at(TK_Gt) ? ">" : ">=");
    n->line = l->line; n->col = l->col;
    p_++;
    n->a = l;
    n->b = parse_shift();
    l = n;
  }
  return l;
}

Node* Parser::parse_shift() {
  Node* l = parse_additive();
  for (;;) {
    bool shr = at_shr();
    if (!shr && !at(TK_Shl)) break;
    Node* n = node(E_Binary);
    n->name = Str(shr ? ">>" : "<<");
    n->line = cur().line; n->col = cur().col; n->len = 2;
    p_ += shr ? 2 : 1;
    n->a = l;
    n->b = parse_additive();
    l = n;
  }
  return l;
}

Node* Parser::parse_additive() {
  Node* l = parse_multiplicative();
  while (at(TK_Plus) || at(TK_Minus)) {
    Node* n = node(E_Binary);
    n->name = Str(at(TK_Plus) ? "+" : "-");
    n->line = cur().line; n->col = cur().col; n->len = 1;
    p_++;
    n->a = l;
    n->b = parse_multiplicative();
    l = n;
  }
  return l;
}

Node* Parser::parse_multiplicative() {
  Node* l = parse_unary();
  while (at(TK_Star) || at(TK_Slash) || at(TK_Percent)) {
    Node* n = node(E_Binary);
    n->name = Str(at(TK_Star) ? "*" : at(TK_Slash) ? "/" : "%");
    n->line = cur().line; n->col = cur().col; n->len = 1;
    p_++;
    n->a = l;
    n->b = parse_unary();
    l = n;
  }
  return l;
}

Node* Parser::parse_unary() {
  if (at(TK_Bang) || at(TK_Minus) || at(TK_Tilde)) {
    Node* n = node(E_Unary);
    n->name = Str(at(TK_Bang) ? "!" : at(TK_Minus) ? "-" : "~");
    p_++;
    n->a = parse_unary();
    return n;
  }
  if (at(TK_Try)) {
    Node* n = node(E_Try);
    p_++;
    n->a = parse_unary();
    return n;
  }
  // task.yield() のように後ろが . なら、モジュールの呼び出しとして読む
  if (at(TK_Task) && peek(1).kind != TK_Dot) {
    Node* n = node(E_Task);
    p_++;
    n->a = parse_unary();
    return n;
  }
  if (at(TK_Ref)) {
    Node* n = node(E_Ref);
    p_++;
    n->a = parse_unary();
    return n;
  }
  return parse_power();
}

// 冪乗は右結合で、単項より強く結び付く（-2 ** 2 は -(2 ** 2)）。
// 右側は単項から読むので、2.0 ** -1.0 のようにも書ける
Node* Parser::parse_power() {
  Node* l = parse_postfix();
  if (at(TK_Star2)) {
    Node* n = node(E_Binary);
    n->name = Str("**");
    n->line = cur().line; n->col = cur().col; n->len = 2;
    p_++;
    n->a = l;
    n->b = parse_unary();
    return n;
  }
  return l;
}

void Parser::parse_args(Node* call) {
  p_++;  // (
  if (!at(TK_RParen)) {
    do { call->list.push(parse_expr()); } while (eat(TK_Comma));
  }
  expect(TK_RParen, "呼び出しは ) で閉じます", "expected ) to close the call");
}

Node* Parser::parse_postfix() {
  Node* e = parse_primary();
  for (;;) {
    if (at(TK_Dot) || at(TK_QDot)) {
      bool opt = at(TK_QDot);
      Node* n = node(E_Field);
      n->line = cur().line; n->col = cur().col;
      n->optional_chain = opt;
      p_++;
      if (at(TK_Ident) || (cur().text.size() > 0 && cur().kind != TK_EOF)) {
        n->name = cur().text;
        n->len = cur().len;
        p_++;
      } else {
        error_here(diag_.L(". の後ろにメンバ名が要ります", "expected a member name after ."), Str());
      }
      n->a = e;
      e = n;
      continue;
    }
    if (at(TK_LParen)) {
      Node* n = node(E_Call);
      n->line = e->line; n->col = e->col; n->len = e->len;
      n->a = e;
      parse_args(n);
      e = n;
      continue;
    }
    if (at(TK_Lt) && (e->kind == E_Ident || e->kind == E_Field)) {
      Vec<TypeExpr*> args;
      if (try_parse_type_args(&args)) {
        Node* n = node(E_Call);
        n->line = e->line; n->col = e->col; n->len = e->len;
        n->a = e;
        n->targs = args;
        parse_args(n);
        e = n;
        continue;
      }
    }
    if (at(TK_LBracket)) {
      Node* n = node(E_Index);
      n->line = e->line; n->col = e->col;
      p_++;
      n->a = e;
      n->b = parse_expr();
      expect(TK_RBracket, "添字は ] で閉じます", "expected ] to close the index");
      e = n;
      continue;
    }
    if (at(TK_Bang)) {
      Node* n = node(E_Force);
      n->line = cur().line; n->col = cur().col; n->len = 1;
      p_++;
      n->a = e;
      e = n;
      continue;
    }
    break;
  }
  return e;
}

// f"..." を、文字の部分と式の部分に分ける
static bool fstr_unescape(const Str& src, int from, int to, Str* out) {
  for (int i = from; i < to; i++) {
    char c = src[i];
    if (c != '\\') { out->push(c); continue; }
    i++;
    if (i >= to) return false;
    char e = src[i];
    switch (e) {
      case 'n': out->push('\n'); break;
      case 't': out->push('\t'); break;
      case 'r': out->push('\r'); break;
      case '0': out->push('\0'); break;
      case '\\': out->push('\\'); break;
      case '"': out->push('"'); break;
      case 'x': {
        int v = 0;
        for (int k = 0; k < 2 && i + 1 < to; k++) {
          char h = src[++i];
          v = v * 16 + (h <= '9' ? h - '0' : (h <= 'F' ? h - 'A' + 10 : h - 'a' + 10));
        }
        out->push((char)v);
        break;
      }
      case 'u': {
        if (i + 1 < to && src[i + 1] == '{') {
          i += 2;
          int v = 0;
          while (i < to && src[i] != '}') {
            char h = src[i];
            v = v * 16 + (h <= '9' ? h - '0' : (h <= 'F' ? h - 'A' + 10 : h - 'a' + 10));
            i++;
          }
          if (v < 0x80) out->push((char)v);
          else if (v < 0x800) { out->push((char)(0xC0 | (v >> 6))); out->push((char)(0x80 | (v & 0x3F))); }
          else if (v < 0x10000) {
            out->push((char)(0xE0 | (v >> 12)));
            out->push((char)(0x80 | ((v >> 6) & 0x3F)));
            out->push((char)(0x80 | (v & 0x3F)));
          } else {
            out->push((char)(0xF0 | (v >> 18)));
            out->push((char)(0x80 | ((v >> 12) & 0x3F)));
            out->push((char)(0x80 | ((v >> 6) & 0x3F)));
            out->push((char)(0x80 | (v & 0x3F)));
          }
        }
        break;
      }
      default: out->push(e); break;
    }
  }
  return true;
}

Node* Parser::parse_fstring(const Token& t) {
  Node* n = node_at(E_FStr, t);
  const Str& src = t.text;
  int i = 0;
  Str lit;
  while (i < src.size()) {
    char c = src[i];
    if (c == '{' && i + 1 < src.size() && src[i + 1] == '{') { lit.push('{'); i += 2; continue; }
    if (c == '}' && i + 1 < src.size() && src[i + 1] == '}') { lit.push('}'); i += 2; continue; }
    if (c == '{') {
      if (lit.size()) {
        FStrPart p;
        p.is_expr = false;
        fstr_unescape(lit, 0, lit.size(), &p.text);
        n->parts.push(p);
        lit.clear();
      }
      int depth = 1;
      int j = i + 1;
      int colon = -1;
      bool in_str = false;
      while (j < src.size() && depth > 0) {
        char d = src[j];
        if (in_str) {
          if (d == '\\') j++;
          else if (d == '"') in_str = false;
        } else if (d == '"') in_str = true;
        else if (d == '{' || d == '[' || d == '(') depth++;
        else if (d == ')' || d == ']') depth--;
        else if (d == '}') { depth--; if (depth == 0) break; }
        else if (d == ':' && depth == 1 && colon < 0) colon = j;
        j++;
      }
      if (j >= src.size()) {
        Diagnostic& d = diag_.error("E0001", diag_.L("f\"...\" の { が閉じていません",
                                                     "unclosed { in an f-string"));
        d.spans.push(Span(t.line, t.col, t.len));
        d.help.push(diag_.L("{ と } で式を囲みます。文字としての { は {{ と書きます",
                            "write {{ for a literal brace"));
        break;
      }
      int expr_end = colon >= 0 ? colon : j;
      Str expr_src = src.sub(i + 1, expr_end - i - 1);
      FStrPart p;
      p.is_expr = true;
      if (colon >= 0) p.spec = src.sub(colon + 1, j - colon - 1);
      Parser sub(expr_src, unit_->module, arena_, diag_);
      // 位置は f 文字列の頭に寄せる（細かい桁は出さない）
      for (int k = 0; k < sub.toks_.size(); k++) { sub.toks_[k].line = t.line; sub.toks_[k].col = t.col; }
      p.expr = sub.parse_expr();
      n->parts.push(p);
      i = j + 1;
      continue;
    }
    lit.push(c);
    i++;
  }
  if (lit.size()) {
    FStrPart p;
    p.is_expr = false;
    fstr_unescape(lit, 0, lit.size(), &p.text);
    n->parts.push(p);
  }
  return n;
}

Node* Parser::parse_primary() {
  const Token& t = cur();
  switch (t.kind) {
    case TK_Int: { Node* n = node(E_Int); n->ival = t.ival; p_++; return n; }
    case TK_Float: { Node* n = node(E_Float); n->dval = t.dval; p_++; return n; }
    case TK_True: { Node* n = node(E_Bool); n->ival = 1; p_++; return n; }
    case TK_False: { Node* n = node(E_Bool); n->ival = 0; p_++; return n; }
    case TK_None: { Node* n = node(E_None); p_++; return n; }
    case TK_Str: { Node* n = node(E_Str); n->name = t.text; p_++; return n; }
    case TK_Bytes: { Node* n = node(E_Bytes); n->name = t.text; p_++; return n; }
    case TK_FStr: { Node* n = parse_fstring(t); p_++; return n; }
    case TK_This: { Node* n = node(E_This); p_++; return n; }
    case TK_Super: { Node* n = node(E_Super); p_++; return n; }
    case TK_Ident: { Node* n = node(E_Ident); n->name = t.text; n->len = t.len; p_++; return n; }
    // 型名は関数のように呼べる（型変換）
    case TK_ThisType: { Node* n = node(E_Ident); n->name = Str("This"); p_++; return n; }
    case TK_Panic: {
      Node* n = node(S_Panic);
      p_++;
      if (eat(TK_LParen)) {
        if (!at(TK_RParen)) n->a = parse_expr();
        expect(TK_RParen, ") が要ります", "expected )");
      } else {
        error_here(diag_.L("panic(\"理由\") と書きます", "write panic(\"reason\")"), Str());
      }
      return n;
    }
    case TK_Parallel: {
      Node* n = node(E_Parallel);
      p_++;
      expect(TK_LBrace, "parallel の後ろは { で始めます", "expected { after parallel");
      while (!at(TK_RBrace) && !at(TK_EOF)) {
        Node* e = parse_expr();
        expect(TK_Semi, "parallel の中の文にも ; が要ります", "expected ; inside parallel");
        n->list.push(e);
      }
      expect(TK_RBrace, "parallel は } で閉じます", "expected } to close parallel");
      return n;
    }
    case TK_LParen: {
      p_++;
      Node* e = parse_expr();
      expect(TK_RParen, ") が要ります", "expected )");
      return e;
    }
    case TK_LBracket: {
      Node* n = node(E_ListLit);
      p_++;
      if (!at(TK_RBracket)) {
        do {
          if (at(TK_RBracket)) break;
          n->list.push(parse_expr());
        } while (eat(TK_Comma));
      }
      expect(TK_RBracket, "配列は ] で閉じます", "expected ] to close the list");
      return n;
    }
    case TK_LBrace: {
      Node* n = node(E_MapLit);
      p_++;
      if (!at(TK_RBrace)) {
        do {
          if (at(TK_RBrace)) break;
          MapPair pr;
          pr.key = parse_expr();
          expect(TK_Colon, "連想配列は キー: 値 と書きます", "expected : between key and value");
          pr.val = parse_expr();
          n->pairs.push(pr);
        } while (eat(TK_Comma));
      }
      expect(TK_RBrace, "連想配列は } で閉じます", "expected } to close the map");
      return n;
    }
    default: break;
  }
  // 型名（int / list / map ...）も名前として扱う。型変換の呼び出しに使う
  if (t.text.size() > 0) {
    Node* n = node(E_Ident);
    n->name = t.text;
    n->len = t.len;
    p_++;
    return n;
  }
  error_here(diag_.L("ここに式が要ります", "expected an expression"),
             diag_.L("値・変数・呼び出しのどれかを書きます", "write a value, a variable, or a call"));
  Node* n = node(E_Int);
  p_++;
  return n;
}

// ------------------------------------------------------------------ 全体
Unit* Parser::parse() {
  while (at(TK_Import)) parse_import();

  while (!at(TK_EOF)) {
    int before = p_;
    bool is_public = false;
    if (at(TK_Public)) { is_public = true; p_++; }
    else if (at(TK_Private)) { p_++; }

    if (at(TK_Import)) {
      Diagnostic& d = diag_.error("E0001", diag_.L("import はファイルの先頭にまとめて書きます",
                                                   "imports must appear at the top of the file"));
      d.spans.push(Span(cur().line, cur().col, cur().len));
      d.help.push(diag_.L("関数やクラスより前に移します", "move it above the declarations"));
      parse_import();
      continue;
    }
    if (at(TK_Func)) {
      FuncDecl* f = parse_func(is_public, false, false, false);
      unit_->funcs.push(f);
      if (f->name == "main") unit_->has_main = true;
      continue;
    }
    if (at(TK_Class)) {
      unit_->classes.push(parse_class(is_public));
      continue;
    }
    if (at(TK_Var) || at(TK_Const)) {
      bool is_const = at(TK_Const);
      unit_->globals.push(parse_global(is_public, is_const));
      continue;
    }
    if (is_public) {
      error_here(diag_.L("public を付けられるのは func / class / var / const だけです",
                         "public may only be used on func, class, var, or const"), Str());
      sync_to_statement();
      continue;
    }
    // トップレベルに並べた文
    Node* s = parse_statement();
    if (s) unit_->top_stmts.push(s);
    if (p_ == before) { p_++; }  // 進まないときは1つ読み飛ばす（無限ループ防止）
  }
  return unit_;
}

}  // namespace shark

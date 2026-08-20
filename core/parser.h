// parser.h — 構文解析（spec/syntax.md）
#ifndef SHARK_PARSER_H
#define SHARK_PARSER_H

#include "ast.h"
#include "diag.h"
#include "lexer.h"

namespace shark {

class Parser {
 public:
  Parser(const Str& src, const Str& module, Arena& arena, DiagBag& diag);
  Unit* parse();

 private:
  // 位置
  const Token& cur() const { return toks_[p_]; }
  const Token& peek(int n) const { int i = p_ + n; return toks_[i < toks_.size() ? i : toks_.size() - 1]; }
  bool at(TokKind k) const { return toks_[p_].kind == k; }
  bool eat(TokKind k) { if (at(k)) { p_++; return true; } return false; }
  bool expect(TokKind k, const char* what_ja, const char* what_en);
  void error_here(const Str& msg, const Str& help);
  void error_here(const char* code, const Str& msg, const Str& help);
  // いま見ているのが予約語か（func 〜 none）
  bool at_keyword() const { return cur().kind >= TK_Func && cur().kind <= TK_None; }
  // 名前が要るところに来た予約語。誤りを出し、綴りをそのまま名前として返して読み進める
  Str keyword_as_name(const char* role_ja, const char* role_en);
  void sync_to_statement();

  Node* node(NodeKind k);
  Node* node_at(NodeKind k, const Token& t);

  // 宣言
  void parse_import();
  FuncDecl* parse_func(bool is_public, bool is_virtual, bool is_override, bool in_class);
  void parse_signature(FuncDecl* f);
  ClassDecl* parse_class(bool is_public);
  GlobalDecl* parse_global(bool is_public, bool is_const);
  void parse_generic_params(Vec<GenericParam>* out);
  TypeExpr* parse_type();
  bool try_parse_type_args(Vec<TypeExpr*>* out);

  // 文
  Node* parse_block();
  Node* parse_statement();
  Node* parse_var_stmt(bool is_const);
  Node* parse_if();
  Node* parse_while();
  Node* parse_for();

  // 式
  Node* parse_expr() { return parse_coalesce(); }
  Node* parse_coalesce();
  Node* parse_or();
  Node* parse_and();
  Node* parse_bit_or();
  Node* parse_bit_xor();
  Node* parse_bit_and();
  Node* parse_equality();
  Node* parse_relational();
  Node* parse_shift();
  Node* parse_additive();
  Node* parse_multiplicative();
  Node* parse_unary();
  Node* parse_power();
  Node* parse_postfix();
  bool at_shr() const;      // 並んだ 2 つの > を >> とみなす
  bool at_shr_assign() const;
  Node* parse_primary();
  Node* parse_lambda();
  Node* parse_fstring(const Token& t);
  void parse_args(Node* call);

  Vec<Token> toks_;
  int p_;
  Arena& arena_;
  DiagBag& diag_;
  Unit* unit_;
  int panic_depth_;
};

}  // namespace shark
#endif

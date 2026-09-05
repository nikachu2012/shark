// jit_x64.inc — x86-64 の機械語を作る（core/jit.cpp から取り込む）
//
// 値の置き場（TaskState::stack）は仮想マシンとまったく同じものを使い、
// 積み方も変えない。そのため**どの命令の切れ目でも仮想マシンに戻れる**。
// 作り方の分からない命令は、その場で仮想マシンに1つだけ動かしてもらう。
// 組み立て方は jit_arm64.inc とまったく同じで、機械語の作り方だけが違う。
//
// 呼び出しをまたいで持つもの（呼ばれた側が守るレジスタ）
//   rbx VM*                r12 局所変数の先頭   r13 積んだ値の上（次に書く場所）
//   r14 残りの刻み（budget） r15 グローバルの先頭  rbp 定数の表
// 守るレジスタは6つしかないので、値の置き場の限界（ループの入口でしか見ない）だけは
// 自分の枠に置く（[rsp+kLimAt]）。
// rax・rcx・rdx・rsi・rdi・r8〜r11 は使い捨て。手伝いの関数を呼ぶと消える。
//
// 呼び出しの決まりは2つある。違うのは引数の置き場所と、呼ぶ側が空けておく枠だけ
// なので、arg_reg / arg_imm と前口上に閉じ込めてある。
//   Windows    rcx rdx r8 r9 のあとは積む。呼ぶ側が 32 バイト空けておく（影の置き場）
//   System V   rdi rsi rdx rcx r8 r9 のあとは積む（macOS・Linux）

namespace {

#if defined(_WIN32)
#define SHARK_JIT_WIN64 1
#endif

// レジスタの番号（機械語がそのまま使う並び）
enum {
  X_AX = 0, X_CX, X_DX, X_BX, X_SP, X_BP, X_SI, X_DI,
  X_R8, X_R9, X_R10, X_R11, X_R12, X_R13, X_R14, X_R15
};
enum { R_VM = X_BX, R_LOC = X_R12, R_TOP = X_R13, R_BUD = X_R14, R_GLB = X_R15, R_KON = X_BP };

// 分岐の条件（jcc / setcc の下 4 ビット）
enum {
  C_O = 0, C_NO, C_B, C_AE, C_E, C_NE, C_BE, C_A, C_S, C_NS, C_P, C_NP, C_L, C_GE, C_LE, C_G
};

// 自分の枠。手伝いの関数に渡す引数の置き場（6つぶんで 48）と、値の置き場の限界。
// 呼ぶ直前に rsp が 16 の倍数になるよう、押し込む6つと合わせて大きさを決めてある
static const int kLimAt = 48;
static const int kFrame = 56;

struct Fix {
  int at;    // 埋める 4 バイトの位置（その次の位置からの差を書く）
  int lab;
};

// 後回しにする短い道すじ（めったに通らないので、本体の後ろにまとめて置く）
struct Stub {
  int lab;
  int kind;      // 0 = 仮想マシンに1命令頼む、1 = 抜ける前に覚え書きを合わせる、2 = 比べもの
  int op_ip;
  int next_lab;  // 戻り先（kind 0 と 2）
  int arg;       // kind 1 は ip、kind 2 は命令コード
};

struct Asm {
  Vec<uint8_t> w;
  Vec<int32_t> lpos;
  Vec<Fix> fix;
  Vec<Stub> stubs;
  bool ok;

  Asm() : ok(true) {}

  int here() const { return w.size(); }
  void b1(uint32_t x) { w.push((uint8_t)(x & 0xffu)); }
  void b4(int32_t v) { for (int i = 0; i < 4; i++) b1((uint32_t)v >> (i * 8)); }
  void b8(uint64_t v) { for (int i = 0; i < 8; i++) b1((uint32_t)(v >> (i * 8))); }
  int label() { lpos.push(-1); return lpos.size() - 1; }
  void bind(int l) { lpos[l] = w.size(); }

  // --- 命令の組み立て ---
  // REX。w64 = 64 ビットで扱う、r = reg の 4 ビット目、b = rm の 4 ビット目
  void rex(int w64, int r, int b) {
    uint32_t v = 0x40u | ((uint32_t)(w64 & 1) << 3) | ((((uint32_t)r >> 3) & 1u) << 2) |
                 (((uint32_t)b >> 3) & 1u);
    if (v != 0x40u) b1(v);
  }
  // レジスタどうし（mod = 11）。reg には命令の枝番が入ることもある
  void mrm_r(int reg, int rm) { b1(0xC0u | ((uint32_t)(reg & 7) << 3) | (uint32_t)(rm & 7)); }
  // [base + ずれ]。rsp と r12 は SIB が要り、rbp と r13 は「ずれ無し」を書けない
  void mrm_m(int reg, int base, int off) {
    int b = base & 7;
    int mod = (off == 0 && b != 5) ? 0 : (off >= -128 && off <= 127) ? 1 : 2;
    b1((uint32_t)(mod << 6) | ((uint32_t)(reg & 7) << 3) | (uint32_t)b);
    if (b == 4) b1(0x24);
    if (mod == 1) b1((uint32_t)off);
    else if (mod == 2) b4(off);
  }

  // --- 動かす ---
  void mov(int d, int s) {
    if (d == s) return;
    rex(1, s, d);
    b1(0x89);
    mrm_r(s, d);
  }
  void mov_addr(int d, uintptr_t v) {   // 住所は必ず 8 バイトで置く
    rex(1, 0, d);
    b1(0xB8u + (uint32_t)(d & 7));
    b8((uint64_t)v);
  }
  // 数を置く。**どの道すじも旗（フラグ）を触らない**ので、比べたあとにも使える
  void mov_imm(int d, int64_t v) {
    if (v >= -2147483647LL - 1 && v <= 2147483647LL) {
      rex(1, 0, d);
      b1(0xC7);
      mrm_r(0, d);
      b4((int32_t)v);
      return;
    }
    if (v > 0 && v <= 0xFFFFFFFFLL) {   // 32 ビットで置けば、上は 0 で埋まる
      rex(0, 0, d);
      b1(0xB8u + (uint32_t)(d & 7));
      b4((int32_t)(uint32_t)v);
      return;
    }
    mov_addr(d, (uintptr_t)(uint64_t)v);
  }

  // --- 読み書き ---
  void ld(int d, int base, int off) { rex(1, d, base); b1(0x8B); mrm_m(d, base, off); }
  void st(int s, int base, int off) { rex(1, s, base); b1(0x89); mrm_m(s, base, off); }
  void ldb(int d, int base, int off) {   // 1 バイト読んで、上を 0 で埋める
    rex(1, d, base);
    b1(0x0F);
    b1(0xB6);
    mrm_m(d, base, off);
  }
  void ldb_r(int d, int s) { rex(1, d, s); b1(0x0F); b1(0xB6); mrm_r(d, s); }
  void ldsw(int d, int base, int off) {   // 4 バイト読んで、符号を伸ばす
    rex(1, d, base);
    b1(0x63);
    mrm_m(d, base, off);
  }
  void st32_imm(int base, int off, int32_t v) {
    rex(0, 0, base);
    b1(0xC7);
    mrm_m(0, base, off);
    b4(v);
  }
  // 4 バイトのところに小さい数を足し引き・比べる（参照数を数えるところで使う）
  void alu32_m(int ext, int base, int off, int v) {
    rex(0, 0, base);
    b1(0x83);
    mrm_m(ext, base, off);
    b1((uint32_t)v);
  }

  // --- 足し引き・比べる ---
  void alu(int opc, int d, int s) { rex(1, s, d); b1(opc); mrm_r(s, d); }
  void add(int d, int s) { alu(0x01, d, s); }
  void sub(int d, int s) { alu(0x29, d, s); }
  void and_(int d, int s) { alu(0x21, d, s); }
  void or_(int d, int s) { alu(0x09, d, s); }
  void xor_(int d, int s) { alu(0x31, d, s); }
  void cmp(int d, int s) { alu(0x39, d, s); }
  void test(int d, int s) { alu(0x85, d, s); }
  void alu_imm(int ext, int d, int64_t v) {
    if (v >= -128 && v <= 127) {
      rex(1, 0, d);
      b1(0x83);
      mrm_r(ext, d);
      b1((uint32_t)(int32_t)v);
      return;
    }
    if (v < -2147483647LL - 1 || v > 2147483647LL) { ok = false; return; }
    rex(1, 0, d);
    b1(0x81);
    mrm_r(ext, d);
    b4((int32_t)v);
  }
  void add_imm(int d, int64_t v) { alu_imm(0, d, v); }
  void sub_imm(int d, int64_t v) { alu_imm(5, d, v); }
  void cmp_imm(int d, int64_t v) { alu_imm(7, d, v); }
  void imul(int d, int s) { rex(1, d, s); b1(0x0F); b1(0xAF); mrm_r(d, s); }
  void neg(int d) { rex(1, 0, d); b1(0xF7); mrm_r(3, d); }
  void not_(int d) { rex(1, 0, d); b1(0xF7); mrm_r(2, d); }
  void idiv(int d) { rex(1, 0, d); b1(0xF7); mrm_r(7, d); }
  void cqo() { b1(0x48); b1(0x99); }
  void shl_cl(int d) { rex(1, 0, d); b1(0xD3); mrm_r(4, d); }
  void sar_cl(int d) { rex(1, 0, d); b1(0xD3); mrm_r(7, d); }
  void shl_imm(int d, int n) { rex(1, 0, d); b1(0xC1); mrm_r(4, d); b1((uint32_t)n); }
  // 条件が立っていれば 1、でなければ 0 を、下の 1 バイトに置く（上は先に消しておく）
  void setcc(int cond, int d) {
    if (d >= 4) b1(0x40u | (((uint32_t)d >> 3) & 1u));   // sil/dil を指すには REX が要る
    b1(0x0F);
    b1(0x90u + (uint32_t)cond);
    mrm_r(0, d);
  }

  // --- 小数（double。SSE2 は x86-64 なら必ずある）---
  void movsd_ld(int d, int base, int off) {
    b1(0xF2); rex(0, d, base); b1(0x0F); b1(0x10); mrm_m(d, base, off);
  }
  void movsd_st(int s, int base, int off) {
    b1(0xF2); rex(0, s, base); b1(0x0F); b1(0x11); mrm_m(s, base, off);
  }
  void sse(int opc, int d, int s) { b1(0xF2); rex(0, d, s); b1(0x0F); b1(opc); mrm_r(d, s); }
  void addsd(int d, int s) { sse(0x58, d, s); }
  void subsd(int d, int s) { sse(0x5C, d, s); }
  void mulsd(int d, int s) { sse(0x59, d, s); }
  void divsd(int d, int s) { sse(0x5E, d, s); }

  // --- 分岐 ---
  void rel32(int l) { Fix f; f.at = here(); f.lab = l; fix.push(f); b4(0); }
  void jmp(int l) { b1(0xE9); rel32(l); }
  void jcc(int cond, int l) { b1(0x0F); b1(0x80u + (uint32_t)cond); rel32(l); }
  void jmp_r(int r) { if (r >= 8) b1(0x41); b1(0xFF); mrm_r(4, r); }
  void call_r(int r) { if (r >= 8) b1(0x41); b1(0xFF); mrm_r(2, r); }
  void push(int r) { if (r >= 8) b1(0x41); b1(0x50u + (uint32_t)(r & 7)); }
  void pop(int r) { if (r >= 8) b1(0x41); b1(0x58u + (uint32_t)(r & 7)); }
  void ret() { b1(0xC3); }

  // --- 手伝いの関数を呼ぶ ---
  // i 番目の引数を置く。元は跨いで持つレジスタか数なので、引数の置き場とは重ならない
  void arg_reg(int i, int r) {
#if defined(SHARK_JIT_WIN64)
    static const int kArg[4] = {X_CX, X_DX, X_R8, X_R9};
    if (i < 4) mov(kArg[i], r);
    else st(r, X_SP, 32 + (i - 4) * 8);
#else
    static const int kArg[6] = {X_DI, X_SI, X_DX, X_CX, X_R8, X_R9};
    mov(kArg[i], r);
#endif
  }
  void arg_imm(int i, int64_t v) {
#if defined(SHARK_JIT_WIN64)
    static const int kArg[4] = {X_CX, X_DX, X_R8, X_R9};
    if (i < 4) { mov_imm(kArg[i], v); return; }
    mov_imm(X_AX, v);
    st(X_AX, X_SP, 32 + (i - 4) * 8);
#else
    static const int kArg[6] = {X_DI, X_SI, X_DX, X_CX, X_R8, X_R9};
    mov_imm(kArg[i], v);
#endif
  }
  void call_helper(int slot) {
    mov_addr(X_AX, (uintptr_t)g_helpers[slot]);
    call_r(X_AX);
  }

  // 飛び先を埋める
  bool resolve() {
    for (int i = 0; i < fix.size(); i++) {
      int to = lpos[fix[i].lab];
      if (to < 0) return false;
      int64_t off = (int64_t)to - (int64_t)(fix[i].at + 4);
      if (off > 2147483647LL || off < -2147483647LL - 1) return false;
      uint32_t v = (uint32_t)(int32_t)off;
      for (int k = 0; k < 4; k++) w[fix[i].at + k] = (uint8_t)(v >> (k * 8));
    }
    return true;
  }
};

// 上から i 番目（0 が一番上）の値の場所
static int slot_at(int i) { return -16 * (i + 1); }

// 参照を1つ増やす（obj のときだけ呼ぶ。o は場所を指すレジスタ）
static void emit_retain(Asm& a, int o) { a.alu32_m(0, o, g_lay.obj_rc, 1); }

// 参照を1つ減らす。最後の1つなら片付けを頼む（使い捨てのレジスタと戻り先が消える）
static void emit_release(Asm& a, int o) {
  int l_slow = a.label(), l_done = a.label();
  a.alu32_m(7, o, g_lay.obj_rc, 1);   // cmp [o + rc], 1
  a.jcc(C_LE, l_slow);
  a.alu32_m(5, o, g_lay.obj_rc, 1);   // sub [o + rc], 1
  a.jmp(l_done);
  a.bind(l_slow);
  a.arg_reg(0, o);
  a.call_helper(H_RELEASE);
  a.bind(l_done);
}

// 値を1つ積む（rk = 種類、rp = 中身）
static void emit_push(Asm& a, int rk, int rp) {
  a.st(rk, R_TOP, 0);
  a.st(rp, R_TOP, 8);
  a.add_imm(R_TOP, 16);
}

// ------------------------------------------------------------------ 作る
struct Gen {
  Asm a;
  FuncInfo* f;
  const uint8_t* c;
  int n;
  Vec<int32_t> iplab;   // 命令の位置 -> ラベル
  int l_exit;

  // 後回しの道すじを1つ足して、そのラベルを返す
  int stub(int kind, int op_ip, int next_lab, int arg) {
    Stub s;
    s.lab = a.label();
    s.kind = kind;
    s.op_ip = op_ip;
    s.next_lab = next_lab;
    s.arg = arg;
    a.stubs.push(s);
    return s.lab;
  }

  // 仮想マシンに1命令だけ動かしてもらう（そのまま続きへ落ちる形）
  void emit_fallback(int op_ip, int next_ip) {
    a.arg_reg(0, R_VM);
    a.arg_imm(1, op_ip);
    a.arg_imm(2, next_ip);
    a.arg_reg(3, R_TOP);
    a.call_helper(H_FALLBACK);
    a.test(X_AX, X_AX);
    a.jcc(C_E, l_exit);
    a.mov(R_TOP, X_AX);
  }
};

// 1つの命令を機械語にする。作り方を知らない命令なら false
static bool emit_op(Gen& g, int ip, int len, int next) {
  Asm& a = g.a;
  FuncInfo* f = g.f;
  const uint8_t* c = g.c;
  int op = c[ip];
  int arg = len >= 4 ? rd_i32(c, ip + 1) : 0;
  // 最後の命令の次は無い（そこまで来ることはないが、来たら抜ける）
  int next_lab = (next < g.n && g.iplab[next] >= 0) ? g.iplab[next] : g.l_exit;

  switch (op) {
    case OP_NOP:
      return true;

    case OP_CONST: {
      if (arg < 0 || arg >= f->consts.size()) return false;
      const Value& v = f->consts[arg];
      if (v.k == V_Obj) {
        a.ld(X_AX, R_KON, arg * 16);
        a.ld(X_CX, R_KON, arg * 16 + 8);
        emit_retain(a, X_CX);
      } else {
        a.mov_imm(X_AX, (int64_t)(uint8_t)v.k);
        a.mov_imm(X_CX, v.i);
      }
      emit_push(a, X_AX, X_CX);
      return true;
    }
    case OP_NONE: case OP_VOID: case OP_TRUE: case OP_FALSE: {
      int kind = op == OP_NONE ? V_None : op == OP_VOID ? V_Void : V_Bool;
      a.mov_imm(X_AX, kind);
      a.mov_imm(X_CX, op == OP_TRUE ? 1 : 0);
      emit_push(a, X_AX, X_CX);
      return true;
    }
    case OP_POP: {
      int l_skip = a.label();
      a.ldb(X_AX, R_TOP, slot_at(0));
      a.sub_imm(R_TOP, 16);
      a.cmp_imm(X_AX, V_Obj);
      a.jcc(C_NE, l_skip);
      a.ld(X_CX, R_TOP, 8);
      emit_release(a, X_CX);
      a.bind(l_skip);
      return true;
    }
    case OP_DUP: {
      int l_st = a.label();
      a.ld(X_AX, R_TOP, slot_at(0));
      a.ld(X_CX, R_TOP, slot_at(0) + 8);
      a.ldb_r(X_DX, X_AX);
      a.cmp_imm(X_DX, V_Obj);
      a.jcc(C_NE, l_st);
      emit_retain(a, X_CX);
      a.bind(l_st);
      emit_push(a, X_AX, X_CX);
      return true;
    }
    case OP_SWAP: {
      a.ld(X_AX, R_TOP, slot_at(1));
      a.ld(X_CX, R_TOP, slot_at(1) + 8);
      a.ld(X_DX, R_TOP, slot_at(0));
      a.ld(X_R8, R_TOP, slot_at(0) + 8);
      a.st(X_DX, R_TOP, slot_at(1));
      a.st(X_R8, R_TOP, slot_at(1) + 8);
      a.st(X_AX, R_TOP, slot_at(0));
      a.st(X_CX, R_TOP, slot_at(0) + 8);
      return true;
    }

    case OP_LOAD_LOCAL: case OP_LOAD_GLOBAL: {
      bool loc = op == OP_LOAD_LOCAL;
      if (loc && (arg < 0 || arg >= f->nlocals)) return false;
      if (arg < 0) return false;
      int base = loc ? R_LOC : R_GLB;
      int l_st = a.label();
      a.ld(X_AX, base, arg * 16);
      a.ld(X_CX, base, arg * 16 + 8);
      a.ldb_r(X_DX, X_AX);
      a.cmp_imm(X_DX, V_Obj);
      a.jcc(C_B, l_st);
      if (loc) {
        // ref 引数（借用）は仮想マシンに任せる
        a.jcc(C_A, g.stub(0, ip, next_lab, 0));
      } else {
        a.jcc(C_A, l_st);
      }
      emit_retain(a, X_CX);
      a.bind(l_st);
      emit_push(a, X_AX, X_CX);
      return true;
    }
    case OP_STORE_LOCAL: case OP_STORE_GLOBAL: {
      bool loc = op == OP_STORE_LOCAL;
      if (loc && (arg < 0 || arg >= f->nlocals)) return false;
      if (arg < 0) return false;
      int base = loc ? R_LOC : R_GLB;
      int l_done = a.label();
      a.ld(X_AX, R_TOP, slot_at(0));        // 積んであるほう（新しい値）
      a.ld(X_CX, R_TOP, slot_at(0) + 8);
      a.ld(X_DX, base, arg * 16);           // いま入っているほう
      a.ld(X_R8, base, arg * 16 + 8);
      a.ldb_r(X_R9, X_DX);
      if (loc) {
        a.cmp_imm(X_R9, V_Ref);
        a.jcc(C_E, g.stub(0, ip, next_lab, 0));
      }
      a.sub_imm(R_TOP, 16);
      a.st(X_AX, base, arg * 16);
      a.st(X_CX, base, arg * 16 + 8);
      a.cmp_imm(X_R9, V_Obj);
      a.jcc(C_NE, l_done);
      emit_release(a, X_R8);
      a.bind(l_done);
      return true;
    }

    case OP_LOAD_FIELD: {
      if (arg < 0) return false;
      int l_fb = g.stub(0, ip, next_lab, 0);
      int l_st = a.label();
      a.ldb(X_AX, R_TOP, slot_at(0));
      a.cmp_imm(X_AX, V_Obj);
      a.jcc(C_NE, l_fb);
      a.ld(X_CX, R_TOP, slot_at(0) + 8);            // Obj*
      a.ldb(X_DX, X_CX, g_lay.obj_kind);
      a.cmp_imm(X_DX, O_Inst);
      a.jcc(C_NE, l_fb);
      a.ldsw(X_R8, X_CX, g_lay.inst_fields + g_lay.vec_size);
      a.cmp_imm(X_R8, arg);
      a.jcc(C_LE, l_fb);                            // 数が足りない（壊れている）
      a.ld(X_R9, X_CX, g_lay.inst_fields + g_lay.vec_data);
      a.ld(X_R10, X_R9, arg * 16);
      a.ld(X_R11, X_R9, arg * 16 + 8);
      a.ldb_r(X_AX, X_R10);
      a.cmp_imm(X_AX, V_Obj);
      a.jcc(C_NE, l_st);
      emit_retain(a, X_R11);
      a.bind(l_st);
      a.st(X_R10, R_TOP, slot_at(0));               // 実体のあった場所に結果を置く
      a.st(X_R11, R_TOP, slot_at(0) + 8);
      emit_release(a, X_CX);                        // 実体を離す（結果は先に置いてある）
      return true;
    }

    case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT:
    case OP_DIV_INT: case OP_MOD_INT: {
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ld(X_AX, R_TOP, slot_at(1) + 8);
      a.ld(X_CX, R_TOP, slot_at(0) + 8);
      if (op == OP_ADD_INT) {
        a.add(X_AX, X_CX);
        a.jcc(C_O, l_fb);
      } else if (op == OP_SUB_INT) {
        a.sub(X_AX, X_CX);
        a.jcc(C_O, l_fb);
      } else if (op == OP_MUL_INT) {
        a.imul(X_AX, X_CX);
        a.jcc(C_O, l_fb);
      } else {
        a.test(X_CX, X_CX);
        a.jcc(C_E, l_fb);      // 0 で割る
        a.cmp_imm(X_CX, -1);
        a.jcc(C_E, l_fb);      // -1 で割るのは、あふれることがあるので任せる
        a.cqo();               // rdx:rax に符号を伸ばす
        a.idiv(X_CX);          // rax = 商、rdx = 余り
        if (op == OP_MOD_INT) a.mov(X_AX, X_DX);
      }
      a.mov_imm(X_CX, V_Int);
      a.st(X_CX, R_TOP, slot_at(1));
      a.st(X_AX, R_TOP, slot_at(1) + 8);
      a.sub_imm(R_TOP, 16);
      return true;
    }
    case OP_NEG_INT: {
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ld(X_AX, R_TOP, slot_at(0) + 8);
      a.neg(X_AX);
      a.jcc(C_O, l_fb);
      a.mov_imm(X_CX, V_Int);
      a.st(X_CX, R_TOP, slot_at(0));
      a.st(X_AX, R_TOP, slot_at(0) + 8);
      return true;
    }
    case OP_AND_INT: case OP_OR_INT: case OP_XOR_INT: {
      a.ld(X_AX, R_TOP, slot_at(1) + 8);
      a.ld(X_CX, R_TOP, slot_at(0) + 8);
      if (op == OP_AND_INT) a.and_(X_AX, X_CX);
      else if (op == OP_OR_INT) a.or_(X_AX, X_CX);
      else a.xor_(X_AX, X_CX);
      a.mov_imm(X_CX, V_Int);
      a.st(X_CX, R_TOP, slot_at(1));
      a.st(X_AX, R_TOP, slot_at(1) + 8);
      a.sub_imm(R_TOP, 16);
      return true;
    }
    case OP_SHL_INT: case OP_SHR_INT: {
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ld(X_AX, R_TOP, slot_at(1) + 8);
      a.ld(X_CX, R_TOP, slot_at(0) + 8);   // ずらす数は cl で渡す決まり
      a.cmp_imm(X_CX, 63);
      a.jcc(C_A, l_fb);      // 0〜63 の外（負の数も、符号なしで見れば大きいので入る）
      if (op == OP_SHL_INT) a.shl_cl(X_AX);
      else a.sar_cl(X_AX);   // 右へは符号を保ったままずらす（仮想マシンと同じ）
      a.mov_imm(X_CX, V_Int);
      a.st(X_CX, R_TOP, slot_at(1));
      a.st(X_AX, R_TOP, slot_at(1) + 8);
      a.sub_imm(R_TOP, 16);
      return true;
    }
    case OP_BNOT_INT: {
      a.ld(X_AX, R_TOP, slot_at(0) + 8);
      a.not_(X_AX);
      a.mov_imm(X_CX, V_Int);
      a.st(X_CX, R_TOP, slot_at(0));
      a.st(X_AX, R_TOP, slot_at(0) + 8);
      return true;
    }

    case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT: {
      a.movsd_ld(0, R_TOP, slot_at(1) + 8);
      a.movsd_ld(1, R_TOP, slot_at(0) + 8);
      if (op == OP_ADD_FLOAT) a.addsd(0, 1);
      else if (op == OP_SUB_FLOAT) a.subsd(0, 1);
      else if (op == OP_MUL_FLOAT) a.mulsd(0, 1);
      else a.divsd(0, 1);
      a.mov_imm(X_AX, V_Float);
      a.st(X_AX, R_TOP, slot_at(1));
      a.movsd_st(0, R_TOP, slot_at(1) + 8);
      a.sub_imm(R_TOP, 16);
      return true;
    }
    case OP_NEG_FLOAT: {
      // 符号のビットを裏返すだけ（0.0 も NaN も、小数の引き算と同じ答えになる）
      a.ld(X_AX, R_TOP, slot_at(0) + 8);
      a.mov_imm(X_CX, -9223372036854775807LL - 1);
      a.xor_(X_AX, X_CX);
      a.mov_imm(X_CX, V_Float);
      a.st(X_CX, R_TOP, slot_at(0));
      a.st(X_AX, R_TOP, slot_at(0) + 8);
      return true;
    }

    case OP_NOT: {
      int l_st = a.label();
      a.ldb(X_AX, R_TOP, slot_at(0));
      a.mov_imm(X_CX, 1);
      a.cmp_imm(X_AX, V_Bool);
      a.jcc(C_NE, l_st);
      a.ld(X_DX, R_TOP, slot_at(0) + 8);
      a.test(X_DX, X_DX);
      a.mov_imm(X_CX, 0);          // 数を置くだけなので、上で比べた結果は消えない
      a.setcc(C_E, X_CX);
      a.bind(l_st);
      a.mov_imm(X_AX, V_Bool);
      a.st(X_AX, R_TOP, slot_at(0));
      a.st(X_CX, R_TOP, slot_at(0) + 8);
      return true;
    }
    case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
      int l_slow = g.stub(2, ip, next_lab, op);
      a.ldb(X_AX, R_TOP, slot_at(1));
      a.cmp_imm(X_AX, V_Int);
      a.jcc(C_NE, l_slow);
      a.ldb(X_AX, R_TOP, slot_at(0));
      a.cmp_imm(X_AX, V_Int);
      a.jcc(C_NE, l_slow);
      a.ld(X_DX, R_TOP, slot_at(1) + 8);
      a.ld(X_R8, R_TOP, slot_at(0) + 8);
      a.cmp(X_DX, X_R8);
      a.mov_imm(X_CX, 0);
      int cond = op == OP_EQ ? C_E : op == OP_NE ? C_NE : op == OP_LT ? C_L
                 : op == OP_LE ? C_LE : op == OP_GT ? C_G : C_GE;
      a.setcc(cond, X_CX);
      a.mov_imm(X_AX, V_Bool);
      a.st(X_AX, R_TOP, slot_at(1));
      a.st(X_CX, R_TOP, slot_at(1) + 8);
      a.sub_imm(R_TOP, 16);
      return true;
    }

    case OP_JUMP: {
      if (arg < 0 || arg >= g.n) return false;
      a.jmp(g.iplab[arg]);
      return true;
    }
    case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
    case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP: {
      if (arg < 0 || arg >= g.n) return false;
      bool keep = (op == OP_JUMP_IF_FALSE_KEEP || op == OP_JUMP_IF_TRUE_KEEP);
      bool on_true = (op == OP_JUMP_IF_TRUE || op == OP_JUMP_IF_TRUE_KEEP);
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ldb(X_AX, R_TOP, slot_at(0));
      a.cmp_imm(X_AX, V_Bool);
      a.jcc(C_NE, l_fb);                 // bool 以外は仮想マシンに任せる
      a.ld(X_CX, R_TOP, slot_at(0) + 8);
      if (!keep) a.sub_imm(R_TOP, 16);   // 引き算で比べた結果が消えるので、見るのは後
      a.test(X_CX, X_CX);
      a.jcc(on_true ? C_NE : C_E, g.iplab[arg]);
      return true;
    }
    case OP_JUMP_IF_NONE: case OP_JUMP_IF_NOT_NONE: {
      if (arg < 0 || arg >= g.n) return false;
      int l_fall = a.label();
      a.ldb(X_AX, R_TOP, slot_at(0));
      a.cmp_imm(X_AX, V_None);
      if (op == OP_JUMP_IF_NONE) {
        a.jcc(C_NE, l_fall);             // 値があるならそのまま
        a.sub_imm(R_TOP, 16);            // none は捨てて飛ぶ
        a.jmp(g.iplab[arg]);
      } else {
        a.jcc(C_E, l_fall);
        a.jmp(g.iplab[arg]);             // 値があるならそのまま飛ぶ
      }
      a.bind(l_fall);
      if (op == OP_JUMP_IF_NOT_NONE) a.sub_imm(R_TOP, 16);
      return true;
    }

    case OP_ITER_NEW: {
      a.mov_imm(X_AX, V_Int);
      a.mov_imm(X_CX, 0);
      emit_push(a, X_AX, X_CX);
      return true;
    }
    case OP_ITER_NEXT: {
      // 数の並び（range）だけをここで回す。ほかの入れ物と、終わりの後始末は仮想マシンへ
      if (arg < 0 || arg >= g.n) return false;
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ldb(X_AX, R_TOP, slot_at(1));
      a.cmp_imm(X_AX, V_Obj);
      a.jcc(C_NE, l_fb);
      a.ld(X_CX, R_TOP, slot_at(1) + 8);
      a.ldb(X_AX, X_CX, g_lay.obj_kind);
      a.cmp_imm(X_AX, O_Range);
      a.jcc(C_NE, l_fb);
      a.ld(X_DX, R_TOP, slot_at(0) + 8);      // いくつ目か
      a.ld(X_R8, X_CX, g_lay.range_start);
      a.ld(X_R9, X_CX, g_lay.range_end);
      a.ld(X_R10, X_CX, g_lay.range_step);
      a.mov(X_R11, X_DX);
      a.imul(X_R11, X_R10);
      a.add(X_R11, X_R8);                     // v = start + i * step
      int l_neg = a.label(), l_have = a.label();
      a.cmp_imm(X_R10, 0);
      a.jcc(C_LE, l_neg);
      a.cmp(X_R11, X_R9);
      a.jcc(C_GE, l_fb);                      // 終わり。後始末は仮想マシンに任せる
      a.jmp(l_have);
      a.bind(l_neg);
      a.cmp(X_R11, X_R9);
      a.jcc(C_LE, l_fb);
      a.bind(l_have);
      a.add_imm(X_DX, 1);
      a.st(X_DX, R_TOP, slot_at(0) + 8);
      a.mov_imm(X_AX, V_Int);
      emit_push(a, X_AX, X_R11);
      return true;
    }

    case OP_INDEX_GET: {
      // 配列と int のときだけ。ほかは仮想マシンへ（つまずく場合もあちらが出す）
      int l_fb = g.stub(0, ip, next_lab, 0);
      int l_st = a.label();
      a.ldb(X_AX, R_TOP, slot_at(1));
      a.cmp_imm(X_AX, V_Obj);
      a.jcc(C_NE, l_fb);
      a.ld(X_CX, R_TOP, slot_at(1) + 8);      // 入れ物
      a.ldb(X_AX, X_CX, g_lay.obj_kind);
      a.cmp_imm(X_AX, O_List);
      a.jcc(C_NE, l_fb);
      a.ldb(X_AX, R_TOP, slot_at(0));
      a.cmp_imm(X_AX, V_Int);
      a.jcc(C_NE, l_fb);
      a.ld(X_DX, R_TOP, slot_at(0) + 8);      // 添字
      a.ldsw(X_R8, X_CX, g_lay.list_vec + g_lay.vec_size);
      a.cmp(X_DX, X_R8);
      a.jcc(C_AE, l_fb);                      // 範囲の外（負の数もここで外れる）
      a.ld(X_R9, X_CX, g_lay.list_vec + g_lay.vec_data);
      a.mov(X_R11, X_DX);
      a.shl_imm(X_R11, 4);                    // 値1つは 16 バイト
      a.add(X_R9, X_R11);
      a.ld(X_R10, X_R9, 0);
      a.ld(X_R11, X_R9, 8);
      a.ldb_r(X_AX, X_R10);
      a.cmp_imm(X_AX, V_Obj);
      a.jcc(C_NE, l_st);
      emit_retain(a, X_R11);
      a.bind(l_st);
      a.sub_imm(R_TOP, 16);
      a.st(X_R10, R_TOP, slot_at(0));         // 入れ物のあった場所に結果を置く
      a.st(X_R11, R_TOP, slot_at(0) + 8);
      emit_release(a, X_CX);                  // 入れ物を離す
      return true;
    }

    // --- 呼び出し ---
    case OP_CALL: {
      int nargs = rd_i32(c, ip + 5);
      a.arg_reg(0, R_VM);
      a.arg_reg(1, R_TOP);
      a.arg_imm(2, arg);
      a.arg_imm(3, nargs);
      a.arg_imm(4, next);
      a.call_helper(H_CALL);
      a.test(X_AX, X_AX);
      a.jcc(C_E, g.stub(0, ip, next_lab, 0));   // 呼べなければ仮想マシンに任せる
      a.jmp(g.l_exit);                          // 枠が変わったので、いったん戻る
      return true;
    }
    case OP_CALL_NATIVE: {
      int nargs = rd_i32(c, ip + 5);
      a.arg_reg(0, R_VM);
      a.arg_reg(1, R_TOP);
      a.arg_imm(2, arg);
      a.arg_imm(3, nargs);
      a.arg_imm(4, ip);
      a.arg_imm(5, next);
      a.call_helper(H_CALL_NATIVE);
      a.test(X_AX, X_AX);
      a.jcc(C_E, g.l_exit);
      a.mov(R_TOP, X_AX);
      return true;
    }
    case OP_RET: case OP_RET_VOID: {
      a.arg_reg(0, R_VM);
      a.arg_reg(1, R_TOP);
      a.arg_imm(2, op == OP_RET ? 1 : 0);
      a.call_helper(H_RET);
      a.jmp(g.l_exit);
      return true;
    }

    default:
      return false;
  }
}

// 関数を1つ、機械語にする
static bool jit_emit(FuncInfo* f, const JitScan& sc, Vec<uint8_t>* out, Vec<int32_t>* offout) {
  Gen g;
  g.f = f;
  g.c = f->code.data();
  g.n = f->code.size();
  Asm& a = g.a;

  g.iplab.resize(g.n, -1);
  for (int ip = 0; ip < g.n; ip++)
    if (sc.start[ip]) g.iplab[ip] = a.label();
  g.l_exit = a.label();

  // --- 前口上 ---
  // 引数は機種の決まりで届く。積んであるぶんは、レジスタを押し込む前に読んでおく
#if defined(SHARK_JIT_WIN64)
  // rcx=VM* rdx=局所変数 r8=積んだ値の上 r9=飛び先。刻み・グローバル・限界は積んである
  a.ld(X_R10, X_SP, 40);
  a.ld(X_R11, X_SP, 48);
  a.ld(X_AX, X_SP, 56);
#else
  // rdi=VM* rsi=局所変数 rdx=積んだ値の上 rcx=飛び先 r8=刻み r9=グローバル、限界は積んである
  a.ld(X_AX, X_SP, 8);
#endif
  a.push(X_BX);
  a.push(X_BP);
  a.push(X_R12);
  a.push(X_R13);
  a.push(X_R14);
  a.push(X_R15);
  a.sub_imm(X_SP, kFrame);
  a.st(X_AX, X_SP, kLimAt);
#if defined(SHARK_JIT_WIN64)
  a.mov(R_VM, X_CX);
  a.mov(R_LOC, X_DX);
  a.mov(R_TOP, X_R8);
  a.mov(R_BUD, X_R10);
  a.mov(R_GLB, X_R11);
  a.mov_addr(R_KON, (uintptr_t)f->consts.data());
  a.jmp_r(X_R9);
#else
  a.mov(R_VM, X_DI);
  a.mov(R_LOC, X_SI);
  a.mov(R_TOP, X_DX);
  a.mov(R_BUD, X_R8);
  a.mov(R_GLB, X_R9);
  a.mov_addr(R_KON, (uintptr_t)f->consts.data());
  a.jmp_r(X_CX);
#endif

  // --- 本体 ---
  for (int ip = 0; ip < g.n;) {
    int len = op_operand_len(g.c[ip]);
    int next = ip + 1 + len;
    a.bind(g.iplab[ip]);
    if (sc.block[ip]) {
      if (sc.block[ip] == 2) {
        // ループの入口。刻みが尽きたか、値の置き場が心細くなったら仮想マシンに返す。
        // 置き場の限界は「この関数がもう1周ぶん積んでも収まる」ところに引いてあるので、
        // 中では確かめずに積んでよい（jit.cpp の max_push）
        int l_out = g.stub(1, ip, -1, ip);
        a.cmp_imm(R_BUD, 0);
        a.jcc(C_LE, l_out);
        a.ld(X_AX, X_SP, kLimAt);
        a.cmp(R_TOP, X_AX);
        a.jcc(C_A, l_out);
      }
      int nins = sc.ninst[ip];
      if (nins > 0) a.sub_imm(R_BUD, nins);
      // 「どのタスクも進めない」を数えるところ。進んだので 0 に戻す（vm.cpp と同じ）
      a.st32_imm(R_VM, g_lay.vm_steps, 0);
    }
    if (!emit_op(g, ip, len, next)) g.emit_fallback(ip, next);
    ip = next;
  }
  a.jmp(g.l_exit);   // 最後の命令から、後ろに置く道すじへ落ちないように

  // --- 後回しにした道すじ ---
  for (int i = 0; i < a.stubs.size(); i++) {
    Stub s = a.stubs[i];
    a.bind(s.lab);
    if (s.kind == 0) {
      int len = op_operand_len(g.c[s.op_ip]);
      g.emit_fallback(s.op_ip, s.op_ip + 1 + len);
      a.jmp(s.next_lab);
    } else if (s.kind == 1) {
      a.arg_reg(0, R_VM);
      a.arg_reg(1, R_TOP);
      a.arg_imm(2, s.arg);
      a.call_helper(H_SYNC);
      a.jmp(g.l_exit);
    } else {
      a.arg_reg(0, R_TOP);
      a.arg_imm(1, s.arg);
      a.call_helper(H_COMPARE);
      a.mov(R_TOP, X_AX);
      a.jmp(s.next_lab);
    }
  }

  // --- 出口 ---
  a.bind(g.l_exit);
  a.mov(X_AX, R_BUD);
  a.add_imm(X_SP, kFrame);
  a.pop(X_R15);
  a.pop(X_R14);
  a.pop(X_R13);
  a.pop(X_R12);
  a.pop(X_BP);
  a.pop(X_BX);
  a.ret();

  if (!a.ok || !a.resolve()) return false;

  // 命令の位置 -> 機械語の中の位置（バイト）
  offout->resize(g.n, -1);
  for (int ip = 0; ip < g.n; ip++)
    if (g.iplab[ip] >= 0) (*offout)[ip] = a.lpos[g.iplab[ip]];
  out->swap_with(a.w);
  return true;
}

}  // namespace

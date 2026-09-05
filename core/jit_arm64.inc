// jit_arm64.inc — ARM64（AArch64）の機械語を作る（core/jit.cpp から取り込む）
//
// 値の置き場（TaskState::stack）は仮想マシンとまったく同じものを使い、
// 積み方も変えない。そのため**どの命令の切れ目でも仮想マシンに戻れる**。
// 作り方の分からない命令は、その場で仮想マシンに1つだけ動かしてもらう。
//
// 呼び出しをまたいで持つもの（呼ばれた側が守るレジスタ）
//   x19 VM*                 x20 局所変数の先頭     x21 積んだ値の上（次に書く場所）
//   x22 残りの刻み（budget） x23 グローバルの先頭   x24 手伝いの関数の表
//   x25 定数の表             x26 値の置き場の限界（ループの入口で見る）
// x0〜x17 は使い捨て。手伝いの関数を呼ぶと消えるので、跨いで持たない。

namespace {

enum {
  R_VM = 19, R_LOC = 20, R_TOP = 21, R_BUD = 22, R_GLB = 23, R_HLP = 24, R_KON = 25, R_LIM = 26
};
enum { C_EQ = 0, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE };

// 呼び出しの前後で守るぶんの置き場（16 の倍数にする）
static const int kFrameSize = 80;

struct Fix {
  int at;     // 直す命令の位置
  int kind;   // 0 = b（26 ビット）、1 = b.cond / cbz（19 ビット）
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
  Vec<uint32_t> w;
  Vec<int32_t> lpos;
  Vec<Fix> fix;
  Vec<Stub> stubs;
  bool ok;

  Asm() : ok(true) {}

  int here() const { return w.size(); }
  void put(uint32_t x) { w.push(x); }
  int label() { lpos.push(-1); return lpos.size() - 1; }
  void bind(int l) { lpos[l] = w.size(); }

  // --- 分岐 ---
  void b(int l) { fix.push(Fix{here(), 0, l}); put(0x14000000u); }
  void bc(int cond, int l) { fix.push(Fix{here(), 1, l}); put(0x54000000u | (uint32_t)cond); }
  void cbz(int rt, int l) { fix.push(Fix{here(), 1, l}); put(0xB4000000u | (uint32_t)rt); }
  void cbnz(int rt, int l) { fix.push(Fix{here(), 1, l}); put(0xB5000000u | (uint32_t)rt); }
  void br(int n) { put(0xD61F0000u | ((uint32_t)n << 5)); }
  void blr(int n) { put(0xD63F0000u | ((uint32_t)n << 5)); }
  void ret() { put(0xD65F03C0u); }

  // --- 動かす ---
  void mov(int d, int m) { put(0xAA0003E0u | ((uint32_t)m << 16) | (uint32_t)d); }
  void movz(int d, uint32_t imm16, int hw) {
    put(0xD2800000u | ((uint32_t)hw << 21) | (imm16 << 5) | (uint32_t)d);
  }
  void movk(int d, uint32_t imm16, int hw) {
    put(0xF2800000u | ((uint32_t)hw << 21) | (imm16 << 5) | (uint32_t)d);
  }
  void movn(int d, uint32_t imm16, int hw) {
    put(0x92800000u | ((uint32_t)hw << 21) | (imm16 << 5) | (uint32_t)d);
  }
  void mov_imm(int d, uint64_t v) {
    if (v <= 0xFFFFu) { movz(d, (uint32_t)v, 0); return; }
    uint64_t nv = ~v;
    if (nv <= 0xFFFFu) { movn(d, (uint32_t)nv, 0); return; }
    bool first = true;
    for (int hw = 0; hw < 4; hw++) {
      uint32_t p = (uint32_t)((v >> (hw * 16)) & 0xFFFFu);
      if (p == 0 && !first) continue;
      if (p == 0 && first) continue;
      if (first) { movz(d, p, hw); first = false; }
      else movk(d, p, hw);
    }
    if (first) movz(d, 0, 0);
  }

  // --- 足し引き・比べる ---
  void add_imm(int d, int n, int imm) {
    put(0x91000000u | ((uint32_t)imm << 10) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void sub_imm(int d, int n, int imm) {
    put(0xD1000000u | ((uint32_t)imm << 10) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void add_imm32(int d, int n, int imm) {
    put(0x11000000u | ((uint32_t)imm << 10) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void sub_imm32(int d, int n, int imm) {
    put(0x51000000u | ((uint32_t)imm << 10) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void cmp_imm(int n, int imm) {
    put(0xF1000000u | ((uint32_t)imm << 10) | ((uint32_t)n << 5) | 31u);
  }
  void cmp_imm32(int n, int imm) {
    put(0x71000000u | ((uint32_t)imm << 10) | ((uint32_t)n << 5) | 31u);
  }
  void cmn_imm(int n, int imm) {   // n + imm を見る（n == -imm なら等しい）
    put(0xB1000000u | ((uint32_t)imm << 10) | ((uint32_t)n << 5) | 31u);
  }
  void cmp(int n, int m) { put(0xEB000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | 31u); }
  void cmp_asr(int n, int m, int sh) {   // cmp Xn, Xm, ASR #sh
    put(0xEB800000u | ((uint32_t)m << 16) | ((uint32_t)sh << 10) | ((uint32_t)n << 5) | 31u);
  }
  void add_reg(int d, int n, int m) {
    put(0x8B000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void add_lsl(int d, int n, int m, int sh) {   // add Xd, Xn, Xm, LSL #sh
    put(0x8B000000u | ((uint32_t)m << 16) | ((uint32_t)sh << 10) | ((uint32_t)n << 5) |
        (uint32_t)d);
  }
  void adds(int d, int n, int m) {
    put(0xAB000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void subs(int d, int n, int m) {
    put(0xEB000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void negs(int d, int m) { subs(d, 31, m); }
  void mul(int d, int n, int m) {
    put(0x9B000000u | ((uint32_t)m << 16) | (31u << 10) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void madd(int d, int n, int m, int acc) {
    put(0x9B000000u | ((uint32_t)m << 16) | ((uint32_t)acc << 10) | ((uint32_t)n << 5) |
        (uint32_t)d);
  }
  void msub(int d, int n, int m, int acc) {
    put(0x9B008000u | ((uint32_t)m << 16) | ((uint32_t)acc << 10) | ((uint32_t)n << 5) |
        (uint32_t)d);
  }
  void smulh(int d, int n, int m) {
    put(0x9B407C00u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void sdiv(int d, int n, int m) {
    put(0x9AC00C00u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void and_reg(int d, int n, int m) {
    put(0x8A000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void orr_reg(int d, int n, int m) {
    put(0xAA000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void eor_reg(int d, int n, int m) {
    put(0xCA000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void lslv(int d, int n, int m) {
    put(0x9AC02000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void asrv(int d, int n, int m) {
    put(0x9AC02800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void mvn(int d, int m) { put(0xAA2003E0u | ((uint32_t)m << 16) | (uint32_t)d); }
  void cset(int d, int cond) {
    put(0x9A9F07E0u | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)d);
  }
  void and_imm8(int d, int n) {   // and Wd, Wn, #0xff
    put(0x12001C00u | ((uint32_t)n << 5) | (uint32_t)d);
  }

  // --- 読み書き ---
  // 収まらないずれは x8 に住所を作ってから使う（x8 は使い捨て）
  void addr(int d, int base, int off) {
    if (off >= 0 && off <= 4095) { add_imm(d, base, off); return; }
    if (off < 0 && -off <= 4095) { sub_imm(d, base, -off); return; }
    mov_imm(d, (uint64_t)(int64_t)off);
    add_reg(d, base, d);
  }
  void ldp_off(int t, int t2, int n, int imm) {
    put(0xA9400000u | (((uint32_t)((imm / 8) & 0x7f)) << 15) | ((uint32_t)t2 << 10) |
        ((uint32_t)n << 5) | (uint32_t)t);
  }
  void stp_off(int t, int t2, int n, int imm) {
    put(0xA9000000u | (((uint32_t)((imm / 8) & 0x7f)) << 15) | ((uint32_t)t2 << 10) |
        ((uint32_t)n << 5) | (uint32_t)t);
  }
  void stp_post(int t, int t2, int n, int imm) {
    put(0xA8800000u | (((uint32_t)((imm / 8) & 0x7f)) << 15) | ((uint32_t)t2 << 10) |
        ((uint32_t)n << 5) | (uint32_t)t);
  }
  void stp_pre(int t, int t2, int n, int imm) {
    put(0xA9800000u | (((uint32_t)((imm / 8) & 0x7f)) << 15) | ((uint32_t)t2 << 10) |
        ((uint32_t)n << 5) | (uint32_t)t);
  }
  void ldp_post(int t, int t2, int n, int imm) {
    put(0xA8C00000u | (((uint32_t)((imm / 8) & 0x7f)) << 15) | ((uint32_t)t2 << 10) |
        ((uint32_t)n << 5) | (uint32_t)t);
  }
  bool pair_ok(int off) const { return off >= -512 && off <= 504 && (off % 8) == 0; }
  void ldp_at(int t, int t2, int base, int off) {
    if (pair_ok(off)) { ldp_off(t, t2, base, off); return; }
    addr(8, base, off);
    ldp_off(t, t2, 8, 0);
  }
  void stp_at(int t, int t2, int base, int off) {
    if (pair_ok(off)) { stp_off(t, t2, base, off); return; }
    addr(8, base, off);
    stp_off(t, t2, 8, 0);
  }
  // 単発の読み書き。scaled（正で割り切れる）→ unscaled（±256）→ 住所を作る、の順
  void mem1(uint32_t sc, uint32_t un, int size, int t, int n, int off) {
    if (off >= 0 && (off % size) == 0 && (off / size) <= 4095) {
      put(sc | ((uint32_t)(off / size) << 10) | ((uint32_t)n << 5) | (uint32_t)t);
      return;
    }
    if (off >= -256 && off <= 255) {
      put(un | (((uint32_t)off & 0x1ffu) << 12) | ((uint32_t)n << 5) | (uint32_t)t);
      return;
    }
    addr(8, n, off);
    put(sc | ((uint32_t)8 << 5) | (uint32_t)t);
  }
  void ld64(int t, int n, int off) { mem1(0xF9400000u, 0xF8400000u, 8, t, n, off); }
  void st64(int t, int n, int off) { mem1(0xF9000000u, 0xF8000000u, 8, t, n, off); }
  void ld32(int t, int n, int off) { mem1(0xB9400000u, 0xB8400000u, 4, t, n, off); }
  void st32(int t, int n, int off) { mem1(0xB9000000u, 0xB8000000u, 4, t, n, off); }
  void ldsw(int t, int n, int off) { mem1(0xB9800000u, 0xB8800000u, 4, t, n, off); }
  void ldb(int t, int n, int off) { mem1(0x39400000u, 0x38400000u, 1, t, n, off); }
  void ldr_d(int t, int n, int off) { mem1(0xFD400000u, 0xFC400000u, 8, t, n, off); }
  void str_d(int t, int n, int off) { mem1(0xFD000000u, 0xFC000000u, 8, t, n, off); }

  // --- 小数（double）---
  void fadd(int d, int n, int m) {
    put(0x1E602800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void fsub(int d, int n, int m) {
    put(0x1E603800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void fmul(int d, int n, int m) {
    put(0x1E600800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void fdiv(int d, int n, int m) {
    put(0x1E601800u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
  }
  void fneg(int d, int n) { put(0x1E614000u | ((uint32_t)n << 5) | (uint32_t)d); }

  // --- 手伝いの関数を呼ぶ ---
  void call_helper(int slot) {
    ld64(16, R_HLP, slot * 8);
    blr(16);
  }

  // 飛び先を埋める
  bool resolve() {
    for (int i = 0; i < fix.size(); i++) {
      int to = lpos[fix[i].lab];
      if (to < 0) return false;
      int off = to - fix[i].at;
      if (fix[i].kind == 0) {
        if (off > 0x1FFFFFF || off < -0x2000000) return false;
        w[fix[i].at] |= (uint32_t)off & 0x03FFFFFFu;
      } else {
        if (off > 0x3FFFF || off < -0x40000) return false;
        w[fix[i].at] |= ((uint32_t)off & 0x7FFFFu) << 5;
      }
    }
    return true;
  }
};

// 上から i 番目（0 が一番上）の値の場所
static int slot_at(int i) { return -16 * (i + 1); }

// 参照を1つ増やす（obj のときだけ呼ぶ。o は場所を指すレジスタ）
static void emit_retain(Asm& a, int o, int tmp) {
  a.ld32(tmp, o, g_lay.obj_rc);
  a.add_imm32(tmp, tmp, 1);
  a.st32(tmp, o, g_lay.obj_rc);
}

// 参照を1つ減らす。最後の1つなら片付けを頼む（x0・x2・x16 と戻り先が消える）
static void emit_release(Asm& a, int o) {
  int l_slow = a.label(), l_done = a.label();
  a.ld32(2, o, g_lay.obj_rc);
  a.cmp_imm32(2, 1);
  a.bc(C_LE, l_slow);
  a.sub_imm32(2, 2, 1);
  a.st32(2, o, g_lay.obj_rc);
  a.b(l_done);
  a.bind(l_slow);
  if (o != 0) a.mov(0, o);
  a.call_helper(H_RELEASE);
  a.bind(l_done);
}

// 値を1つ積む（rk = 種類、rp = 中身）
static void emit_push(Asm& a, int rk, int rp) { a.stp_post(rk, rp, R_TOP, 16); }

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
    a.mov(0, R_VM);
    a.mov_imm(1, (uint64_t)(uint32_t)op_ip);
    a.mov_imm(2, (uint64_t)(uint32_t)next_ip);
    a.mov(3, R_TOP);
    a.call_helper(H_FALLBACK);
    a.cbz(0, l_exit);
    a.mov(R_TOP, 0);
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
        a.ldp_at(0, 1, R_KON, arg * 16);
        emit_retain(a, 1, 2);
      } else {
        a.mov_imm(0, (uint64_t)(uint8_t)v.k);
        a.mov_imm(1, (uint64_t)v.i);
      }
      emit_push(a, 0, 1);
      return true;
    }
    case OP_NONE: case OP_VOID: case OP_TRUE: case OP_FALSE: {
      int kind = op == OP_NONE ? V_None : op == OP_VOID ? V_Void : V_Bool;
      a.mov_imm(0, (uint64_t)kind);
      a.mov_imm(1, op == OP_TRUE ? 1u : 0u);
      emit_push(a, 0, 1);
      return true;
    }
    case OP_POP: {
      int l_skip = a.label();
      a.ldb(0, R_TOP, slot_at(0));
      a.sub_imm(R_TOP, R_TOP, 16);
      a.cmp_imm32(0, V_Obj);
      a.bc(C_NE, l_skip);
      a.ld64(1, R_TOP, 8);
      emit_release(a, 1);
      a.bind(l_skip);
      return true;
    }
    case OP_DUP: {
      int l_st = a.label();
      a.ldp_at(0, 1, R_TOP, slot_at(0));
      a.and_imm8(2, 0);
      a.cmp_imm32(2, V_Obj);
      a.bc(C_NE, l_st);
      emit_retain(a, 1, 3);
      a.bind(l_st);
      emit_push(a, 0, 1);
      return true;
    }
    case OP_SWAP: {
      a.ldp_at(0, 1, R_TOP, slot_at(1));
      a.ldp_at(2, 3, R_TOP, slot_at(0));
      a.stp_at(2, 3, R_TOP, slot_at(1));
      a.stp_at(0, 1, R_TOP, slot_at(0));
      return true;
    }

    case OP_LOAD_LOCAL: case OP_LOAD_GLOBAL: {
      bool loc = op == OP_LOAD_LOCAL;
      if (loc && (arg < 0 || arg >= f->nlocals)) return false;
      if (arg < 0) return false;
      int l_st = a.label();
      a.ldp_at(0, 1, loc ? R_LOC : R_GLB, arg * 16);
      a.and_imm8(2, 0);
      a.cmp_imm32(2, V_Obj);
      a.bc(C_LO, l_st);
      if (loc) {
        // ref 引数（借用）は仮想マシンに任せる
        a.bc(C_HI, g.stub(0, ip, next_lab, 0));
      } else {
        a.bc(C_HI, l_st);
      }
      emit_retain(a, 1, 3);
      a.bind(l_st);
      emit_push(a, 0, 1);
      return true;
    }
    case OP_STORE_LOCAL: case OP_STORE_GLOBAL: {
      bool loc = op == OP_STORE_LOCAL;
      if (loc && (arg < 0 || arg >= f->nlocals)) return false;
      if (arg < 0) return false;
      int base = loc ? R_LOC : R_GLB;
      int l_done = a.label();
      a.ldp_at(0, 1, R_TOP, slot_at(0));
      a.ldp_at(2, 3, base, arg * 16);
      a.and_imm8(4, 2);
      if (loc) {
        a.cmp_imm32(4, V_Ref);
        a.bc(C_EQ, g.stub(0, ip, next_lab, 0));
      }
      a.sub_imm(R_TOP, R_TOP, 16);
      a.stp_at(0, 1, base, arg * 16);
      a.cmp_imm32(4, V_Obj);
      a.bc(C_NE, l_done);
      emit_release(a, 3);
      a.bind(l_done);
      return true;
    }

    case OP_LOAD_FIELD: {
      if (arg < 0) return false;
      int l_fb = g.stub(0, ip, next_lab, 0);
      int l_st = a.label();
      a.ldb(0, R_TOP, slot_at(0));
      a.cmp_imm32(0, V_Obj);
      a.bc(C_NE, l_fb);
      a.ld64(1, R_TOP, slot_at(0) + 8);        // Obj*
      a.ldb(2, 1, g_lay.obj_kind);
      a.cmp_imm32(2, O_Inst);
      a.bc(C_NE, l_fb);
      a.ldsw(3, 1, g_lay.inst_fields + g_lay.vec_size);
      a.cmp_imm(3, arg);
      a.bc(C_LE, l_fb);                        // 数が足りない（壊れている）
      a.ld64(4, 1, g_lay.inst_fields + g_lay.vec_data);
      a.ldp_at(5, 6, 4, arg * 16);
      a.and_imm8(7, 5);
      a.cmp_imm32(7, V_Obj);
      a.bc(C_NE, l_st);
      emit_retain(a, 6, 7);
      a.bind(l_st);
      a.stp_at(5, 6, R_TOP, slot_at(0));       // 実体のあった場所に結果を置く
      emit_release(a, 1);                      // 実体を離す（結果は先に積んである）
      return true;
    }

    case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT:
    case OP_DIV_INT: case OP_MOD_INT: {
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ld64(0, R_TOP, slot_at(1) + 8);
      a.ld64(1, R_TOP, slot_at(0) + 8);
      if (op == OP_ADD_INT) {
        a.adds(2, 0, 1);
        a.bc(C_VS, l_fb);
      } else if (op == OP_SUB_INT) {
        a.subs(2, 0, 1);
        a.bc(C_VS, l_fb);
      } else if (op == OP_MUL_INT) {
        a.mul(2, 0, 1);
        a.smulh(3, 0, 1);
        a.cmp_asr(3, 2, 63);
        a.bc(C_NE, l_fb);
      } else {
        a.cbz(1, l_fb);        // 0 で割る
        a.cmn_imm(1, 1);       // -1 で割るのは、あふれることがあるので任せる
        a.bc(C_EQ, l_fb);
        a.sdiv(2, 0, 1);
        if (op == OP_MOD_INT) a.msub(2, 2, 1, 0);
      }
      a.mov_imm(3, V_Int);
      a.stp_at(3, 2, R_TOP, slot_at(1));
      a.sub_imm(R_TOP, R_TOP, 16);
      return true;
    }
    case OP_NEG_INT: {
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ld64(0, R_TOP, slot_at(0) + 8);
      a.negs(2, 0);
      a.bc(C_VS, l_fb);
      a.mov_imm(3, V_Int);
      a.stp_at(3, 2, R_TOP, slot_at(0));
      return true;
    }
    case OP_AND_INT: case OP_OR_INT: case OP_XOR_INT: {
      a.ld64(0, R_TOP, slot_at(1) + 8);
      a.ld64(1, R_TOP, slot_at(0) + 8);
      if (op == OP_AND_INT) a.and_reg(2, 0, 1);
      else if (op == OP_OR_INT) a.orr_reg(2, 0, 1);
      else a.eor_reg(2, 0, 1);
      a.mov_imm(3, V_Int);
      a.stp_at(3, 2, R_TOP, slot_at(1));
      a.sub_imm(R_TOP, R_TOP, 16);
      return true;
    }
    case OP_SHL_INT: case OP_SHR_INT: {
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ld64(0, R_TOP, slot_at(1) + 8);
      a.ld64(1, R_TOP, slot_at(0) + 8);
      a.cmp_imm(1, 63);
      a.bc(C_HI, l_fb);      // 0〜63 の外（負の数も、符号なしで見れば大きいので入る）
      if (op == OP_SHL_INT) a.lslv(2, 0, 1);
      else a.asrv(2, 0, 1);  // 右へは符号を保ったままずらす（仮想マシンと同じ）
      a.mov_imm(3, V_Int);
      a.stp_at(3, 2, R_TOP, slot_at(1));
      a.sub_imm(R_TOP, R_TOP, 16);
      return true;
    }
    case OP_BNOT_INT: {
      a.ld64(0, R_TOP, slot_at(0) + 8);
      a.mvn(2, 0);
      a.mov_imm(3, V_Int);
      a.stp_at(3, 2, R_TOP, slot_at(0));
      return true;
    }

    case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT: {
      a.ldr_d(0, R_TOP, slot_at(1) + 8);
      a.ldr_d(1, R_TOP, slot_at(0) + 8);
      if (op == OP_ADD_FLOAT) a.fadd(2, 0, 1);
      else if (op == OP_SUB_FLOAT) a.fsub(2, 0, 1);
      else if (op == OP_MUL_FLOAT) a.fmul(2, 0, 1);
      else a.fdiv(2, 0, 1);
      a.mov_imm(3, V_Float);
      a.st64(3, R_TOP, slot_at(1));
      a.str_d(2, R_TOP, slot_at(1) + 8);
      a.sub_imm(R_TOP, R_TOP, 16);
      return true;
    }
    case OP_NEG_FLOAT: {
      a.ldr_d(0, R_TOP, slot_at(0) + 8);
      a.fneg(2, 0);
      a.mov_imm(3, V_Float);
      a.st64(3, R_TOP, slot_at(0));
      a.str_d(2, R_TOP, slot_at(0) + 8);
      return true;
    }

    case OP_NOT: {
      int l_st = a.label();
      a.ldb(0, R_TOP, slot_at(0));
      a.mov_imm(1, 1);
      a.cmp_imm32(0, V_Bool);
      a.bc(C_NE, l_st);
      a.ld64(2, R_TOP, slot_at(0) + 8);
      a.cmp_imm(2, 0);
      a.cset(1, C_EQ);
      a.bind(l_st);
      a.mov_imm(0, V_Bool);
      a.stp_at(0, 1, R_TOP, slot_at(0));
      return true;
    }
    case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
      int l_slow = g.stub(2, ip, next_lab, op);
      a.ldb(0, R_TOP, slot_at(1));
      a.cmp_imm32(0, V_Int);
      a.bc(C_NE, l_slow);
      a.ldb(1, R_TOP, slot_at(0));
      a.cmp_imm32(1, V_Int);
      a.bc(C_NE, l_slow);
      a.ld64(2, R_TOP, slot_at(1) + 8);
      a.ld64(3, R_TOP, slot_at(0) + 8);
      a.cmp(2, 3);
      int cond = op == OP_EQ ? C_EQ : op == OP_NE ? C_NE : op == OP_LT ? C_LT
                 : op == OP_LE ? C_LE : op == OP_GT ? C_GT : C_GE;
      a.cset(4, cond);
      a.mov_imm(5, V_Bool);
      a.stp_at(5, 4, R_TOP, slot_at(1));
      a.sub_imm(R_TOP, R_TOP, 16);
      return true;
    }

    case OP_JUMP: {
      if (arg < 0 || arg >= g.n) return false;
      a.b(g.iplab[arg]);
      return true;
    }
    case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
    case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP: {
      if (arg < 0 || arg >= g.n) return false;
      bool keep = (op == OP_JUMP_IF_FALSE_KEEP || op == OP_JUMP_IF_TRUE_KEEP);
      bool on_true = (op == OP_JUMP_IF_TRUE || op == OP_JUMP_IF_TRUE_KEEP);
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ldb(0, R_TOP, slot_at(0));
      a.cmp_imm32(0, V_Bool);
      a.bc(C_NE, l_fb);                 // bool 以外は仮想マシンに任せる
      a.ld64(1, R_TOP, slot_at(0) + 8);
      if (!keep) a.sub_imm(R_TOP, R_TOP, 16);
      if (on_true) a.cbnz(1, g.iplab[arg]);
      else a.cbz(1, g.iplab[arg]);
      return true;
    }
    case OP_JUMP_IF_NONE: case OP_JUMP_IF_NOT_NONE: {
      if (arg < 0 || arg >= g.n) return false;
      int l_fall = a.label();
      a.ldb(0, R_TOP, slot_at(0));
      a.cmp_imm32(0, V_None);
      if (op == OP_JUMP_IF_NONE) {
        a.bc(C_NE, l_fall);             // 値があるならそのまま
        a.sub_imm(R_TOP, R_TOP, 16);    // none は捨てて飛ぶ
        a.b(g.iplab[arg]);
      } else {
        a.bc(C_EQ, l_fall);
        a.b(g.iplab[arg]);              // 値があるならそのまま飛ぶ
      }
      a.bind(l_fall);
      if (op == OP_JUMP_IF_NOT_NONE) a.sub_imm(R_TOP, R_TOP, 16);
      return true;
    }

    case OP_ITER_NEW: {
      a.mov_imm(0, V_Int);
      a.mov_imm(1, 0);
      emit_push(a, 0, 1);
      return true;
    }
    case OP_ITER_NEXT: {
      // 数の並び（range）だけをここで回す。ほかの入れ物と、終わりの後始末は仮想マシンへ
      if (arg < 0 || arg >= g.n) return false;
      int l_fb = g.stub(0, ip, next_lab, 0);
      a.ldb(0, R_TOP, slot_at(1));
      a.cmp_imm32(0, V_Obj);
      a.bc(C_NE, l_fb);
      a.ld64(1, R_TOP, slot_at(1) + 8);
      a.ldb(2, 1, g_lay.obj_kind);
      a.cmp_imm32(2, O_Range);
      a.bc(C_NE, l_fb);
      a.ld64(3, R_TOP, slot_at(0) + 8);       // いくつ目か
      a.ld64(4, 1, g_lay.range_start);
      a.ld64(5, 1, g_lay.range_end);
      a.ld64(6, 1, g_lay.range_step);
      a.madd(7, 3, 6, 4);                     // v = start + i * step
      int l_neg = a.label(), l_have = a.label();
      a.cmp_imm(6, 0);
      a.bc(C_LE, l_neg);
      a.cmp(7, 5);
      a.bc(C_GE, l_fb);                       // 終わり。後始末は仮想マシンに任せる
      a.b(l_have);
      a.bind(l_neg);
      a.cmp(7, 5);
      a.bc(C_LE, l_fb);
      a.bind(l_have);
      a.add_imm(3, 3, 1);
      a.st64(3, R_TOP, slot_at(0) + 8);
      a.mov_imm(0, V_Int);
      emit_push(a, 0, 7);
      return true;
    }

    case OP_INDEX_GET: {
      // 配列と int のときだけ。ほかは仮想マシンへ（つまずく場合もあちらが出す）
      int l_fb = g.stub(0, ip, next_lab, 0);
      int l_st = a.label();
      a.ldb(0, R_TOP, slot_at(1));
      a.cmp_imm32(0, V_Obj);
      a.bc(C_NE, l_fb);
      a.ld64(1, R_TOP, slot_at(1) + 8);       // 入れ物
      a.ldb(2, 1, g_lay.obj_kind);
      a.cmp_imm32(2, O_List);
      a.bc(C_NE, l_fb);
      a.ldb(3, R_TOP, slot_at(0));
      a.cmp_imm32(3, V_Int);
      a.bc(C_NE, l_fb);
      a.ld64(4, R_TOP, slot_at(0) + 8);       // 添字
      a.ldsw(5, 1, g_lay.list_vec + g_lay.vec_size);
      a.cmp(4, 5);
      a.bc(C_HS, l_fb);                       // 範囲の外（負の数もここで外れる）
      a.ld64(6, 1, g_lay.list_vec + g_lay.vec_data);
      a.add_lsl(6, 6, 4, 4);
      a.ldp_off(9, 10, 6, 0);
      a.and_imm8(11, 9);
      a.cmp_imm32(11, V_Obj);
      a.bc(C_NE, l_st);
      emit_retain(a, 10, 11);
      a.bind(l_st);
      a.sub_imm(R_TOP, R_TOP, 16);
      a.stp_at(9, 10, R_TOP, slot_at(0));     // 入れ物のあった場所に結果を置く
      emit_release(a, 1);                     // 入れ物を離す
      return true;
    }

    // --- 呼び出し ---
    case OP_CALL: {
      int nargs = rd_i32(c, ip + 5);
      a.mov(0, R_VM);
      a.mov(1, R_TOP);
      a.mov_imm(2, (uint64_t)(uint32_t)arg);
      a.mov_imm(3, (uint64_t)(uint32_t)nargs);
      a.mov_imm(4, (uint64_t)(uint32_t)next);
      a.call_helper(H_CALL);
      a.cbz(0, g.stub(0, ip, next_lab, 0));   // 呼べなければ仮想マシンに任せる
      a.b(g.l_exit);                          // 枠が変わったので、いったん戻る
      return true;
    }
    case OP_CALL_NATIVE: {
      int nargs = rd_i32(c, ip + 5);
      a.mov(0, R_VM);
      a.mov(1, R_TOP);
      a.mov_imm(2, (uint64_t)(uint32_t)arg);
      a.mov_imm(3, (uint64_t)(uint32_t)nargs);
      a.mov_imm(4, (uint64_t)(uint32_t)ip);
      a.mov_imm(5, (uint64_t)(uint32_t)next);
      a.call_helper(H_CALL_NATIVE);
      a.cbz(0, g.l_exit);
      a.mov(R_TOP, 0);
      return true;
    }
    case OP_RET: case OP_RET_VOID: {
      a.mov(0, R_VM);
      a.mov(1, R_TOP);
      a.mov_imm(2, op == OP_RET ? 1u : 0u);
      a.call_helper(H_RET);
      a.b(g.l_exit);
      return true;
    }

    default:
      return false;
  }
}

// 関数を1つ、機械語にする。ARM64 の命令はどれも 4 バイトだが、
// 渡すのは機種によらないバイトの並び（core/jit.cpp）なので、最後にほどく
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
  // x0=VM* x1=局所変数 x2=積んだ値の上 x3=飛び先 x4=刻み x5=グローバル x6=置き場の限界
  a.stp_pre(29, 30, 31, -kFrameSize);
  a.stp_off(R_VM, R_LOC, 31, 16);
  a.stp_off(R_TOP, R_BUD, 31, 32);
  a.stp_off(R_GLB, R_HLP, 31, 48);
  a.stp_off(R_KON, R_LIM, 31, 64);
  a.mov(R_VM, 0);
  a.mov(R_LOC, 1);
  a.mov(R_TOP, 2);
  a.mov(R_BUD, 4);
  a.mov(R_GLB, 5);
  a.mov(R_LIM, 6);
  a.mov_imm(R_HLP, (uint64_t)(uintptr_t)g_helpers);
  a.mov_imm(R_KON, (uint64_t)(uintptr_t)f->consts.data());
  a.br(3);

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
        a.bc(C_LE, l_out);
        a.cmp(R_TOP, R_LIM);
        a.bc(C_HI, l_out);
      }
      int nins = sc.ninst[ip];
      if (nins > 0 && nins <= 4095) a.sub_imm(R_BUD, R_BUD, nins);
      // 「どのタスクも進めない」を数えるところ。進んだので 0 に戻す（vm.cpp と同じ）
      a.st32(31, R_VM, g_lay.vm_steps);
    }
    if (!emit_op(g, ip, len, next)) g.emit_fallback(ip, next);
    ip = next;
  }
  a.b(g.l_exit);   // 最後の命令から、後ろに置く道すじへ落ちないように

  // --- 後回しにした道すじ ---
  for (int i = 0; i < a.stubs.size(); i++) {
    Stub s = a.stubs[i];
    a.bind(s.lab);
    if (s.kind == 0) {
      int len = op_operand_len(g.c[s.op_ip]);
      g.emit_fallback(s.op_ip, s.op_ip + 1 + len);
      a.b(s.next_lab);
    } else if (s.kind == 1) {
      a.mov(0, R_VM);
      a.mov(1, R_TOP);
      a.mov_imm(2, (uint64_t)(uint32_t)s.arg);
      a.call_helper(H_SYNC);
      a.b(g.l_exit);
    } else {
      a.mov(0, R_TOP);
      a.mov_imm(1, (uint64_t)(uint32_t)s.arg);
      a.call_helper(H_COMPARE);
      a.mov(R_TOP, 0);
      a.b(s.next_lab);
    }
  }

  // --- 出口 ---
  a.bind(g.l_exit);
  a.mov(0, R_BUD);
  a.ldp_off(R_KON, R_LIM, 31, 64);
  a.ldp_off(R_GLB, R_HLP, 31, 48);
  a.ldp_off(R_TOP, R_BUD, 31, 32);
  a.ldp_off(R_VM, R_LOC, 31, 16);
  a.ldp_post(29, 30, 31, kFrameSize);
  a.ret();

  if (!a.ok || !a.resolve()) return false;

  // 命令の位置 -> 機械語の中の位置（バイト）
  offout->resize(g.n, -1);
  for (int ip = 0; ip < g.n; ip++)
    if (g.iplab[ip] >= 0) (*offout)[ip] = a.lpos[g.iplab[ip]] * 4;
  for (int i = 0; i < a.w.size(); i++)
    for (int k = 0; k < 4; k++) out->push((uint8_t)(a.w[i] >> (k * 8)));
  return true;
}

}  // namespace

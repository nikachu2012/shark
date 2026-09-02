// jit.h — 実行時コンパイル（spec/runtime/execution.md）
//
// 何度も通ったところだけを機械語にして、そこから先はそれを走らせる。
//
// ・**任意機能**。機械語を作れない機種（と移植層に動的コードが無い機種）では
//   何もせず、常に仮想マシンで実行する。どちらで実行しても結果は同じ
// ・機械語にするのは関数まるごと。作り方の分からない命令は、その場で
//   仮想マシンに1命令だけ動かしてもらい、続きへ戻る（意味は必ず一致する）
// ・数えるのは「呼ばれた回数」と「ループの回転数」。しきい値を超えた関数だけ作る
#ifndef SHARK_JIT_H
#define SHARK_JIT_H

#include "program.h"

namespace shark {

struct VM;
struct TaskState;
struct Frame;
struct JitState;

// この機種で機械語を作れるか（作る側と、移植層の動的コードの両方が要る）
bool jit_available();

JitState* jit_new();
void jit_free(JitState* j);
// 覚えている機械語を捨てる（別のプログラムを読み込む前に呼ぶ）
void jit_clear(JitState* j);

// 関数が1つぶん熱くなったことを知らせる。機械語にしたら true。
// 呼び出しのたびと、ループが1回転するたびに呼ぶ
bool jit_hot(JitState* j, VM& vm, FuncInfo* f);

// いまの枠を機械語で走らせる。走らせたら true（budget を減らして返す）。
// 機械語が無い・入れないときは false（呼んだ側が仮想マシンで動かす）
bool jit_run(JitState* j, VM& vm, TaskState* t, Frame& fr, int* budget);

}  // namespace shark
#endif

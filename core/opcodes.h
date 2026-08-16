// opcodes.h — 仮想マシンの命令（spec/runtime/execution.md）
//
// スタック型。命令コードは1バイト、オペランドは 4 バイトの符号付き整数。
// 命令セットはプラットフォームに依存しない。同じバイトコードはどこでも同じ結果になる。
#ifndef SHARK_OPCODES_H
#define SHARK_OPCODES_H

namespace shark {

enum Op {
  OP_NOP = 0,
  OP_CONST,        // k        定数を積む
  OP_NONE, OP_VOID, OP_TRUE, OP_FALSE,
  OP_POP, OP_DUP,
  OP_SWAP,         //          上の2つを入れ替える
  OP_ROT_UNDER,    // n        いちばん上の値を、n 個下に差し込む

  OP_LOAD_LOCAL,   // i
  OP_STORE_LOCAL,  // i
  OP_LOAD_GLOBAL,  // i
  OP_STORE_GLOBAL, // i
  OP_LOAD_FIELD,   // i        [obj] -> [値]
  OP_REF_LOCAL,    // i        ref 引数として借用を積む

  // 代入先（place）。書き込み時コピーはここで起きる
  OP_PLACE_LOCAL,  // i
  OP_PLACE_GLOBAL, // i
  OP_PLACE_FIELD,  // i        [place] -> [place]
  OP_PLACE_INDEX,  //          [place, 添字] -> [place]
  OP_PLACE_LOAD,   //          place の中身を積む
  OP_PLACE_STORE,  //          [place, 値] -> 代入
  OP_PLACE_DUP,
  OP_PLACE_REF,    //          place を ref（借用）にして値スタックへ

  // 計算
  OP_ADD_INT, OP_SUB_INT, OP_MUL_INT, OP_DIV_INT, OP_MOD_INT, OP_NEG_INT,
  OP_ADD_FLOAT, OP_SUB_FLOAT, OP_MUL_FLOAT, OP_DIV_FLOAT, OP_NEG_FLOAT,
  OP_CONCAT,       // n        n 個の文字列をつなぐ
  OP_ADD_TIME_DUR, OP_SUB_TIME_DUR, OP_SUB_TIME_TIME, OP_ADD_DUR_DUR, OP_SUB_DUR_DUR,
  OP_NOT,
  OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
  OP_CMP_DYN,      //          Comparable の compare（ジェネリクスで使う）

  // 分岐
  OP_JUMP,             // a
  OP_JUMP_IF_FALSE,    // a
  OP_JUMP_IF_TRUE,     // a
  OP_JUMP_IF_FALSE_KEEP,  // a  値を残したまま（&& ||）
  OP_JUMP_IF_TRUE_KEEP,   // a
  OP_JUMP_IF_NONE,     // a    none なら捨てて飛ぶ。値があればそのまま
  OP_JUMP_IF_NOT_NONE, // a
  OP_JUMP_IF_ERR,      // a    Result が失敗なら飛ぶ（値はそのまま）
  OP_UNWRAP_OK,        //      Result -> 中身
  OP_UNWRAP_ERR,       //      Result -> Error
  OP_FORCE,            //      T? -> T（none なら panic）

  // 呼び出し
  OP_CALL,          // f, n
  OP_CALL_NATIVE,   // id, n
  OP_CALL_VIRTUAL,  // slot, n
  OP_CALL_VALUE,    // n
  OP_RET, OP_RET_VOID,

  // 生成
  OP_NEW_LIST,   // n
  OP_NEW_MAP,    // n（キーと値が交互に積んである）
  OP_NEW_INST,   // cls
  OP_MAKE_OK, OP_MAKE_ERR,

  // 添字
  OP_INDEX_GET,

  // 繰り返し
  OP_ITER_NEW,
  OP_ITER_NEXT,  // a  終わりなら a へ

  // 並行
  OP_TASK,       // f, n   タスクとして走らせる
  OP_PARALLEL,   // n      n 個の Task を全部待ち、list<T> にする

  OP_PANIC,
  OP_HALT,
};

// 呼び出しの種類（型検査が決め、コード生成が読む）
enum CallKind {
  CK_None = 0,
  CK_Func,       // 普通の関数・メソッド（静的）
  CK_Native,     // 処理系が持つ関数
  CK_Virtual,    // virtual メソッド
  CK_Ctor,       // クラスの生成
  CK_Value,      // 関数を値として受け取ったもの
  CK_Convert,    // 型変換（int(x) など）
  CK_CmpDyn,     // ジェネリクスの compare
};

}  // namespace shark
#endif

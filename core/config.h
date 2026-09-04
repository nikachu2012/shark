// config.h — 処理系の設定
//
// ソースから作るとき（Engine、shark.h）と、保存したバイトコードを動かすとき
// （Runtime、runtime.h）で同じものを使う。入れるモジュールの組み合わせが
// 関数の表の並びを決めるので（registry.h）、両方で揃えないと番号が食い違う。
#ifndef SHARK_CONFIG_H
#define SHARK_CONFIG_H

#include "diag.h"

namespace shark {

struct Config {
  Lang lang;
  bool strict;        // 警告をエラーとして扱う
  int stack_size;       // main の値スタックの大きさ（値の数）
  int task_stack_size;  // task で走らせるものの値スタックの大きさ
  int max_call_depth;   // 呼び出しの深さの上限
  size_t memory_limit;  // 実行中のプログラムが使ってよい量（バイト）。超えたら実行時エラー。0 は上限なし
  // 入れるモジュール（必須の time / math / task は常に入る）
  bool with_file, with_path, with_text, with_fmt, with_json, with_os, with_crypto, with_test,
      with_ui;
  Config()
      : lang(LANG_JA), strict(false), stack_size(65536), task_stack_size(4096),
        max_call_depth(10000), memory_limit(256u << 20), with_file(true), with_path(true),
        with_text(true), with_fmt(true), with_json(true), with_os(true), with_crypto(true),
        with_test(true), with_ui(true) {}
};

}  // namespace shark
#endif

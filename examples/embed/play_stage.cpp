// play_stage.cpp — 焼き込んだバイトコードを動かすゲーム（spec/runtime/bytecode.md）
//
//   1. 実行装置を作る            Runtime(config)
//   2. ホストの関数を登録する      register_host(...)   ← 作ったときと同じ順で
//   3. バイトコードを読む         load(bytecode)
//   4. 少しずつ動かす             step(budget)
//
// game.cpp との違いは3の1行だけ。ソースを渡す代わりにバイトコードを渡す。
// そのぶん、この実行ファイルには字句解析・構文解析・型検査・コード生成が入らない
// （Makefile の RT_SRC だけをリンクしている）。
//
// ファイルも開かない。バイトコードは stage_bytecode.h に配列として入っていて、
// ゲーム機に載るのはこの実行ファイルひとつで済む。
#include <stdio.h>

#include "../../core/platform/platform.h"
#include "../../core/runtime.h"
#include "game_hosts.h"
#include "stage_bytecode.h"

using namespace shark;

// print の出力は、ホストが受け取って好きな場所に出す
static void on_output(void* ud, const char* s, int n) {
  (void)ud;
  printf("      [吹き出し] %.*s", n, s);
}

int main() {
  platform_set(platform_desktop());

  Str code((const char*)kStageBytecode, (int)sizeof kStageBytecode);
  Config cfg = game::game_config();

  // 作ったときに決めた「使ってよいメモリ」と「panic の言い方」は、覚え書きに入っている
  // （build_stage.cpp）。読んで使わないと、ここの既定（64MB）のままになる
  Str err;
  BytecodeHeader h;
  if (!bytecode_read_header(code, &h, cfg.lang, &err)) {
    printf("バイトコードを読めない: %s\n", err.c_str());
    return 1;
  }
  cfg.memory_limit = (size_t)h.memory_mb << 20;
  cfg.lang = h.lang;

  Runtime rt(cfg);
  game::register_game_hosts(rt);   // 作ったときと同じ順（game_hosts.h）

  HostIO io;
  io.write_out = on_output;
  rt.set_io(io);

  if (!rt.load(code, &err)) {
    // 版や関数の表が食い違っていれば、ここで気づく
    printf("バイトコードを読めない: %s\n", err.c_str());
    return 1;
  }

  // ゲームの毎フレーム。1フレームに 60 命令だけ進める
  for (int frame = 1; frame <= 12; frame++) {
    printf("フレーム %2d: ", frame);
    RunStatus st = rt.step(60);
    if (st == SK_Running) {
      printf("まだ動いている（描画に戻れる）\n");
      continue;
    }
    if (st == SK_Finished) {
      printf("終わった（終了コード %d）\n", rt.exit_code());
      printf("\nこの実行ファイルに入っているのは実行装置だけで、\n"
             "ステージのソースも、型検査の仕組みも持っていない。\n");
      return 0;
    }
    printf("止まった: %s\n", rt.error_message().c_str());
    return 1;
  }
  printf("\n12 フレーム進めても終わらなかったので、こちらから止める\n");
  rt.abort_run();
  rt.step(60);
  printf("止めた: %s\n", rt.error_message().c_str());
  return 0;
}

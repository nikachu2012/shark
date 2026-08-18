// build_stage.cpp — ステージのコードをバイトコードにして、C の配列で書き出す
//
// これは**開発機で動かす道具**。ソースを読んで型検査まで済ませ、
// 結果を stage_bytecode.h に焼く（spec/runtime/bytecode.md）。
//
//   ./build_stage stage.shk stage_bytecode.h
//
// ゲーム本体（play_stage）はこの .h を取り込むだけなので、
// 字句解析も型検査も持たずに済む。プレイヤーの書いたコードをその場で動かす形
// （game.cpp）とは別に、「作者が用意したステージ」はこの形で配れる。
#include <stdio.h>

#include "../../core/bytecode.h"
#include "../../core/platform/platform.h"
#include "../../core/shark.h"
#include "game_hosts.h"

using namespace shark;

static bool read_file(const char* path, Str* out) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, (int)n);
  fclose(f);
  return true;
}

int main(int argc, char** argv) {
  platform_set(platform_desktop());
  if (argc < 3) {
    fprintf(stderr, "使い方: build_stage <stage.shk> <出力する .h>\n");
    return 2;
  }
  Str src;
  if (!read_file(argv[1], &src)) {
    fprintf(stderr, "ファイルを開けません: %s\n", argv[1]);
    return 2;
  }

  Config cfg = game::game_config();
  Engine engine(cfg);
  game::register_game_hosts(engine);   // 動かす側と同じ順で登録する

  const Vec<Diagnostic>& ds = engine.load(Str(argv[1]), src);
  for (int i = 0; i < ds.size(); i++) {
    Str text = format_diagnostic(ds[i], src, false);
    fputs(text.c_str(), stderr);
  }
  if (!engine.ok()) {
    fprintf(stderr, "型検査で止まったので、バイトコードは作らない\n");
    return 1;
  }

  BytecodeHeader h;
  h.main_file = Str(argv[1]);
  h.lang = cfg.lang;
  h.memory_mb = 8;                     // ゲーム機のつもりで小さめにしておく
  h.modules = modules_bits(cfg);
  Str code, err;
  if (!bytecode_write(*engine.program(), engine.registry(), h, &code, &err)) {
    fprintf(stderr, "バイトコードにできません: %s\n", err.c_str());
    return 1;
  }

  FILE* out = fopen(argv[2], "wb");
  if (!out) {
    fprintf(stderr, "書き出せません: %s\n", argv[2]);
    return 2;
  }
  fprintf(out,
          "// stage_bytecode.h — build_stage が作ったもの（直接さわらない）\n"
          "//   もと: %s\n"
          "static const unsigned char kStageBytecode[] = {",
          argv[1]);
  for (int i = 0; i < code.size(); i++) {
    if (i % 16 == 0) fprintf(out, "\n    ");
    fprintf(out, "0x%02x,", (unsigned char)code[i]);
  }
  fprintf(out, "\n};\n");
  fclose(out);
  printf("%s を作りました（バイトコード %d バイト）\n", argv[2], code.size());
  return 0;
}

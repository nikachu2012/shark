// vm_main.cpp — sharkvm（バイトコードだけを動かす実行装置）
//
// shark コマンドから、字句解析・構文解析・型検査・コード生成を外したもの。
// 中身は仮想マシンと標準ライブラリだけで、ソースは読めない（spec/runtime/bytecode.md）。
//
//   sharkvm hello.shkc          保存したバイトコードを動かす
//   ./hello                     自分のうしろに埋まっていれば、それを動かす（単一バイナリ）
//
// うしろに埋める形は shark build が作る。埋まっているときは、
// 引数をぜんぶプログラムに渡す（実行装置の選べるものは、作るときに決まっている）。
#include <stdio.h>

#include "../core/runtime.h"
#include "host.h"

namespace shark {

// --version で出す文字。shark build はこの中の "shark-runtime-stub-1" を探して、
// 渡されたファイルが実行装置かどうかを見分ける（--version で使うので消えない）
static const char kStubVersion[] = "shark 実行装置 0.1.0 [shark-runtime-stub-1]";

static void usage() {
  printf(
      "sharkvm — Shark🦈 のバイトコードを動かす（ソースは読めない）\n"
      "\n"
      "使い方:\n"
      "  sharkvm <file.shkc> [引数...]   保存したバイトコードを動かす\n"
      "\n"
      "選べるもの:\n"
      "  --memory <MB>  使ってよいメモリの量（省くと、作ったときに決めた量）\n"
      "  --lang ja|en   panic の言い方\n"
      "  --no-color     色を付けない（環境変数 NO_COLOR でも同じ）\n"
      "\n"
      "ソースから作るのは shark コマンドです（shark build <file.shk>）。\n");
}

static int run_bytecode(const Str& bytes, const Str& shown_name, int memory_mb, bool has_memory,
                        Lang lang, bool has_lang, bool color, const Vec<Str>& args) {
  Str err;
  BytecodeHeader h;
  if (!bytecode_read_header(bytes, &h, lang, &err)) {
    fprintf(stderr, "%s: %s\n", shown_name.c_str(), err.c_str());
    return 2;
  }
  // 作ったときの決めごとを引き継ぐ。命令で渡されたものが勝つ
  Config cfg;
  modules_to_config(h.modules, &cfg);
  cfg.lang = has_lang ? lang : h.lang;
  cfg.memory_limit = (size_t)(has_memory ? memory_mb : h.memory_mb) << 20;

  Runtime rt(cfg);
  rt.set_io(host_io());
  if (!rt.load(bytes, &err)) {
    fprintf(stderr, "%s: %s\n", shown_name.c_str(), err.c_str());
    return 2;
  }
  if (!rt.has_entry()) {
    fprintf(stderr, "実行するものがありません\n");
    return 1;
  }
  os_set_args(args);
  return run_loop(rt.vm(), color);
}

int main_impl(int argc, char** argv) {
  platform_set(platform_desktop());

  // 自分自身のうしろにバイトコードが埋まっていれば、それを動かす（単一バイナリ）
  Str self;
  Str payload;
  if (exe_path(&self) && pack_read_payload(self, &payload)) {
    Vec<Str> args;
    for (int i = 0; i < argc; i++) args.push(Str(argv[i]));
    return run_bytecode(payload, argc > 0 ? Str(argv[0]) : Str("shark"), 0, false, LANG_JA, false,
                        color_default(), args);
  }

  // ここから下は、ふつうの実行装置として呼ばれたとき
  Lang lang = LANG_JA;
  bool has_lang = false;
  int memory_mb = 0;
  bool has_memory = false;
  bool color = color_default();
  Vec<Str> rest;
  for (int i = 1; i < argc; i++) {
    Str a(argv[i]);
    if (rest.size() == 0) {   // 実行装置に向けた指定は、ファイル名より前だけ
      if (a == "--lang" && i + 1 < argc) {
        lang = Str(argv[++i]) == "en" ? LANG_EN : LANG_JA;
        has_lang = true;
        continue;
      }
      if (a == "--memory" && i + 1 < argc) {
        int64_t mb = 0;
        if (!str_to_int(Str(argv[++i]), &mb) || mb < 0) {
          fprintf(stderr, "--memory には MB の数を渡します（例: --memory 32）\n");
          return 2;
        }
        memory_mb = (int)mb;
        has_memory = true;
        continue;
      }
      if (a == "--no-color") { color = false; continue; }
      if (a == "-h" || a == "--help") { usage(); return 0; }
      if (a == "--version") {
        printf("%s\n", kStubVersion);
        return 0;
      }
    }
    rest.push(a);
  }
  if (rest.size() == 0) { usage(); return 2; }

  Str bytes;
  if (!read_file(rest[0], &bytes)) {
    fprintf(stderr, "ファイルを開けません: %s\n", rest[0].c_str());
    return 2;
  }
  return run_bytecode(bytes, rest[0], memory_mb, has_memory, lang, has_lang, color, rest);
}

}  // namespace shark

int main(int argc, char** argv) { return shark::main_impl(argc, argv); }

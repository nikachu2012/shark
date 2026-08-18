// bytecode.h — バイトコードの保存と読み戻し（spec/runtime/bytecode.md）
//
// ・書くのは型検査を通した Program。読むのは仮想マシンだけを持つ実行装置（runtime.h）
// ・ファイルは触らない。バイト列を作って返すだけで、書き出すのはフロントエンド
// ・命令の並びはそのまま入れる。仮想マシンが要らないもの（構文木・診断の位置）は入れない
#ifndef SHARK_BYTECODE_H
#define SHARK_BYTECODE_H

#include "config.h"
#include "program.h"
#include "registry.h"
#include "types.h"

namespace shark {

// ファイルの先頭 4 バイト。単一バイナリの中に埋めたときも同じ
extern const char kBytecodeMagic[4];   // "SHKC"
const int kBytecodeVersion = 1;

// 先頭に置く覚え書き。実行装置はこれを見てから中身を読む
struct BytecodeHeader {
  int version;
  Str main_file;       // 診断に出す名前（"examples/hello.shk"）
  Lang lang;           // panic の言い方に使う
  int memory_mb;       // 動かすときに使ってよい量。0 は上限なし
  uint32_t modules;    // 入れた標準ライブラリの組み合わせ
  uint64_t natives;    // 関数の表の指紋（registry_signature）
  BytecodeHeader() : version(kBytecodeVersion), lang(LANG_JA), memory_mb(64), modules(0), natives(0) {}
};

// 設定 ⇔ 覚え書きのビット
uint32_t modules_bits(const Config& cfg);
void modules_to_config(uint32_t bits, Config* cfg);

// 型検査を通した Program をバイト列にする
bool bytecode_write(Program& prog, const Registry& reg, const BytecodeHeader& h, Str* out,
                    Str* err);
// 覚え書きだけ読む（どのモジュールを入れて作られたかを、表を作る前に知るため）
bool bytecode_read_header(const Str& in, BytecodeHeader* h, Lang lang, Str* err);
// 中身を読む。prog は空のものを渡す。types と reg は覚え書きに合わせて作ったもの
bool bytecode_read(const Str& in, Program* prog, TypeTable& types, const Registry& reg, Lang lang,
                   Str* err);

}  // namespace shark
#endif

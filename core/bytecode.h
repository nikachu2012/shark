// bytecode.h — バイトコードのシリアライズおよびデシリアライズ（spec/runtime/bytecode.md 参照）
//
// ・シリアライズ対象は型検査済みの Program。デシリアライズ側は VM 実行エンジン（runtime.h 参照）
// ・ファイル I/O は行わずバイト列（Str）を入出力。ファイル書き出しはフロントエンド側が担当
// ・バイトコード命令列をそのまま保持。VM 実行に不要な構文木やソース位置情報は含めない
#ifndef SHARK_BYTECODE_H
#define SHARK_BYTECODE_H

#include "config.h"
#include "program.h"
#include "registry.h"
#include "types.h"

namespace shark {

// マジックナンバー（先頭 4 バイト）。スタンドアロンバイナリ埋め込み時も共通
extern const char kBytecodeMagic[4];   // "SHKC"
const int kBytecodeVersion = 4;   // バージョン 4: print/write の sep/end 引数対応
// チェックサム（CRC32）のオフセット（マジック 4B + バージョン 4B の直後）
const int kChecksumAt = 8;

// バイトコードヘッダ。ランタイムはヘッダ情報を検証した上でペイロードをロードする
struct BytecodeHeader {
  int version;
  Str main_file;       // 診断出力用ソースファイル名（例: "examples/hello.shk"）
  Lang lang;           // panic 出力言語（LANG_JA / LANG_EN）
  int memory_mb;       // メモリ使用量上限（MB）。0 は無制限
  uint32_t modules;    // リンクされた標準モジュールのビットマスク
  uint64_t natives;    // ネイティブ関数レジストリのシグネチャ（registry_signature）
  BytecodeHeader() : version(kBytecodeVersion), lang(LANG_JA), memory_mb(256), modules(0), natives(0) {}
};

// 設定 ⇔ モジュールビットマスク変換
uint32_t modules_bits(const Config& cfg);
void modules_to_config(uint32_t bits, Config* cfg);

// 型検査済み Program をバイトコード列にシリアライズ
bool bytecode_write(Program& prog, const Registry& reg, const BytecodeHeader& h, Str* out,
                    Str* err);
// ヘッダのみ読み込み（必要なモジュールを事前に判別するため）
bool bytecode_read_header(const Str& in, BytecodeHeader* h, Lang lang, Str* err);
// バイトコードペイロードをデシリアライズ。空の prog を渡し、ヘッダ情報に基づいて初期化した types と reg を渡す
bool bytecode_read(const Str& in, Program* prog, TypeTable& types, const Registry& reg, Lang lang,
                   Str* err);

}  // namespace shark
#endif

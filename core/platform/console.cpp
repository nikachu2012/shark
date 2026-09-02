// console.cpp — ファイルも OS も持たない機種向けの移植層（雛形）
//
// ゲーム機に載せるときは、これを複製して埋める。必須の4つだけを用意すればよい。
// 任意機能（file / os）は 0 のままでよく、その場合そのモジュールは import できない。
#include "platform.h"

#include <stdlib.h>

namespace shark {
namespace {

void* c_alloc(size_t n) { return malloc(n ? n : 1); }          // ← 機種の確保に差し替える
void* c_realloc(void* p, size_t n) { return realloc(p, n ? n : 1); }
void  c_free(void* p) { free(p); }
void  c_fatal(const char*) { abort(); }                        // ← 機種の停止に差し替える

int64_t c_now() { return 0; }                                   // ← 実時刻が取れるなら返す
int64_t c_mono_counter = 0;
int64_t c_mono() { return c_mono_counter += 1000000; }          // ← 単調増加する時計に差し替える
void    c_sleep(int64_t) {}                                     // 待たない（step で刻む）
int     c_local_offset(int64_t) { return 0; }

void c_write_out(const char*, int) {}                           // ← 画面に出す
void c_write_err(const char*, int) {}
bool c_read_line(Str*) { return false; }
void c_exit(int) {}

const Platform kConsole = {
    c_alloc, c_realloc, c_free, c_fatal,
    c_now, c_mono, c_sleep, c_local_offset,
    c_write_out, c_write_err, c_read_line, c_exit,
    0,  // file なし
    0,  // os なし
    0,  // 乱数のもとなし（機種の乱数器を持っているなら PlatformRandom を埋める。
        //   埋めるまで std.crypto の乱数は実行時エラーになる。ハッシュは使える）
    0,  // 画面なし（機種の描画先を持っているなら PlatformScreen を埋める。
        //   埋めるまで std.ui は見えない面に描くだけになる → spec/library/ui.md）
    0,  // 字なし（機種が字を描いてくれるなら PlatformFont を埋める。
        //   埋めるまで std.ui の字は内蔵の 5×7（ASCII）だけになる）
    0   // 動的コードなし（実行できるメモリを持っているなら PlatformExec を埋める。
        //   埋めるまで実行時コンパイルはせず、常に仮想マシンで動く → spec/runtime/execution.md）
};

}  // namespace

const Platform* platform_console() { return &kConsole; }

}  // namespace shark

// web.h — ブラウザ（WebAssembly）向けの移植層の出入口
//
// 移植層そのものは web.cpp。ここには、ホスト（web/shark_web.cpp）が
// 差し込むものだけを置く。コアはこのファイルを見ない。
#ifndef SHARK_PLATFORM_WEB_H
#define SHARK_PLATFORM_WEB_H

#include "platform.h"

namespace shark {

// 移植層が書いた文字の受け取り先。ホストが決める（0 に戻すと捨てる）
typedef void (*WebSink)(void* ud, const char* s, int n, bool is_err);
void web_set_sink(WebSink fn, void* ud);

}  // namespace shark
#endif

// fmt_src.h — ソースの見た目を整える（shark fmt）
//
// 入るのも出るのもソースの文字列だけ。ファイルは読まないし、書かない
// （コアの決めごと。spec/README.md）。
#ifndef SHARK_FMT_SRC_H
#define SHARK_FMT_SRC_H

#include "support.h"

namespace shark {

// 整えたソースを返す。**読めないソース**（字句の誤り）と、整えたものが
// もとと違う意味になってしまうときは、もとのまま返して ok に false を入れる。
// ok は 0 でもよい
Str format_source(const Str& src, bool* ok = 0);

}  // namespace shark
#endif

// prelude.h — Shark 自身で書いた部分
//
// 並べ替えは、基本型でもクラスでも同じコードで動かしたいので、
// ここに Shark で書いてある（compare の呼び分けは仮想マシンが行う）。
#ifndef SHARK_PRELUDE_H
#define SHARK_PRELUDE_H

namespace shark {

static const char* kPreludeSource =
    "func __merge<T: Comparable>(ref xs: list<T>, lo: int, mid: int, hi: int) -> void {\n"
    "  var tmp: list<T> = [];\n"
    "  var a = lo;\n"
    "  var b = mid;\n"
    "  while a < mid && b < hi {\n"
    "    if xs[b].compare(xs[a]) < 0 {\n"
    "      tmp.push(xs[b]);\n"
    "      b = b + 1;\n"
    "    } else {\n"
    "      tmp.push(xs[a]);\n"
    "      a = a + 1;\n"
    "    }\n"
    "  }\n"
    "  while a < mid { tmp.push(xs[a]); a = a + 1; }\n"
    "  while b < hi { tmp.push(xs[b]); b = b + 1; }\n"
    "  var k = 0;\n"
    "  while k < tmp.len() {\n"
    "    xs[lo + k] = tmp[k];\n"
    "    k = k + 1;\n"
    "  }\n"
    "}\n"
    "\n"
    "func __sort<T: Comparable>(ref xs: list<T>) -> void {\n"
    "  var n = xs.len();\n"
    "  if n < 2 { return; }\n"
    "  var width = 1;\n"
    "  while width < n {\n"
    "    var i = 0;\n"
    "    while i < n {\n"
    "      var mid = i + width;\n"
    "      var hi = i + width + width;\n"
    "      if mid > n { mid = n; }\n"
    "      if hi > n { hi = n; }\n"
    "      __merge(ref xs, i, mid, hi);\n"
    "      i = i + width + width;\n"
    "    }\n"
    "    width = width + width;\n"
    "  }\n"
    "}\n";

}  // namespace shark
#endif

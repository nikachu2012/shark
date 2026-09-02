#!/bin/sh
# tests/run.sh — .shk を動かし、.expected と見比べる
#
#   make test        で呼ばれる
#   sh tests/run.sh  でも動く
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
# std.ui は画面を開かない。開くと機種によって結果が変わり、窓も出てしまう
# （spec/library/ui.md）。見えない面に描くだけになる
SHARK_UI=off
export SHARK_UI
shark="$root/shark"
pass=0
fail=0

# 見比べる前に行末の CR を落とす。Windows で書いたソースは行が CR LF で終わり、
# 端末に出すときも LF が CR LF になるので、そのままだと中身が同じでも食い違う
norm() { tr -d '\r'; }
expnorm="${TMPDIR:-/tmp}/shark_expected.$$"   # 食い違いを見せるときの置き場

run_dir() {
  dir="$1"
  cd "$root/$dir" || exit 1
  for f in *.shk; do
    exp="${f%.shk}.expected"
    [ -f "$exp" ] || continue
    got=$("$shark" run --no-color "$f" 2>&1 | norm)
    if [ "$got" = "$(norm < "$exp")" ]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      echo "fail  $dir/$f"
      norm < "$exp" > "$expnorm"
      printf '%s\n' "$got" | diff -u "$expnorm" - | sed -n '3,12p'
    fi
  done
  cd "$root" || exit 1
}

run_dir tests/cases
run_dir tests/errors

cd "$root/tests" || exit 1

# メモリの上限を超えたら、実行時エラーで止まること
out=$("$shark" run --no-color --memory 8 memory_limit.shk 2>&1 | norm | head -2)
if [ "$out" = "$(norm < memory_limit.expected)" ]; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "fail  tests/memory_limit.shk"
  norm < memory_limit.expected > "$expnorm"
  printf '%s\n' "$out" | diff -u "$expnorm" -
fi

# 後始末の取りこぼしと、上限の見張り（C++ 側）
if [ -x "$root/tests/memcheck" ] || [ -x "$root/tests/memcheck.exe" ]; then
  if "$root/tests/memcheck" > /tmp/shark_memcheck.txt 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    cat /tmp/shark_memcheck.txt
  fi
fi

# 壊れたバイトコードを断るか（C++ 側）。落ちれば、この実行ファイルごと死ぬので失敗になる
if [ -x "$root/tests/bytecheck" ] || [ -x "$root/tests/bytecheck.exe" ]; then
  if "$root/tests/bytecheck" > /tmp/shark_bytecheck.txt 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    cat /tmp/shark_bytecheck.txt
  fi
fi

# 単一バイナリと、保存したバイトコード（spec/runtime/bytecode.md）。
# tests/cases を build して、ソースから動かしたときと同じ出力になることを見る
sharkvm="$root/sharkvm"
tmp="${TMPDIR:-/tmp}/shark_build_test.$$"
if [ -x "$sharkvm" ] || [ -x "$sharkvm.exe" ]; then
  mkdir -p "$tmp"
  cd "$root/tests/cases" || exit 1
  for f in *.shk; do
    exp="${f%.shk}.expected"
    [ -f "$exp" ] || continue
    if ! "$shark" build --no-color -o "$tmp/app" "$f" > "$tmp/build.log" 2>&1; then
      fail=$((fail + 1))
      echo "fail  build tests/cases/$f"
      sed -n '1,6p' "$tmp/build.log"
      continue
    fi
    got=$(NO_COLOR=1 "$tmp/app" 2>&1 | norm)
    if [ "$got" = "$(norm < "$exp")" ]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      echo "fail  単一バイナリ tests/cases/$f"
      norm < "$exp" > "$expnorm"
      printf '%s\n' "$got" | diff -u "$expnorm" - | sed -n '3,12p'
    fi
  done
  # バイトコードだけ保存したものを、実行装置と shark run の両方で動かす
  f=01_basics.shk
  exp="${f%.shk}.expected"
  if ! "$shark" build --no-color --bytecode -o "$tmp/one.shkc" "$f" > "$tmp/build.log" 2>&1; then
    fail=$((fail + 1))
    echo "fail  build --bytecode tests/cases/$f"
    sed -n '1,6p' "$tmp/build.log"
  else
    for how in "$sharkvm --no-color" "$shark run --no-color"; do
      got=$($how "$tmp/one.shkc" 2>&1 | norm)
      if [ "$got" = "$(norm < "$exp")" ]; then
        pass=$((pass + 1))
      else
        fail=$((fail + 1))
        echo "fail  $how $tmp/one.shkc"
        norm < "$exp" > "$expnorm"
        printf '%s\n' "$got" | diff -u "$expnorm" - | sed -n '3,12p'
      fi
    done
  fi
  cd "$root/tests" || exit 1
  rm -rf "$tmp"
else
  echo "skip  単一バイナリ（sharkvm がありません。make sharkvm）"
fi

# std.test の走らせ方も見る
out=$("$shark" test unit_test.shk 2>&1 | norm)
if [ "$out" = "$(norm < unit_test.expected)" ]; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "fail  tests/unit_test.shk"
  norm < unit_test.expected > "$expnorm"
  printf '%s\n' "$out" | diff -u "$expnorm" -
fi

rm -f "$expnorm"
echo ""
echo "$((pass + fail)) 件中 $pass 件成功"
[ "$fail" -eq 0 ]

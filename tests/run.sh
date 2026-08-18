#!/bin/sh
# tests/run.sh — .shk を動かし、.expected と見比べる
#
#   make test        で呼ばれる
#   sh tests/run.sh  でも動く
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
shark="$root/shark"
pass=0
fail=0

run_dir() {
  dir="$1"
  cd "$root/$dir" || exit 1
  for f in *.shk; do
    exp="${f%.shk}.expected"
    [ -f "$exp" ] || continue
    got=$("$shark" run --no-color "$f" 2>&1)
    if [ "$got" = "$(cat "$exp")" ]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      echo "fail  $dir/$f"
      printf '%s\n' "$got" | diff -u "$exp" - | sed -n '3,12p'
    fi
  done
  cd "$root" || exit 1
}

run_dir tests/cases
run_dir tests/errors

cd "$root/tests" || exit 1

# メモリの上限を超えたら、実行時エラーで止まること
out=$("$shark" run --no-color --memory 8 memory_limit.shk 2>&1 | head -2)
if [ "$out" = "$(cat memory_limit.expected)" ]; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "fail  tests/memory_limit.shk"
  printf '%s\n' "$out" | diff -u memory_limit.expected -
fi

# 後始末の取りこぼしと、上限の見張り（C++ 側）
if [ -x "$root/tests/memcheck" ]; then
  if "$root/tests/memcheck" > /tmp/shark_memcheck.txt 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    cat /tmp/shark_memcheck.txt
  fi
fi

# 単一バイナリと、保存したバイトコード（spec/runtime/bytecode.md）。
# tests/cases を build して、ソースから動かしたときと同じ出力になることを見る
sharkvm="$root/sharkvm"
tmp="${TMPDIR:-/tmp}/shark_build_test.$$"
if [ -x "$sharkvm" ]; then
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
    got=$(NO_COLOR=1 "$tmp/app" 2>&1)
    if [ "$got" = "$(cat "$exp")" ]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      echo "fail  単一バイナリ tests/cases/$f"
      printf '%s\n' "$got" | diff -u "$exp" - | sed -n '3,12p'
    fi
  done
  # バイトコードだけ保存したものを、実行装置と shark run の両方で動かす
  f=01_basics.shk
  exp="${f%.shk}.expected"
  "$shark" build --no-color --bytecode -o "$tmp/one.shkc" "$f" > "$tmp/build.log" 2>&1
  for how in "$sharkvm --no-color" "$shark run --no-color"; do
    got=$($how "$tmp/one.shkc" 2>&1)
    if [ "$got" = "$(cat "$exp")" ]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      echo "fail  $how $tmp/one.shkc"
      printf '%s\n' "$got" | diff -u "$exp" - | sed -n '3,12p'
    fi
  done
  cd "$root/tests" || exit 1
  rm -rf "$tmp"
else
  echo "skip  単一バイナリ（sharkvm がありません。make sharkvm）"
fi

# std.test の走らせ方も見る
out=$("$shark" test unit_test.shk 2>&1)
if [ "$out" = "$(cat unit_test.expected)" ]; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "fail  tests/unit_test.shk"
  printf '%s\n' "$out" | diff -u unit_test.expected -
fi

echo ""
echo "$((pass + fail)) 件中 $pass 件成功"
[ "$fail" -eq 0 ]

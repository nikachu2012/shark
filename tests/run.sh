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

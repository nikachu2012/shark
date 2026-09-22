#!/bin/sh
# tests/run.sh — .shk テストを実行し、.expected と出力を比較検証する
#
#   make test        で呼ばれる
#   sh tests/run.sh  でも動く
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
# std.ui はウィンドウを開かない。環境による差異やウィンドウ表示を防ぐため
# （spec/library/ui.md）、オフスクリーンバッファ描画モードとする
SHARK_UI=off
export SHARK_UI
shark="$root/shark"
pass=0
fail=0

# 比較前に行末の CR を除去（Windows 改行コード CRLF による差分を正規化）
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

# コードフォーマッタ（shark fmt）の検証。フォーマット後の出力一致および
# 冪等性（再フォーマットで結果が変化しないこと）を確認
if [ -f "$root/tests/fmt/messy.shk" ]; then
  got=$("$shark" fmt "$root/tests/fmt/messy.shk" 2>&1 | norm)
  if [ "$got" = "$(norm < "$root/tests/fmt/tidy.expected")" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "fail  tests/fmt/messy.shk"
    norm < "$root/tests/fmt/tidy.expected" > "$expnorm"
    printf '%s\n' "$got" | diff -u "$expnorm" - | sed -n '3,12p'
  fi
  again=$("$shark" fmt "$root/tests/fmt/tidy.expected" 2>&1 | norm)
  if [ "$again" = "$(norm < "$root/tests/fmt/tidy.expected")" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "fail  整えたものを、もう一度整えると変わってしまう"
  fi
fi

# 対話（shark repl）の検証。入力を流し込み、値の表示・定義の引き継ぎ・
# 誤りのあとの続行・:reset・os.exit での終了をまとめて確かめる
if [ -f "$root/tests/repl/session.txt" ]; then
  got=$(cd "$root/tests/repl" && "$shark" repl --no-color < session.txt 2>&1 | norm)
  if [ "$got" = "$(norm < "$root/tests/repl/session.expected")" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "fail  tests/repl/session.txt"
    norm < "$root/tests/repl/session.expected" > "$expnorm"
    printf '%s\n' "$got" | diff -u "$expnorm" - | sed -n '3,12p'
  fi
fi

# メモリリークおよびメモリ上限の検証（C++ 側）
if [ -x "$root/tests/memcheck" ] || [ -x "$root/tests/memcheck.exe" ]; then
  if "$root/tests/memcheck" > /tmp/shark_memcheck.txt 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    cat /tmp/shark_memcheck.txt
  fi
fi

# 不正なバイトコードを安全に拒絶するかの検証（C++ 側）
if [ -x "$root/tests/bytecheck" ] || [ -x "$root/tests/bytecheck.exe" ]; then
  if "$root/tests/bytecheck" > /tmp/shark_bytecheck.txt 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    cat /tmp/shark_bytecheck.txt
  fi
fi

# IME（文字入力）処理のモック環境テスト（C++ 側）
if [ -x "$root/tests/imecheck" ] || [ -x "$root/tests/imecheck.exe" ]; then
  if "$root/tests/imecheck" > /tmp/shark_imecheck.txt 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    cat /tmp/shark_imecheck.txt
  fi
fi

# UI ウィジェットのクリック・ホバー動作のモック環境テスト（C++ 側）
if [ -x "$root/tests/uicheck" ] || [ -x "$root/tests/uicheck.exe" ]; then
  if "$root/tests/uicheck" > /tmp/shark_uicheck.txt 2>&1; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    cat /tmp/shark_uicheck.txt
  fi
fi

# 単一バイナリおよびバイトコード実行の検証（spec/runtime/bytecode.md）。
# tests/cases を build し、インタープリタ実行と同一出力になることを確認
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
  # 保存したバイトコードを sharkvm と shark run の両方で実行検証
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

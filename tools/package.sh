#!/bin/sh
# package.sh — 作ったものを、配れる形に包む（dist/ に出る）
#
#   sh tools/package.sh            # 機種の名前は自分で決める（uname から）
#   sh tools/package.sh macos-arm64
#   make dist                      # 作ってから包む（Makefile から）
#
# 中身は、それだけで動く1つの実行ファイル（shark と sharkvm）と、
# 同梱のフォント・見本・README。**入れてもらうものは何も無い。**
# 押すたびの検査（.github/workflows）も、配るときも、ここを呼ぶ
# （包み方を1か所にしておく。README の「押すたびの検査と、配りかた」）。
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# 機種の名前。渡されなければ uname から作る
name=${1:-}
if [ -z "$name" ]; then
  os=$(uname -s 2>/dev/null || echo unknown)
  arch=$(uname -m 2>/dev/null || echo unknown)
  case "$os" in
    Darwin) os=macos ;;
    Linux) os=linux ;;
    MINGW*|MSYS*|CYGWIN*) os=windows ;;
    *) os=$(echo "$os" | tr 'A-Z' 'a-z') ;;
  esac
  case "$arch" in
    arm64|aarch64) arch=arm64 ;;
    x86_64|amd64) arch=x86_64 ;;
  esac
  name="$os-$arch"
fi

exe=""
[ -f "$root/shark.exe" ] && exe=".exe"
if [ ! -f "$root/shark$exe" ]; then
  echo "shark$exe がありません。先に作ります（make、または tools\\build_win.bat）" >&2
  exit 1
fi

dir="$root/dist/shark-$name"
rm -rf "$dir"
mkdir -p "$dir"

cp "$root/shark$exe" "$dir/"
[ -f "$root/sharkvm$exe" ] && cp "$root/sharkvm$exe" "$dir/"
cp "$root/README.md" "$dir/"

# 日本語の字形。フロントエンドは実行ファイルの隣（か assets/fonts）を探す
mkdir -p "$dir/assets/fonts"
cp "$root/assets/fonts/NotoSansJP-Regular.otf" "$dir/assets/fonts/"
cp "$root/assets/fonts/LICENSE-NotoSansJP.txt" "$dir/assets/fonts/"

# 見本。広げてすぐ動かせるように、まるごと入れる
cp -r "$root/examples" "$dir/examples"
rm -rf "$dir/examples/embed"

cat > "$dir/はじめに.txt" <<'TXT'
Shark🦈 — ゲーム機で動く学習用プログラミング言語

  ./shark run examples/hello.shk      動かす
  ./shark run examples/widgets.shk    画面の部品をぜんぶ出す
  ./shark fmt examples/hello.shk      見た目を整える

入れてもらうものはありません。日本語の字形も中に入っています。
くわしくは README.md を読んでください。

macOS で「開発元を確認できないため開けません」と言われたら、
一度だけ次を打てば開けます。

  xattr -d com.apple.quarantine ./shark ./sharkvm
TXT

# 包む。Windows は zip、それ以外は tar.gz（相手が広げやすい形）
cd "$root/dist"
if [ -n "$exe" ]; then
  out="shark-$name.zip"
  rm -f "$out"
  if command -v 7z >/dev/null 2>&1; then
    7z a "$out" "shark-$name" > /dev/null
  elif command -v powershell >/dev/null 2>&1; then
    powershell -NoProfile -Command "Compress-Archive -Path 'shark-$name' -DestinationPath '$out' -Force"
  else
    out="shark-$name.tar.gz"
    tar -czf "$out" "shark-$name"
  fi
else
  out="shark-$name.tar.gz"
  rm -f "$out"
  tar -czf "$out" "shark-$name"
fi

echo "できました: dist/$out"

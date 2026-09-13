#!/bin/sh
# package.sh — ビルド成果物を配布用パッケージとしてアーカイブする（dist/ に出力）
#
#   sh tools/package.sh            # プラットフォーム名は自動判定（uname から）
#   sh tools/package.sh macos-arm64
#   make dist                      # ビルド後にパッケージ化（Makefile から実行）
#
# 中身は、スタンドアロンで動作する実行ファイル（shark と sharkvm）、
# 同梱フォント、サンプルコード、ドキュメント（docs/）、README。（追加の外部依存は不要）
# push時のCI（.github/workflows）およびリリース配布時に共通で実行される
# （パッケージ化の処理を一箇所に集約。README の「プッシュ時の自動テストと配布」を参照）。
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# プラットフォーム名。未指定時は uname から自動判定
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
  echo "shark$exe が見つかりません。先にビルドしてください（make、または tools\\build_win.bat）" >&2
  exit 1
fi

dir="$root/dist/shark-$name"
rm -rf "$dir"
mkdir -p "$dir"

cp "$root/shark$exe" "$dir/"
[ -f "$root/sharkvm$exe" ] && cp "$root/sharkvm$exe" "$dir/"
cp "$root/README.md" "$dir/"

# 日本語フォント。フロントエンドは実行ファイルと同一ディレクトリ（または assets/fonts）を探索
mkdir -p "$dir/assets/fonts"
cp "$root/assets/fonts/NotoSansJP-Regular.otf" "$dir/assets/fonts/"
cp "$root/assets/fonts/LICENSE-NotoSansJP.txt" "$dir/assets/fonts/"

# サンプルコード。すぐに実行確認できるよう全体を同梱
cp -r "$root/examples" "$dir/examples"
rm -rf "$dir/examples/embed"

# ドキュメント（APIリファレンスと言語ガイド）。ブラウザ版に同梱するものと同一。
# 未生成の場合はここで生成する（生成できない場合はドキュメントなしでアーカイブ）
if [ ! -f "$root/docs/reference/index.html" ]; then
  python3 "$root/docs/gen.py" > /dev/null 2>&1 || true
fi
if [ -f "$root/docs/reference/index.html" ]; then
  cp -R "$root/docs/reference" "$dir/docs"
fi

cat > "$dir/はじめに.txt" <<'TXT'
Shark🦈 — ゲーム機で動く学習用プログラミング言語

  ./shark run examples/hello.shk      実行する
  ./shark run examples/widgets.shk    GUIウィジェットのデモを実行
  ./shark fmt examples/hello.shk      コードを自動整形

外部依存はなく、単体で動作します。日本語フォントも同梱されています。

  docs/index.html                     APIリファレンス（ブラウザで閲覧）
  docs/guide.html                     言語ガイド
  README.md                           ビルド・組み込み手順

macOS で「開発元を確認できないため開けません」と警告された場合は、
ターミナルで以下のコマンドを実行してください。

  xattr -d com.apple.quarantine ./shark ./sharkvm
TXT

# アーカイブ作成。Windows は zip、その他は tar.gz
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

echo "生成完了: dist/$out"

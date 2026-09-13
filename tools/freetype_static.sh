#!/bin/sh
# freetype_static.sh — FreeType のソースを取得し、静的ライブラリとしてビルドする（build/freetype/libfreetype.a）
#
#   sh tools/freetype_static.sh          # ビルド（既に存在する場合はスキップ）
#   sh tools/freetype_static.sh --flags  # make に渡すビルドフラグのみを出力
#
# 目的:
#   FreeType は日本語等のフォント描画に使用する唯一の外部ライブラリ。
#   OS 提供の動的ライブラリにリンクすると、配布先の環境にも同一ライブラリが必要となる。
#   ここで静的ライブラリとしてあらかじめビルドしておくことで、単一バイナリで配布可能にする。
#   Windows での同等の処理は tools\build_win.bat freetype（build_win.ps1 側）が担う。
#
# ソースの取得元およびビルド対象ファイル一覧は build_win.ps1 と統一してある。
# 圧縮や画像展開ライブラリ（zlib・libpng・brotli）にはリンクしない。
# フォントのアウトライン描画には不要であり、依存関係を増やさないため。
set -e

root=$(cd "$(dirname "$0")/.." && pwd)
ver=VER-2-13-3
src="$root/build/freetype-src"
out="$root/build/freetype"
lib="$out/libfreetype.a"

if [ "${1:-}" = "--flags" ]; then
  printf 'FREETYPE=1 FT_CFLAGS=-I%s/include FT_LIBS=%s\n' "$src" "$lib"
  exit 0
fi

if [ -f "$lib" ]; then
  echo "もうあります: $lib"
  exit 0
fi

mkdir -p "$root/build"
if [ ! -f "$src/include/ft2build.h" ]; then
  echo "FreeType $ver を取ってきています..."
  url="https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/$ver"
  tgz="$root/build/freetype.tar.gz"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$tgz"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$tgz" "$url"
  else
    echo "curl も wget も見つかりません。$src に FreeType のソースコードを配置してから再実行してください" >&2
    exit 1
  fi
  (cd "$root/build" && tar -xzf freetype.tar.gz)
  rm -rf "$src"
  mv "$root/build/freetype-$ver" "$src"
  rm -f "$tgz"
fi

# docs/INSTALL.ANY が挙げているもの。ftmodule.h が名指しする組み立て部品も揃える
files="base/ftsystem.c base/ftinit.c base/ftdebug.c base/ftbase.c \
base/ftbbox.c base/ftglyph.c base/ftbdf.c base/ftbitmap.c \
base/ftcid.c base/ftfstype.c base/ftgasp.c base/ftgxval.c \
base/ftmm.c base/ftotval.c base/ftpatent.c base/ftpfr.c \
base/ftstroke.c base/ftsynth.c base/fttype1.c base/ftwinfnt.c \
bdf/bdf.c cff/cff.c cid/type1cid.c pcf/pcf.c pfr/pfr.c \
sfnt/sfnt.c truetype/truetype.c type1/type1.c type42/type42.c \
winfonts/winfnt.c \
smooth/smooth.c raster/raster.c sdf/sdf.c svg/svg.c \
autofit/autofit.c cache/ftcache.c gzip/ftgzip.c lzw/ftlzw.c \
psaux/psaux.c pshinter/pshinter.c psnames/psnames.c"

obj="$out/obj"
mkdir -p "$obj"
CC=${CC:-cc}
echo "FreeType を作っています..."
for f in $files; do
  name=$(basename "$f" .c)
  [ -f "$obj/$name.o" ] && continue
  # -DFT2_BUILD_LIBRARY は「ライブラリ本体を作っている側」の目印
  $CC -O2 -w -DFT2_BUILD_LIBRARY -I"$src/include" -c "$src/src/$f" -o "$obj/$name.o"
done
rm -f "$lib"
ar rcs "$lib" "$obj"/*.o
echo "できました: $lib"
echo
echo "使い方:"
echo "  make \$(sh tools/freetype_static.sh --flags)"

#!/bin/sh
# build.sh — ブラウザで動く Shark を作る（web/dist/ に出る）
#
#   sh web/build.sh          # 作る（make web も同じ）
#
# 配るのは web/serve.sh の役目。ここは作るところまでしかしない。
# 要るもの: Emscripten（emcc）。入っていなければ入れ方を出して止まる。
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
out="$here/dist"

# 書くところに使う Monaco Editor。npm から取り寄せて web/vendor/ にためる
# （初回だけ。git には入れない）
MONACO=0.56.0
monaco_dir="$here/vendor/monaco-$MONACO"

if ! command -v emcc >/dev/null 2>&1; then
  if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    . "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
  fi
fi
if ! command -v emcc >/dev/null 2>&1; then
  cat >&2 <<'MSG'
emcc が見つかりません。Emscripten を入れてから、もう一度実行します。

  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
  ~/emsdk/emsdk install latest
  ~/emsdk/emsdk activate latest
  . ~/emsdk/emsdk_env.sh
MSG
  exit 1
fi

# コア（移植層は platform/web.cpp を選ぶ）とブラウザ側のホスト
CORE="\
$root/core/support.cpp $root/core/value.cpp $root/core/program.cpp $root/core/types.cpp \
$root/core/diag.cpp $root/core/lexer.cpp $root/core/parser.cpp $root/core/check.cpp \
$root/core/codegen.cpp $root/core/vm.cpp $root/core/registry.cpp $root/core/shark.cpp \
$root/core/platform/web.cpp \
$root/core/lib/format.cpp $root/core/lib/builtin.cpp $root/core/lib/math.cpp \
$root/core/lib/time.cpp $root/core/lib/task.cpp $root/core/lib/fmt.cpp \
$root/core/lib/path.cpp $root/core/lib/file.cpp $root/core/lib/os.cpp \
$root/core/lib/text.cpp $root/core/lib/json.cpp $root/core/lib/test.cpp \
$root/core/lib/crypto.cpp \
$here/shark_web.cpp"

# ---------------------------------------------------------------- Monaco Editor
if [ ! -f "$monaco_dir/vs/editor/editor.main.js" ]; then   # 取り寄せ済みかの印
  echo "Monaco Editor $MONACO を取り寄せています..."
  tmp=$(mktemp -d)
  url="https://registry.npmjs.org/monaco-editor/-/monaco-editor-$MONACO.tgz"
  if ! curl -fsSL "$url" -o "$tmp/monaco.tgz"; then
    rm -rf "$tmp"
    echo "取り寄せられません: $url" >&2
    echo "つながる場所で一度 make web を通すと、web/vendor/ にたまって次からは要りません。" >&2
    exit 1
  fi
  rm -rf "$monaco_dir"
  mkdir -p "$monaco_dir"
  tar -xzf "$tmp/monaco.tgz" -C "$monaco_dir" --strip-components=2 package/min/vs
  rm -rf "$tmp"
  # 使うものだけ残す。書くところ本体と、その付属（入力候補・説明・引数の案内）だけで、
  # 他の言語の色分けや TypeScript などの言語サービスは要らない。24 MB → 4 MB ほどになる
  find "$monaco_dir/vs" -mindepth 1 -maxdepth 1 -name '*.js' \
    ! -name 'loader.js' ! -name 'nls.messages-loader.js' \
    ! -name 'editor-*.js' ! -name 'editorWorkerHost-*.js' ! -name 'index-*.js' \
    ! -name 'toggleHighContrast-*.js' ! -name 'monaco.contribution-*.js' \
    ! -name '*.worker-*.js' \
    -exec rm -f {} +
  rm -rf "$monaco_dir/vs/language"
  find "$monaco_dir/vs/editor" -mindepth 1 \
    ! -name 'editor.main.js' ! -name 'editor.main.css' -exec rm -rf {} +
  find "$monaco_dir/vs/basic-languages" -mindepth 1 \
    ! -name 'monaco.contribution.js' -exec rm -rf {} +
  find "$monaco_dir/vs/assets" -mindepth 1 ! -name 'editor.worker-*.js' -exec rm -rf {} +
  # 画面の文字は英語のまま使う（言い換えの表は要らない）
  rm -rf "$monaco_dir/vs/nls"
fi

mkdir -p "$out"

echo "コンパイル中（初回は数分かかります）..."
emcc $CORE \
  -std=c++17 -O2 -fno-exceptions -fno-rtti \
  -Wall -Wextra -Wno-unused-parameter \
  -sMODULARIZE=1 -sEXPORT_NAME=createShark \
  -sENVIRONMENT=web,node \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=48MB -sSTACK_SIZE=8MB \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8 \
  -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
  --closure 0 \
  -o "$out/shark.js"

cp "$here/index.html" "$here/app.js" "$here/lang.js" "$here/style.css" "$out/"
python3 "$here/examples.py" "$root" "$here" "$out/examples.js"
python3 "$here/api.py" "$root" "$out/api.js"

# Monaco 本体と、そこへの道しるべ（ファイル名に版ごとの印が付くので、作るときに書き出す）
rm -rf "$out/vendor"
mkdir -p "$out/vendor"
cp -R "$monaco_dir/vs" "$out/vendor/vs"
cat > "$out/vendor/monaco-paths.js" <<EOF
// monaco-paths.js — web/build.sh が作る
window.MONACO = { vs: 'vendor/vs', version: '$MONACO' };
EOF

echo "できました: web/dist/"
ls -lh "$out" | sed 's/^/  /'

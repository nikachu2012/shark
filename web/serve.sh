#!/bin/sh
# serve.sh — 作ってから、その場で配る
#
#   sh web/serve.sh          # 作って http://localhost:8000/ に配る（make web-serve も同じ）
#   sh web/serve.sh 8080     # 港（ポート）を変える
#
# 作るのは web/build.sh。ここから毎回呼ぶので、直した内容がそのまま出る。
# ブラウザは file:// から .wasm を読めないので、見るときはこれを通す。
set -e

here=$(cd "$(dirname "$0")" && pwd)
port=${1:-8000}

case "$port" in
  ''|*[!0-9]*)
    echo "港（ポート）は数で渡します（例: sh web/serve.sh 8080）" >&2
    exit 2
    ;;
esac

sh "$here/build.sh"

echo
echo "http://localhost:$port/ を開きます（止めるときは Ctrl-C）"
cd "$here/dist" && exec python3 -m http.server "$port"

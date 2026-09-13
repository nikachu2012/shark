# Shark🦈 — コアと shark コマンドをビルドする
#
#   make            コアと shark コマンド、ランタイム（sharkvm）をビルドする
#   make test       tests/ を実行する
#   make docs       stdlib/ の宣言から HTML リファレンスを生成する
#   make docs-check 宣言に記述されたサンプルコードをすべて shark で実行確認する
#   make web        ブラウザ向け（WebAssembly版）をビルドする。Emscripten が必要
#   make web-serve  ビルド後、ローカルサーバーで配信する（http://localhost:8000/）
#   make clean
#
# Windows で Visual Studio を使うときは、この Makefile ではなく
# tools\build_win.bat を使用する（make や MinGW は不要）。README の
# 「Windows でビルドする」を参照。MSYS2 / MinGW の make なら、この Makefile がそのまま動作する。
#
# 例外と RTTI は使用しない（spec/skeleton.md）。外部ライブラリには依存しない。

CXX      ?= c++
CXXSTD   ?= -std=c++17
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
CORE_FLAGS = $(CXXSTD) $(CXXFLAGS) -fno-exceptions -fno-rtti

UNAME_S := $(shell uname -s)

# Windows の実行ファイルには .exe を付与する（付けないと毎回再ビルドされる）
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
EXE := .exe
else
EXE :=
endif

# ウィンドウ表示（std.ui）は OS のシステムライブラリを実行時に動的ロードする（dlopen / LoadLibrary）。
# そのためビルド時に必要な外部ライブラリはない。古い glibc では dlopen のために -ldl が必要
ifeq ($(UNAME_S),Linux)
LDLIBS += -ldl
endif

# 日本語フォント等を描画するための FreeType。唯一の外部ライブラリであり、リンクは任意。
# インストールされていれば自動検出し、無ければ内蔵の 5×7 ビットマップフォントのみを使用する。
#   make FREETYPE=0   インストールされていても使用しない
#   make FREETYPE=1   使用する（pkg-config で見つからないときは FT_CFLAGS/FT_LIBS を渡す）
# インストール方法は README の「日本語フォントの表示」を参照
FREETYPE ?= $(shell pkg-config --exists freetype2 2>/dev/null && echo 1 || echo 0)
ifeq ($(FREETYPE),1)
FT_CFLAGS ?= $(shell pkg-config --cflags freetype2)
FT_LIBS   ?= $(shell pkg-config --libs freetype2)
CORE_FLAGS += -DSHARK_FREETYPE $(FT_CFLAGS)
LDLIBS += $(FT_LIBS)
endif

# コアは大きく2つに分かれる。
#   RT_SRC  バイトコードを実行するためのランタイム（sharkvm はこれだけで構成）
#   FE_SRC  ソースコードからバイトコードを生成するフロントエンド（字句解析・構文解析・型検査・コード生成）
RT_SRC = \
  core/support.cpp core/value.cpp core/program.cpp core/types.cpp core/diag.cpp \
  core/vm.cpp core/registry.cpp core/bytecode.cpp core/runtime.cpp \
  core/platform/desktop.cpp core/platform/console.cpp \
  core/lib/format.cpp core/lib/builtin.cpp core/lib/math.cpp core/lib/time.cpp \
  core/lib/task.cpp core/lib/fmt.cpp core/lib/path.cpp core/lib/file.cpp \
  core/lib/os.cpp core/lib/text.cpp core/lib/json.cpp core/lib/test.cpp \
  core/lib/crypto.cpp core/lib/ui.cpp

FE_SRC = \
  core/lexer.cpp core/parser.cpp core/check.cpp core/codegen.cpp core/fmt_src.cpp \
  core/shark.cpp

CORE_SRC = $(RT_SRC) $(FE_SRC)

FRONT_SRC = frontend/main.cpp
VM_FRONT_SRC = frontend/vm_main.cpp

OBJ = $(CORE_SRC:.cpp=.o) $(FRONT_SRC:.cpp=.o)
VM_OBJ = $(RT_SRC:.cpp=.o) $(VM_FRONT_SRC:.cpp=.o)
HDR = $(wildcard core/*.h core/platform/*.h core/platform/*.inc core/lib/*.inc frontend/*.h)

all: shark$(EXE) sharkvm$(EXE)

# Shark 自身で実装されたコード（ソート処理および宣言的 UI のエントリポイント）。
# コア自身はファイル入出力を行わないため、C++ ヘッダとして埋め込む
core/prelude.h: stdlib/prelude.shk stdlib/prelude_ui.shk tools/prelude.py
	@python3 tools/prelude.py

# ゲームへの組み込み例（spec/runtime/embedding.md）
#   game        プレイヤーが記述したコードをその場で解析・実行する例
#   play_stage  ステージデータをあらかじめバイトコード化して埋め込んだ例
embed: examples/embed/game$(EXE) examples/embed/play_stage$(EXE)
examples/embed/game$(EXE): examples/embed/game.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# バイトコード埋め込みの例（spec/runtime/bytecode.md）。
#   build_stage  開発環境用ツール。stage.shk を C の配列定義へ変換する
#   play_stage   実行環境側。ランタイム（RT_SRC）のみをリンクする
examples/embed/build_stage$(EXE): examples/embed/build_stage.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)
# ホスト関数のテーブルを変更した場合は再生成が必要（シグネチャが不一致になるため）
examples/embed/game.o examples/embed/build_stage.o examples/embed/play_stage.o: \
  examples/embed/game_hosts.h
examples/embed/stage_bytecode.h: examples/embed/build_stage$(EXE) examples/embed/stage.shk \
  examples/embed/game_hosts.h
	@./examples/embed/build_stage$(EXE) examples/embed/stage.shk $@
examples/embed/play_stage.o: examples/embed/stage_bytecode.h
examples/embed/play_stage$(EXE): examples/embed/play_stage.o $(RT_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

shark$(EXE): $(OBJ)
	$(CXX) $(CORE_FLAGS) -o $@ $(OBJ) $(LDLIBS)

# バイトコード実行専用ランタイム（spec/runtime/bytecode.md）。
# shark build が本バイナリを元に単一実行ファイルを生成するため、shark と同時にビルドする
sharkvm$(EXE): $(VM_OBJ)
	$(CXX) $(CORE_FLAGS) -o $@ $(VM_OBJ) $(LDLIBS)

# ヘッダ変更時はすべて再コンパイルする（構造体のレイアウト等が変化するため）
%.o: %.cpp $(HDR)
	$(CXX) $(CORE_FLAGS) -c $< -o $@

test: shark$(EXE) sharkvm$(EXE) tests/memcheck$(EXE) tests/bytecheck$(EXE) tests/imecheck$(EXE) \
      tests/uicheck$(EXE)
	@sh tests/run.sh

# メモリ解放漏れ（リーク）と上限チェックのテスト（tests/run.sh から呼ばれる）
tests/memcheck$(EXE): tests/memcheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# 不正なバイトコードを適切に拒否するか検証する（tests/run.sh から呼ばれる）
tests/bytecheck$(EXE): tests/bytecheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# IME（文字入力）処理をモック環境でテストする（tests/run.sh から呼ばれる）
tests/imecheck$(EXE): tests/imecheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# UIウィジェットのクリック・ホバー動作をモック環境でテストする（tests/run.sh から呼ばれる）
tests/uicheck$(EXE): tests/uicheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# コアのソース一覧を出力する（web/build.sh が使用）。
# 一覧の二重管理を防ぐため、この Makefile の定義をマスターとする
print-core-src:
	@echo $(CORE_SRC)

# 配布用パッケージを作成する（dist/shark-<ターゲット>.tar.gz、Windows は .zip）。
# 中身は実行ファイル・同梱フォント・サンプル。push時のCI（.github/workflows）も
# リリース配布時も、同じ tools/package.sh を呼ぶ
#   make dist NAME=macos-arm64   でターゲット名を指定可能（省略時は uname から自動判定）
NAME ?=
dist: all
	@sh tools/package.sh $(NAME)

# C・Python・Shark のベンチマーク比較
bench: shark$(EXE)
	@python3 bench/run.py

# リファレンス生成（docs/reference/）。stdlib/*.shk の宣言ファイルをマスターとして
# ライブラリごとに1ページずつ生成する。C++ 実装と定義の不整合があれば検出して報告する
docs:
	@python3 docs/gen.py

# 宣言ファイルに記載されたサンプルコードをすべて実行して検証する
docs-check: shark$(EXE)
	@python3 tools/runex.py

# ブラウザ向けビルド（web/README.md）。移植層は core/platform/web.cpp
web:
	@sh web/build.sh

# ビルドしてローカル配信する（web/serve.sh が内部で web/build.sh を呼ぶため、常に最新版が配信される）
#   make web-serve PORT=8080  でポート番号を変更可能
PORT ?= 8000
web-serve:
	@sh web/serve.sh $(PORT)

# ビルド成果物を Node.js でテストする（ヘッドレス実行）
web-test: web
	@node web/test.js

clean:
	rm -f $(OBJ) $(VM_OBJ) examples/embed/game.o examples/embed/game$(EXE) tests/memcheck.o tests/memcheck$(EXE) shark$(EXE) sharkvm$(EXE)
	rm -f tests/bytecheck.o tests/bytecheck$(EXE)
	rm -f tests/imecheck.o tests/imecheck$(EXE)
	rm -f tests/uicheck.o tests/uicheck$(EXE)
	rm -f examples/embed/build_stage.o examples/embed/build_stage$(EXE) examples/embed/play_stage.o \
	      examples/embed/play_stage$(EXE) examples/embed/stage_bytecode.h
	rm -rf docs/reference
	rm -rf bench/build web/dist dist

.PHONY: all test clean dist embed bench docs docs-check web web-serve web-test print-core-src

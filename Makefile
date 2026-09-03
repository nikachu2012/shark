# Shark🦈 — コアと shark コマンドをビルドする
#
#   make            コアと shark コマンド、実行装置（sharkvm）を作る
#   make test       tests/ を走らせる
#   make docs       stdlib/ の宣言から HTML のリファレンスを作る
#   make docs-check 宣言に書いた例を、ぜんぶ本物の shark で動かす
#   make web        ブラウザで動く形（WebAssembly）を作る。Emscripten が要る
#   make web-serve  作ってから、その場で配る（http://localhost:8000/）
#   make clean
#
# Windows で Visual Studio を使うときは、この Makefile ではなく
# tools\build_win.bat を使う（make も MinGW も要らない）。README の
# 「Windows で作る」を見ること。MSYS2 / MinGW の make なら、これがそのまま動く。
#
# 例外と RTTI は使わない（spec/skeleton.md）。外部ライブラリには依存しない。

CXX      ?= c++
CXXSTD   ?= -std=c++17
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
CORE_FLAGS = $(CXXSTD) $(CXXFLAGS) -fno-exceptions -fno-rtti

UNAME_S := $(shell uname -s)

# Windows の実行ファイルには .exe が付く（付けないと、毎回作り直しになる）
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
EXE := .exe
else
EXE :=
endif

# 窓（std.ui）は OS の道具を**実行時に**取りに行く（dlopen / LoadLibrary）。
# そのため作るときに要るライブラリは無い。古い glibc だけ dlopen が -ldl にある
ifeq ($(UNAME_S),Linux)
LDLIBS += -ldl
endif

# 日本語などの字を出すための FreeType。**唯一の外部ライブラリ**で、任意。
# 入っていれば自動で使い、無ければ内蔵の 5×7 の字形だけになる。
#   make FREETYPE=0   入っていても使わない
#   make FREETYPE=1   使う（pkg-config で見つからないときは FT_CFLAGS/FT_LIBS を渡す）
# 入れ方は README の「日本語の字を出す」
FREETYPE ?= $(shell pkg-config --exists freetype2 2>/dev/null && echo 1 || echo 0)
ifeq ($(FREETYPE),1)
FT_CFLAGS ?= $(shell pkg-config --cflags freetype2)
FT_LIBS   ?= $(shell pkg-config --libs freetype2)
CORE_FLAGS += -DSHARK_FREETYPE $(FT_CFLAGS)
LDLIBS += $(FT_LIBS)
endif

# コアは2つに分かれる。
#   RT_SRC  バイトコードを動かすのに要るもの（＝実行装置。sharkvm はこれだけ）
#   FE_SRC  ソースからバイトコードを作るところ（字句解析・構文解析・型検査・コード生成）
RT_SRC = \
  core/support.cpp core/value.cpp core/program.cpp core/types.cpp core/diag.cpp \
  core/vm.cpp core/registry.cpp core/bytecode.cpp core/runtime.cpp \
  core/platform/desktop.cpp core/platform/console.cpp \
  core/lib/format.cpp core/lib/builtin.cpp core/lib/math.cpp core/lib/time.cpp \
  core/lib/task.cpp core/lib/fmt.cpp core/lib/path.cpp core/lib/file.cpp \
  core/lib/os.cpp core/lib/text.cpp core/lib/json.cpp core/lib/test.cpp \
  core/lib/crypto.cpp core/lib/ui.cpp

FE_SRC = \
  core/lexer.cpp core/parser.cpp core/check.cpp core/codegen.cpp core/shark.cpp

CORE_SRC = $(RT_SRC) $(FE_SRC)

FRONT_SRC = frontend/main.cpp
VM_FRONT_SRC = frontend/vm_main.cpp

OBJ = $(CORE_SRC:.cpp=.o) $(FRONT_SRC:.cpp=.o)
VM_OBJ = $(RT_SRC:.cpp=.o) $(VM_FRONT_SRC:.cpp=.o)
HDR = $(wildcard core/*.h core/platform/*.h core/platform/*.inc core/lib/*.inc frontend/*.h)

all: shark$(EXE) sharkvm$(EXE)

# Shark 自身で書いた部分（並べ替えと、宣言的 UI の入り口）。
# コアはファイルを読まないので埋め込む
core/prelude.h: stdlib/prelude.shk stdlib/prelude_ui.shk tools/prelude.py
	@python3 tools/prelude.py

# ゲームに組み込む例（spec/runtime/embedding.md）
#   game        プレイヤーが書いたコードを、その場で読んで動かす
#   play_stage  作者のステージを、バイトコードにして焼き込んだもの
embed: examples/embed/game$(EXE) examples/embed/play_stage$(EXE)
examples/embed/game$(EXE): examples/embed/game.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# バイトコードを焼き込む例（spec/runtime/bytecode.md）。
#   build_stage  開発機で動かす道具。stage.shk → C の配列
#   play_stage   ゲーム機に載る側。実行装置（RT_SRC）だけをリンクする
examples/embed/build_stage$(EXE): examples/embed/build_stage.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)
# ホストの表を変えたら焼き直す（指紋が変わるので、古いままだと読めなくなる）
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

# バイトコードだけを動かす実行装置（spec/runtime/bytecode.md）。
# shark build がこれを土台にして単一バイナリを作るので、shark と一緒に作る
sharkvm$(EXE): $(VM_OBJ)
	$(CXX) $(CORE_FLAGS) -o $@ $(VM_OBJ) $(LDLIBS)

# ヘッダを直したら全部を作り直す（型の並びが変わるため）
%.o: %.cpp $(HDR)
	$(CXX) $(CORE_FLAGS) -c $< -o $@

test: shark$(EXE) sharkvm$(EXE) tests/memcheck$(EXE) tests/bytecheck$(EXE) tests/imecheck$(EXE) \
      tests/uicheck$(EXE)
	@sh tests/run.sh

# メモリの後始末と上限を見る（tests/run.sh から呼ばれる）
tests/memcheck$(EXE): tests/memcheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# 壊れたバイトコードを断るか見る（tests/run.sh から呼ばれる）
tests/bytecheck$(EXE): tests/bytecheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# 変換つきの文字入力（IME）を、偽の出し先で動かして見る（tests/run.sh から呼ばれる）
tests/imecheck$(EXE): tests/imecheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# 部品を押した・合わせたときの動きを、偽の出し先で見る（tests/run.sh から呼ばれる）
tests/uicheck$(EXE): tests/uicheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^ $(LDLIBS)

# コアのソース一覧を出す（web/build.sh が使う）。
# 一覧を2か所に書くと、片方だけ増えて気づけないので、ここを正とする
print-core-src:
	@echo $(CORE_SRC)

# C・Python・Shark の速さ比べ
bench: shark$(EXE)
	@python3 bench/run.py

# リファレンス（docs/reference/）。中身は stdlib/*.shk の宣言ファイルが正で、
# ライブラリごとに1枚ずつ作る。実装と食い違っていればここで知らせる
docs:
	@python3 docs/gen.py

# 宣言に書いた例を、ぜんぶ動かして確かめる
docs-check: shark$(EXE)
	@python3 tools/runex.py

# ブラウザで動かす（web/README.md）。移植層は core/platform/web.cpp
web:
	@sh web/build.sh

# 作ってから配る（web/serve.sh が中で web/build.sh を呼ぶので、いつも作りたてが出る）
#   make web-serve PORT=8080  で港（ポート）を変えられる
PORT ?= 8000
web-serve:
	@sh web/serve.sh $(PORT)

# 作ったものを node で確かめる（画面は出さない）
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
	rm -rf bench/build web/dist

.PHONY: all test clean embed bench docs docs-check web web-serve web-test print-core-src

# Shark🦈 — コアと shark コマンドをビルドする
#
#   make            コアと shark コマンドを作る
#   make test       tests/ を走らせる
#   make clean
#
# 例外と RTTI は使わない（spec/skeleton.md）。外部ライブラリには依存しない。

CXX      ?= c++
CXXSTD   ?= -std=c++17
CXXFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
CORE_FLAGS = $(CXXSTD) $(CXXFLAGS) -fno-exceptions -fno-rtti

CORE_SRC = \
  core/support.cpp core/value.cpp core/program.cpp core/types.cpp core/diag.cpp \
  core/lexer.cpp core/parser.cpp core/check.cpp core/codegen.cpp core/vm.cpp \
  core/registry.cpp core/shark.cpp \
  core/platform/desktop.cpp core/platform/console.cpp \
  core/lib/format.cpp core/lib/builtin.cpp core/lib/math.cpp core/lib/time.cpp \
  core/lib/task.cpp core/lib/fmt.cpp core/lib/path.cpp core/lib/file.cpp \
  core/lib/os.cpp core/lib/text.cpp core/lib/json.cpp core/lib/test.cpp

FRONT_SRC = frontend/main.cpp

OBJ = $(CORE_SRC:.cpp=.o) $(FRONT_SRC:.cpp=.o)
HDR = $(wildcard core/*.h core/platform/*.h core/lib/*.inc)

all: shark

# ゲームに組み込む例（spec/runtime/embedding.md）
embed: examples/embed/game
examples/embed/game: examples/embed/game.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^

shark: $(OBJ)
	$(CXX) $(CORE_FLAGS) -o $@ $(OBJ)

# ヘッダを直したら全部を作り直す（型の並びが変わるため）
%.o: %.cpp $(HDR)
	$(CXX) $(CORE_FLAGS) -c $< -o $@

test: shark tests/memcheck
	@sh tests/run.sh

# メモリの後始末と上限を見る（tests/run.sh から呼ばれる）
tests/memcheck: tests/memcheck.o $(CORE_SRC:.cpp=.o)
	$(CXX) $(CORE_FLAGS) -o $@ $^

# C・Python・Shark の速さ比べ
bench: shark
	@python3 bench/run.py

clean:
	rm -f $(OBJ) examples/embed/game.o examples/embed/game tests/memcheck.o tests/memcheck shark
	rm -rf bench/build

.PHONY: all test clean embed bench

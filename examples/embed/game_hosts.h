// game_hosts.h — ゲーム側の操作（move / turn / at_goal）とホスト関数の登録
//
// バイトコードはランタイム側の関数をスロット番号（インデックス）で参照します（spec/runtime/bytecode.md）。
// インデックスは登録順で決定されるため、**コンパイル側（build_stage）と実行側（play_stage）で
// 同一の関数群を同一順序で登録する**必要があります。
// 不整合を防ぐため、設定および登録処理を本ヘッダーに集約しています。
//
// シグネチャや順序に不整合がある場合は、バイトコードロード時にフィンガープリント検証でエラーとなります。
#ifndef SHARK_EXAMPLE_GAME_HOSTS_H
#define SHARK_EXAMPLE_GAME_HOSTS_H

#include <stdio.h>

#include "../../core/config.h"
#include "../../core/registry.h"
#include "../../core/value.h"
#include "../../core/vm.h"

namespace game {

using namespace shark;

// --- ゲームの中の世界 -----------------------------------------------------
static int g_x = 0, g_y = 0, g_dir = 0;   // 0:北 1:東 2:南 3:西
static const int kGoalX = 2, kGoalY = 2;

static NativeStatus h_move(VM& vm, Value* args, int n, Value& out) {
  (void)vm; (void)n;
  const Str& dir = ((StrObj*)val_deref(&args[0])->o)->s;
  int d = (dir == "back") ? (g_dir + 2) % 4 : g_dir;
  if (d == 0) g_y++;
  else if (d == 1) g_x++;
  else if (d == 2) g_y--;
  else g_x--;
  printf("      [ゲーム] ロボットが動いた → (%d, %d)\n", g_x, g_y);
  out = mk_void();
  return N_Ok;
}

static NativeStatus h_turn(VM& vm, Value* args, int n, Value& out) {
  (void)vm; (void)n;
  const Str& dir = ((StrObj*)val_deref(&args[0])->o)->s;
  g_dir = (dir == "right") ? (g_dir + 1) % 4 : (g_dir + 3) % 4;
  printf("      [ゲーム] 向きが変わった → %d\n", g_dir);
  out = mk_void();
  return N_Ok;
}

static NativeStatus h_at_goal(VM& vm, Value* args, int n, Value& out) {
  (void)vm; (void)args; (void)n;
  out = mk_bool(g_x == kGoalX && g_y == kGoalY);
  return N_Ok;
}

// --- コンパイラとランタイムで共通の設定 -----------------------------------
static Config game_config() {
  Config cfg;
  cfg.lang = LANG_JA;
  // ゲーム機ではファイルも OS も使わないことにする
  cfg.with_file = false;
  cfg.with_os = false;
  return cfg;
}

// Engine（コンパイラ側）と Runtime（実行側）は共通の register_host インターフェースを持つため、
// テンプレート関数で共通化して登録可能
template <class T>
void register_game_hosts(T& e) {
  TypeTable& t = e.types();
  e.register_host("move", h_move, t.t_void(), t.t_string());
  e.register_host("turn", h_turn, t.t_void(), t.t_string());
  e.register_host("at_goal", h_at_goal, t.t_bool());
}

}  // namespace game
#endif

#include "diag.h"

namespace shark {

Diagnostic& DiagBag::error(const char* code, const Str& msg) {
  Diagnostic d;
  d.severity = SEV_ERROR;
  d.code = Str(code);
  d.message = msg;
  d.file = file_;
  items_.push(d);
  return items_.back();
}

Diagnostic& DiagBag::warn(const char* code, const Str& msg) {
  Diagnostic d;
  d.severity = strict_ ? SEV_ERROR : SEV_WARNING;
  d.code = Str(code);
  d.message = msg;
  d.file = file_;
  items_.push(d);
  return items_.back();
}

bool DiagBag::has_error() const {
  for (int i = 0; i < items_.size(); i++) if (items_[i].severity == SEV_ERROR) return true;
  return false;
}

struct Explain { const char* code; const char* ja; const char* en; };

static const Explain kExplain[] = {
  {"E0001",
   "書き方が構文の規則に合っていません。かっこや ; の閉じ忘れがよくある原因です。",
   "The code does not match the grammar. A missing bracket or ';' is the usual cause."},
  {"E0003",
   "名前として使えないものが書かれています。if や class のような予約語はそのままでは名前にできず、\n"
   "名前の先頭に数字も置けません。予約語ではない、英字か _ で始まる名前を付けます。",
   "This cannot be used as a name. Reserved words such as if or class are not names, and a name\n"
   "cannot start with a digit. Pick a non-reserved name that starts with a letter or _."},
  {"E0101",
   "変数の型と、入れようとした値の型が違います。Shark は暗黙の型変換をしません。\n"
   "変換は int(x) float(x) string(x) のように必ず書きます。",
   "The variable's type and the assigned value's type differ. Shark never converts implicitly.\n"
   "Write the conversion explicitly: int(x), float(x), string(x)."},
  {"E0102",
   "int と float は混ぜて計算できません。どこで精度が変わったかを読んで分かるようにするためです。\n"
   "float(a) + b のように、どちらかに変換してから計算します。",
   "int and float cannot be mixed. Convert one side first: float(a) + b."},
  {"E0103",
   "if や while の条件に書けるのは bool だけです。0 や空文字列を偽としては扱いません。\n"
   "n != 0 や s.len() > 0 のように、比べた結果を書きます。",
   "Only bool can be used as a condition. Write a comparison such as n != 0."},
  {"E0104",
   "空の [] や {} だけでは要素の型が決まりません。推論はその行だけで完結します。\n"
   "var xs: list<int> = []; のように型を書きます。",
   "An empty [] or {} does not determine the element type. Write var xs: list<int> = [];"},
  {"E0201",
   "T? は「値がないかもしれない」型なので、T としてそのままは使えません。\n"
   "取り出し方は3つ: ?? で既定値を決める、if var で値がある時だけ処理する、! で断言する。",
   "T? may hold no value, so it cannot be used as T. Use ??, if var, or !."},
  {"E0202",
   "Result を返す関数の結果を捨てています。失敗に気づけません。\n"
   "try で投げ返す、if var で受け取る、_ = で意図して捨てる、のどれかを書きます。",
   "The Result of this call is discarded. Use try, if var, or _ = to be explicit."},
  {"E0301",
   "引数はコピーで渡ります。呼び出し先の書き換えを呼び出し元へ反映するには、\n"
   "受け取る側と呼ぶ側の両方に ref と書きます。",
   "Arguments are copies. To let the callee modify the caller's variable, write ref on both sides."},
  {"E0302",
   "ref はタスクの境界を越えられません。タスクには値をそのまま渡してコピーさせます。",
   "ref cannot cross a task boundary. Pass the value itself; it will be copied."},
  {"E0401",
   "親に virtual の無いメソッドは上書きできません。上書きしてよいものには親側に virtual を書きます。",
   "Only methods marked virtual in the parent can be overridden."},
  {"E0402",
   "親の virtual を上書きするときは override が要ります。書き間違いで別のメソッドが\n"
   "増えるのを防ぐためです。",
   "Overriding a virtual method requires the override keyword."},
  {"E0403",
   "純粋仮想（本体のない virtual）が残っているクラスは抽象クラスです。\n"
   "インスタンスを作るには、残りを全て override してください。",
   "A class with unimplemented pure virtual methods is abstract and cannot be instantiated."},
  {"E0501",
   "この処理系にはそのモジュールが入っていません。組み込む側が選べます\n"
   "（spec/library/overview.md）。",
   "This runtime was built without that module. The host chooses which modules to include."},
};

const char* diag_explain(const char* code, Lang lang) {
  for (unsigned i = 0; i < sizeof(kExplain) / sizeof(kExplain[0]); i++) {
    const char* c = kExplain[i].code;
    bool same = true;
    for (int k = 0; k < 6; k++) { if (c[k] != code[k]) { same = false; break; } if (!c[k]) break; }
    if (same) return lang == LANG_JA ? kExplain[i].ja : kExplain[i].en;
  }
  return 0;
}

}  // namespace shark

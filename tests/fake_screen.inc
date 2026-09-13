// fake_screen.inc — テスト用のモック画面・入力バックエンド実装
//
// 実際のウィンドウは開かず、描画バッファの内容をメモリ上に保持する。マウス操作やキー入力は
// テストシナリオからシミュレートイベントとして注入する。IME（テキスト入力）は macOS の
// NSTextInputClient / Web の CompositionEvent と同等のオフセット管理方式を採用している
// （tests/imecheck.cpp で検証）。
//
// tests/imecheck.cpp および tests/uicheck.cpp で使用。
// 差し替えた PlatformScreen インスタンスはプログラム終了まで生存させること（ローカル変数にすると
// main 終了後のクリーンアップ時にダングリング参照となるため）。
// --- モック画面実装 --------------------------------------------------------
namespace fake {

static const uint32_t kAccent = 0x4c9be8;   // ui.theme のアクセントカラー（既定）

struct Ime {
  Vec<int> full;          // 入力バッファ全体のコードポイント（未確定テキストを含む）
  int mark_at, mark_len;  // 未確定テキスト（コンポジション）の範囲（full のインデックス）
  int sel_loc, sel_len;   // 選択範囲・カーソル位置（full のインデックス）
  bool on, multi;
  Ime() : mark_at(0), mark_len(0), sel_loc(0), sel_len(0), on(false), multi(false) {}
};
Ime im;

bool g_open = false;
Vec<ScreenEvent> q;
int qh = 0;
Vec<uint32_t> px;
int pw = 0, ph = 0;
int frame = 0;

// UTF-8 文字列とコードポイント配列の相互変換
void to_cps(const char* s, Vec<int>* out) {
  for (int i = 0; s && s[i];) {
    unsigned char c = (unsigned char)s[i];
    int cp, n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; n = 2; }
    else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
    out->push(cp);
    i += n;
  }
}
void put_cp(unsigned int cp, Str* out) {
  if (cp < 0x80) { out->push((char)cp); return; }
  if (cp < 0x800) {
    out->push((char)(0xc0 | (cp >> 6)));
    out->push((char)(0x80 | (cp & 0x3f)));
    return;
  }
  out->push((char)(0xe0 | (cp >> 12)));
  out->push((char)(0x80 | ((cp >> 6) & 0x3f)));
  out->push((char)(0x80 | (cp & 0x3f)));
}
Str to_utf8(const Vec<int>& v, int from, int upto) {
  Str r;
  for (int i = from; i < upto && i < v.size(); i++) put_cp((unsigned int)v[i], &r);
  return r;
}

// デバイスピクセル比（DPI スケール）。既定は等倍（1.0）。分数スケール（Windows の 150%、
// ブラウザのズーム等）をテストする場合はテスト側から ratio を変更する。
double ratio = 1.0;
double pixel_ratio() { return ratio; }
int scale() { return (int)(ratio + 0.5); }
bool open(const char* t, int w, int h) { (void)t; (void)w; (void)h; g_open = true; return true; }
void close() { g_open = false; }

// 毎フレームの描画完了時に呼ばれるコールバック。テストシナリオの進行に使用する。
// e.step() は VM 命令ステップ数単位のため 1 呼び出しで複数フレーム進むことがある。
// 正確にフレーム数を計測する場合はこのコールバックを使用する。
void (*on_frame)() = 0;

bool same = false;   // 前フレームと同じ描画内容か（アニメーションの静止判定に使用）

void present(const uint32_t* p, int w, int h) {
  same = (pw == w && ph == h && px.size() == w * h);
  if (same)
    for (int i = 0; i < w * h; i++)
      if (px[i] != p[i]) { same = false; break; }
  px.clear();
  for (int i = 0; i < w * h; i++) px.push(p[i]);
  pw = w;
  ph = h;
  frame++;
  if (on_frame) on_frame();
}
// 画面描画が静止しているか（イージングスクロールが目標位置に到達したか等の判定）
bool still() { return same; }

bool poll(ScreenEvent* out) {
  if (qh >= q.size()) { q.clear(); qh = 0; return false; }
  *out = q[qh++];
  return true;
}

void mouse(int x, int y, int code, bool down) {
  ScreenEvent e;
  e.kind = SEV_Mouse;
  e.code = code;
  e.down = down;
  e.x = x;
  e.y = y;
  q.push(e);
}
void key(int code, bool down) {
  ScreenEvent e;
  e.kind = SEV_Key;
  e.code = code;
  e.down = down;
  q.push(e);
}
void closed() {
  ScreenEvent e;
  e.kind = SEV_Close;
  q.push(e);
}

// --- IME モック（macOS / ブラウザ準拠のインデックス管理）-------------------
void text_input(bool on, const char* initial, int x, int y, int h, bool multi) {
  (void)x; (void)y; (void)h;
  if (!on) {
    im.on = false;
    im.full.clear();
    im.mark_at = im.mark_len = im.sel_loc = im.sel_len = 0;
    return;
  }
  im.on = true;
  im.multi = multi;
  if (initial) {
    im.full.clear();
    to_cps(initial, &im.full);
    im.mark_at = im.mark_len = 0;
    im.sel_loc = im.full.size();
    im.sel_len = 0;
  }
}
// 確定テキスト（未確定テキストを除外した文字列）を取得
bool text_state(Str* confirmed, Str* marked) {
  if (!im.on) return false;
  *confirmed = to_utf8(im.full, 0, im.mark_at);
  *confirmed += to_utf8(im.full, im.mark_at + im.mark_len, im.full.size());
  *marked = to_utf8(im.full, im.mark_at, im.mark_at + im.mark_len);
  return true;
}
// 選択範囲（未確定テキストも含めたインデックス管理）を取得
bool text_selection(int* start, int* len) {
  if (!im.on) return false;
  *start = im.sel_loc;
  *len = im.sel_len;
  return true;
}
void text_select(int start, int len) {
  if (!im.on) return;
  im.sel_loc = start;
  im.sel_len = len;
}
void text_replace(const char* s) {
  if (!im.on || im.mark_len > 0) return;   // コンポジション中（未確定状態）は置換不可（macOS 準拠）
  Vec<int> ins;
  to_cps(s, &ins);
  for (int i = 0; i < im.sel_len; i++) im.full.remove(im.sel_loc);
  for (int i = 0; i < ins.size(); i++) im.full.insert(im.sel_loc + i, ins[i]);
  im.sel_loc += ins.size();
  im.sel_len = 0;
}

// 未確定テキスト（コンポジション文字列）を更新
void compose(const char* s) {
  if (im.mark_len == 0) {
    for (int i = 0; i < im.sel_len; i++) im.full.remove(im.sel_loc);
    im.sel_len = 0;
    im.mark_at = im.sel_loc;
  }
  for (int i = 0; i < im.mark_len; i++) im.full.remove(im.mark_at);
  Vec<int> cps;
  to_cps(s, &cps);
  for (int i = 0; i < cps.size(); i++) im.full.insert(im.mark_at + i, cps[i]);
  im.mark_len = cps.size();
  im.sel_loc = im.mark_at + im.mark_len;   // 変換中は末尾にカーソルが移動
  im.sel_len = 0;
}
// 確定テキストをカーソル位置に直接挿入（IME を経由しない直接入力）
void input(const char* t) {
  Vec<int> cps;
  to_cps(t, &cps);
  for (int i = 0; i < im.sel_len; i++) im.full.remove(im.sel_loc);
  im.sel_len = 0;
  for (int i = 0; i < cps.size(); i++) im.full.insert(im.sel_loc + i, cps[i]);
  im.sel_loc += cps.size();
}
// カーソル位置を左右に移動
void move(int by) {
  im.sel_loc += by;
  if (im.sel_loc < 0) im.sel_loc = 0;
  if (im.sel_loc > im.full.size()) im.sel_loc = im.full.size();
  im.sel_len = 0;
}
// 未確定テキストを確定（コンポジション文字列を確定バッファに結合）
void commit() {
  im.sel_loc = im.mark_at + im.mark_len;
  im.sel_len = 0;
  im.mark_at = im.sel_loc;
  im.mark_len = 0;
}

void reset() {
  im = Ime();
  ratio = 1.0;
  frame = 0;
  same = false;
  q.clear();
  qh = 0;
  px.clear();
  pw = ph = 0;
}

const PlatformScreen kScreen = {
    scale, open, close, present, poll, true,
    text_input, text_state, text_selection, text_select, text_replace,
    0, 0,   // クリップボード機能はモック未実装
    0,      // リサイズ中の再描画コールバック
    0,      // マウスカーソル形状設定
    0,      // ウィンドウサイズ変更
    false,  // タイマーイベントは手動シミュレート可能
    0,      // ネイティブファイルダイアログはモック未実装
    pixel_ratio,
};


// 画面バッファの指定座標のピクセル値を取得
uint32_t at(int x, int y) {
  if (x < 0 || y < 0 || x >= pw || y >= ph) return 0;
  return px[y * pw + x];
}
// マウスホイールを回転（単位: 1/100 行、platform.h 準拠）
void wheel_fine(int dy) {
  ScreenEvent e;
  e.kind = SEV_Wheel;
  e.x = 0;
  e.y = dy;
  q.push(e);
}
// 行単位でホイールを回転（標準的な 1 ノッチ相当）
void wheel(int lines) { wheel_fine(lines * 100); }
// マウスカーソルを移動（ボタンは押さずにホバーのみ）
void hover(int x, int y) {
  ScreenEvent e;
  e.kind = SEV_Mouse;
  e.code = -1;
  e.down = false;
  e.x = x;
  e.y = y;
  q.push(e);
}
// テキスト文字の直接入力イベント（SEV_Text）
void type(const char* t) {
  ScreenEvent e;
  e.kind = SEV_Text;
  int i = 0;
  for (; i < 7 && t[i]; i++) e.text[i] = t[i];
  e.text[i] = 0;
  q.push(e);
}
// マウスクリック（マウスダウンとマウスアップを連続送信）
void click(int x, int y) {
  mouse(x, y, 0, true);
  mouse(x, y, 0, false);
}

}  // namespace fake

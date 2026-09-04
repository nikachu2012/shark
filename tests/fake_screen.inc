// fake_screen.inc — 検査のための、偽の出し先（画面と文字入力）
//
// 本物の窓は開かず、面の中身をこちらに控えるだけ。押した・合わせた・打った は
// 台本で流し込む。変換つきの文字入力（IME）は、**macOS の受け皿と同じ数え方**に
// してある（tests/imecheck.cpp が見張っているところ）。
//
// 使うのは tests/imecheck.cpp と tests/uicheck.cpp。
// 差し替えた移植層は**プログラムが終わるまで生かす**こと。自動変数にすると、
// main を抜けたあとの後始末（Vec の解放など）が消えた移植層を触ってしまう。
// --- 偽の出し先 -----------------------------------------------------------
namespace fake {

static const uint32_t kAccent = 0x4c9be8;   // ui.theme の差し色（既定）

struct Ime {
  Vec<int> full;          // 受け皿が持っている字（変換中の字も含む）
  int mark_at, mark_len;  // 変換中のところ（full の数え方）
  int sel_loc, sel_len;   // 選んでいるところ（full の数え方）
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

// UTF-8 と「字の並び」の行き来
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

// 画面の細かさ。既定は等倍。半端な細かさ（Windows の 150%、ブラウザの拡大）を
// 試したいときは、台本から ratio を差し替える
double ratio = 1.0;
double pixel_ratio() { return ratio; }
int scale() { return (int)(ratio + 0.5); }
bool open(const char* t, int w, int h) { (void)t; (void)w; (void)h; g_open = true; return true; }
void close() { g_open = false; }

// 1こま描き終わるたびに呼ばれる。台本を進めるのはここ。
// e.step() は「命令をいくつ動かすか」なので、1回で何こまも進むことがある。
// こま数を数えたいときは、必ずこちらを使う
void (*on_frame)() = 0;

bool same = false;   // 前のこまと同じ絵か（動きが止まったかを見るのに使う）

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
// 絵が動いていないか（巻物が目当てに着いたか）
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

// --- 受け皿（macOS / ブラウザと同じ数え方）-------------------------------
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
// 確定した中身は、変換中の字を**抜いた**もの
bool text_state(Str* confirmed, Str* marked) {
  if (!im.on) return false;
  *confirmed = to_utf8(im.full, 0, im.mark_at);
  *confirmed += to_utf8(im.full, im.mark_at + im.mark_len, im.full.size());
  *marked = to_utf8(im.full, im.mark_at, im.mark_at + im.mark_len);
  return true;
}
// 選んでいるところは、変換中の字も**数に入れた**もの（ここが食い違いのもと）
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
  if (!im.on || im.mark_len > 0) return;   // 変換中は触らない（macOS と同じ）
  Vec<int> ins;
  to_cps(s, &ins);
  for (int i = 0; i < im.sel_len; i++) im.full.remove(im.sel_loc);
  for (int i = 0; i < ins.size(); i++) im.full.insert(im.sel_loc + i, ins[i]);
  im.sel_loc += ins.size();
  im.sel_len = 0;
}

// 変換中の字を入れ替える（打つたびに伸びる）
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
  im.sel_loc = im.mark_at + im.mark_len;   // 変換中はうしろにカーソルが来る
  im.sel_len = 0;
}
// 確定した字を、カーソルのところに入れる（変換を通さずに打ったとき）
void input(const char* t) {
  Vec<int> cps;
  to_cps(t, &cps);
  for (int i = 0; i < im.sel_len; i++) im.full.remove(im.sel_loc);
  im.sel_len = 0;
  for (int i = 0; i < cps.size(); i++) im.full.insert(im.sel_loc + i, cps[i]);
  im.sel_loc += cps.size();
}
// カーソルを左右に動かす（受け皿が持っている機種では、これは OS の仕事）
void move(int by) {
  im.sel_loc += by;
  if (im.sel_loc < 0) im.sel_loc = 0;
  if (im.sel_loc > im.full.size()) im.sel_loc = im.full.size();
  im.sel_len = 0;
}
// 確定する。変換中だった字はそのまま中身になる
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
    0, 0,   // 切り貼りの置き場は持たない
    0,      // 縁を引いている間の描き直し
    0,      // マウスの形
    0,      // 大きさを変えられない窓
    false,  // 刻みはこちらで作れる
    0,      // 機種のファイル選びは持たない
    pixel_ratio,
};


// 面から1点読む
uint32_t at(int x, int y) {
  if (x < 0 || y < 0 || x >= pw || y >= ph) return 0;
  return px[y * pw + x];
}
// 車輪（ホイール）を回す。渡す単位は 1/100 行（platform.h）
void wheel_fine(int dy) {
  ScreenEvent e;
  e.kind = SEV_Wheel;
  e.x = 0;
  e.y = dy;
  q.push(e);
}
// 行ぶんで回す（1段ぶんの車輪を1回、のつもり）
void wheel(int lines) { wheel_fine(lines * 100); }
// カーソルを動かす（押さずに合わせるだけ）
void hover(int x, int y) {
  ScreenEvent e;
  e.kind = SEV_Mouse;
  e.code = -1;
  e.down = false;
  e.x = x;
  e.y = y;
  q.push(e);
}
// 字が打たれた（入力欄に入れていないときの道。SEV_Text）
void type(const char* t) {
  ScreenEvent e;
  e.kind = SEV_Text;
  int i = 0;
  for (; i < 7 && t[i]; i++) e.text[i] = t[i];
  e.text[i] = 0;
  q.push(e);
}
// 押して離す（1回のまわしで両方流す）
void click(int x, int y) {
  mouse(x, y, 0, true);
  mouse(x, y, 0, false);
}

}  // namespace fake

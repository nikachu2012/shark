// screen_term.inc — 端末を画面として使う（desktop.cpp から読む）
//
// 外部の窓の道具（GUI ツールキット）には頼らない（spec/runtime/platform.md）。
// 代わりに端末を画面として使う。升目1つに**上下2画素**を詰め、
// 上半分の四角（▀）の文字色を上の画素、背景色を下の画素にすると、
// 1文字で2画素になる。色は 24 ビット指定（\e[38;2;r;g;bm）。
//
//   ・端末でないとき（結果を | や > で受けているとき）は open が false を返す。
//     そのとき std.ui は見えない面に描くだけになり、出力は汚れない
//   ・離した合図は端末には来ない。has_key_up は false
#if defined(_WIN32)

const PlatformScreen* term_screen() { return 0; }

#else

struct TermState {
  bool open;
  int sw, sh;            // 面の大きさ（画素）
  int ow, oh;            // 画面に出す大きさ（画素）
  double scale;          // 面の画素 → 画面の画素
  int cols, crows;       // 使っている升目の数
  Vec<uint32_t> prev;    // 前に出した升の色（上・下の順に2つずつ）
  bool has_prev;
  struct termios saved;
  bool saved_ok;
  char in[512];          // まだ解釈していない入力
  int inlen;
  bool esc_pending;      // ESC が1つだけ残っている（次に何も来なければキーとして出す）
  TermState() : open(false), sw(0), sh(0), ow(0), oh(0), scale(1), cols(0), crows(0),
                has_prev(false), saved_ok(false), inlen(0), esc_pending(false) {}
};
TermState g_ts;

// 出来事の待ち行列。端末では取りこぼしても困らないので、あふれたら捨てる
ScreenEvent g_evq[128];
int g_evh = 0, g_evn = 0;

void ev_push(const ScreenEvent& e) {
  if (g_evn >= 128) return;
  g_evq[(g_evh + g_evn) % 128] = e;
  g_evn++;
}
void ev_key(int code, bool down) {
  ScreenEvent e; e.kind = SEV_Key; e.code = code; e.down = down; ev_push(e);
}
void ev_text(const char* s, int n) {
  if (n <= 0 || n > 7) return;
  ScreenEvent e; e.kind = SEV_Text;
  for (int i = 0; i < n; i++) e.text[i] = s[i];
  e.text[n] = 0;
  ev_push(e);
}

void t_write(const char* s) { fwrite(s, 1, sk_strlen(s), stdout); }

void t_restore() {
  if (!g_ts.open) return;
  g_ts.open = false;
  // 出したものを片付ける順は、入れたときの逆
  t_write("\x1b[0m\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?7h\x1b[?25h\x1b[?1049l");
  fflush(stdout);
  if (g_ts.saved_ok) tcsetattr(0, TCSAFLUSH, &g_ts.saved);
}
// 途中で終わっても端末を元に戻す（panic やメモリ上限で抜けたとき）
void t_atexit() { t_restore(); }

bool t_open(const char* title, int w, int h) {
  if (g_ts.open) t_restore();
  // 端末でなければ画面は持てない。ここで false を返すと、コアは見えない面に描く
  if (!isatty(0) || !isatty(1)) return false;
  if (w <= 0 || h <= 0) return false;
  g_ts.saved_ok = (tcgetattr(0, &g_ts.saved) == 0);
  if (!g_ts.saved_ok) return false;

  struct termios raw = g_ts.saved;
  // 打った文字をそのまま受け取る。行の確定も、こだまも、Ctrl-C の割り込みも止める
  // （Ctrl-C は 0x03 として届き、「閉じてくれ」に読み替える）。
  // 出す側（OPOST）は触らない。print の改行をそのままにしておくため
  raw.c_lflag = (tcflag_t)(raw.c_lflag & ~(ECHO | ICANON | ISIG | IEXTEN));
  raw.c_iflag = (tcflag_t)(raw.c_iflag & ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
  raw.c_cc[VMIN] = 0;   // 待たない
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(0, TCSAFLUSH, &raw) != 0) return false;

  // 別の画面に切り替える（抜けたときに、元の端末の中身が戻る）
  t_write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J");
  t_write("\x1b[?1000h\x1b[?1002h\x1b[?1006h");   // マウス（押す・引きずる・SGR 形式）
  if (title && title[0]) {   // 窓の名札。端末が対応していなければ何も起きない
    t_write("\x1b]0;");
    t_write(title);
    t_write("\x07");
  }
  fflush(stdout);

  g_ts.open = true;
  g_ts.sw = w; g_ts.sh = h;
  g_ts.has_prev = false;
  g_ts.prev.clear();
  g_ts.inlen = 0;
  g_ts.esc_pending = false;
  g_evh = g_evn = 0;
  static bool registered = false;
  if (!registered) { registered = true; atexit(t_atexit); }
  return true;
}

void t_close() { t_restore(); }

void t_put_color(Str& out, uint32_t c, bool fg) {
  out += fg ? "\x1b[38;2;" : "\x1b[48;2;";
  out += str_from_int((int64_t)((c >> 16) & 0xff)); out.push(';');
  out += str_from_int((int64_t)((c >> 8) & 0xff));  out.push(';');
  out += str_from_int((int64_t)(c & 0xff));         out.push('m');
}

void t_present(const uint32_t* px, int w, int h) {
  if (!g_ts.open || w <= 0 || h <= 0) return;
  g_ts.sw = w; g_ts.sh = h;

  struct winsize ws;
  int cols = 80, rows = 24;
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    cols = ws.ws_col; rows = ws.ws_row;
  }
  rows -= 1;             // 一番下の行は、書くと流れる端末があるので空けておく
  if (rows < 1) rows = 1;

  // 縦横の比を保ったまま、入るところまで。拡大するときは整数倍だけにする
  // （半端に伸ばすと、画素の大きさが場所によって変わって見えるため）
  double s = (double)cols / (double)w;
  double sv = (double)(rows * 2) / (double)h;
  if (sv < s) s = sv;
  if (s >= 1.0) s = (double)(int)s;
  int ow = (int)((double)w * s);
  int oh = (int)((double)h * s);
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  int crows = (oh + 1) / 2;
  g_ts.scale = s; g_ts.ow = ow; g_ts.oh = oh; g_ts.cols = ow; g_ts.crows = crows;

  int cells = ow * crows * 2;
  if (g_ts.prev.size() != cells) {
    g_ts.prev.clear();
    for (int i = 0; i < cells; i++) g_ts.prev.push(0);
    g_ts.has_prev = false;
  }

  Str out;
  out.reserve(cells * 8);
  if (!g_ts.has_prev) out += "\x1b[H\x1b[2J";
  uint32_t last_fg = 0xffffffffu, last_bg = 0xffffffffu;
  int cur_r = -1, cur_c = -1;
  for (int cy = 0; cy < crows; cy++) {
    for (int cx = 0; cx < ow; cx++) {
      int sx = (int)((double)cx / s);
      if (sx >= w) sx = w - 1;
      int ty = (int)((double)(cy * 2) / s);
      if (ty >= h) ty = h - 1;
      uint32_t top = px[ty * w + sx];
      uint32_t bot = 0;
      if (cy * 2 + 1 < oh) {
        int by = (int)((double)(cy * 2 + 1) / s);
        if (by >= h) by = h - 1;
        bot = px[by * w + sx];
      }
      int at = (cy * ow + cx) * 2;
      if (g_ts.has_prev && g_ts.prev[at] == top && g_ts.prev[at + 1] == bot) continue;
      g_ts.prev[at] = top;
      g_ts.prev[at + 1] = bot;
      if (cur_r != cy || cur_c != cx) {
        out += "\x1b[";
        out += str_from_int(cy + 1); out.push(';');
        out += str_from_int(cx + 1); out.push('H');
        cur_r = cy; cur_c = cx;
      }
      if (top != last_fg) { t_put_color(out, top, true); last_fg = top; }
      if (bot != last_bg) { t_put_color(out, bot, false); last_bg = bot; }
      out += "\xe2\x96\x80";   // ▀（上半分の四角）
      cur_c++;
    }
  }
  g_ts.has_prev = true;
  if (out.size()) {
    out += "\x1b[0m";
    fwrite(out.data(), 1, (size_t)out.size(), stdout);
    fflush(stdout);
  }
}

// 升目の位置を、面の中の画素に直す
void t_cell_to_px(int col, int row, int* x, int* y) {
  double s = g_ts.scale > 0 ? g_ts.scale : 1.0;
  int px = (int)(((double)col + 0.5) / s);
  int py = (int)(((double)(row * 2) + 0.5) / s);
  if (px < 0) px = 0;
  if (py < 0) py = 0;
  if (px >= g_ts.sw) px = g_ts.sw - 1;
  if (py >= g_ts.sh) py = g_ts.sh - 1;
  *x = px; *y = py;
}

// 打たれた数字を読む。読めた桁数を返す
int t_read_int(const char* b, int n, int at, int* out) {
  int v = 0, k = 0;
  while (at + k < n && b[at + k] >= '0' && b[at + k] <= '9') {
    v = v * 10 + (b[at + k] - '0');
    k++;
  }
  *out = v;
  return k;
}

// 入力の並びを解釈して、待ち行列に積む。
// 途中で切れている並びは残しておき、続きが来てから読み直す。使った長さを返す
int t_parse(const char* b, int n) {
  int i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)b[i];
    if (c == 0x1b) {
      if (i + 1 >= n) break;                       // 続きがまだ来ていない
      if (b[i + 1] == 'O') {                       // F1〜F4
        if (i + 2 >= n) break;
        char k = b[i + 2];
        int code = 0;
        if (k == 'P') code = SKEY_F1;
        else if (k == 'Q') code = SKEY_F2;
        else if (k == 'R') code = SKEY_F3;
        else if (k == 'S') code = SKEY_F4;
        if (code) ev_key(code, true);
        i += 3;
        continue;
      }
      if (b[i + 1] != '[') { ev_key(SKEY_Escape, true); i += 1; continue; }
      if (i + 2 >= n) break;
      if (b[i + 2] == '<') {                       // マウス（SGR 形式）
        int p = i + 3, btn = 0, cx = 0, cy = 0, k;
        k = t_read_int(b, n, p, &btn); if (!k) { i += 3; continue; } p += k;
        if (p >= n) break;
        if (b[p] != ';') { i += 3; continue; }
        p++;
        k = t_read_int(b, n, p, &cx); if (!k) break; p += k;
        if (p >= n) break;
        if (b[p] != ';') { i += 3; continue; }
        p++;
        k = t_read_int(b, n, p, &cy); if (!k) break; p += k;
        if (p >= n) break;
        char fin = b[p];
        if (fin != 'M' && fin != 'm') { i += 3; continue; }
        p++;
        ScreenEvent e;
        e.kind = SEV_Mouse;
        t_cell_to_px(cx - 1, cy - 1, &e.x, &e.y);
        if (btn & 64) { i = p; continue; }         // 回した合図は渡さない
        if (btn & 32) { e.code = -1; e.down = false; }   // 動いた
        else { e.code = btn & 3; e.down = (fin == 'M'); }
        if (e.code == 3) { i = p; continue; }
        ev_push(e);
        i = p;
        continue;
      }
      // \e[A のような、1文字で終わるもの
      char k2 = b[i + 2];
      int code = 0;
      if (k2 == 'A') code = SKEY_Up;
      else if (k2 == 'B') code = SKEY_Down;
      else if (k2 == 'C') code = SKEY_Right;
      else if (k2 == 'D') code = SKEY_Left;
      else if (k2 == 'H') code = SKEY_Home;
      else if (k2 == 'F') code = SKEY_End;
      if (code) { ev_key(code, true); i += 3; continue; }
      if (k2 >= '0' && k2 <= '9') {                // \e[5~ や \e[1;2C のような、数字つき
        int v = 0, k = t_read_int(b, n, i + 2, &v);
        int p = i + 2 + k;
        if (p >= n) break;
        int mod = 0;
        if (b[p] == ';') {                         // 修飾キーつき（shift+矢印など）
          int mk = t_read_int(b, n, p + 1, &mod);
          if (!mk) { i = p + 1; continue; }
          p += 1 + mk;
          if (p >= n) break;
        }
        // 1 を引いたものが、押されている修飾キーの組み合わせ
        if (mod >= 2) {
          int m = mod - 1;
          if (m & 1) ev_key(SKEY_Shift, true);
          if (m & 2) ev_key(SKEY_Alt, true);
          if (m & 4) ev_key(SKEY_Ctrl, true);
        }
        if (b[p] != '~') {                         // 矢印などに修飾がついたもの
          char fin = b[p];
          int c3 = 0;
          if (fin == 'A') c3 = SKEY_Up;
          else if (fin == 'B') c3 = SKEY_Down;
          else if (fin == 'C') c3 = SKEY_Right;
          else if (fin == 'D') c3 = SKEY_Left;
          else if (fin == 'H') c3 = SKEY_Home;
          else if (fin == 'F') c3 = SKEY_End;
          if (c3) ev_key(c3, true);
          i = p + 1;
          continue;
        }
        {
          int c2 = 0;
          if (v == 1 || v == 7) c2 = SKEY_Home;
          else if (v == 2) c2 = 0;
          else if (v == 3) c2 = SKEY_Delete;
          else if (v == 4 || v == 8) c2 = SKEY_End;
          else if (v == 5) c2 = SKEY_PageUp;
          else if (v == 6) c2 = SKEY_PageDown;
          else if (v >= 15 && v <= 18) c2 = SKEY_F5 + (v - 15);
          else if (v >= 19 && v <= 21) c2 = SKEY_F9 + (v - 19);
          else if (v >= 23 && v <= 24) c2 = SKEY_F11 + (v - 23);
          if (c2) ev_key(c2, true);
          i = p + 1;
          continue;
        }
      }
      i += 3;        // 知らない並び
      continue;
    }
    if (c == 0x03) {                               // Ctrl-C
      ScreenEvent e; e.kind = SEV_Close; ev_push(e);
      i++;
      continue;
    }
    if (c == '\r' || c == '\n') { ev_key(SKEY_Enter, true); i++; continue; }
    if (c == 0x7f || c == 0x08) { ev_key(SKEY_Back, true); i++; continue; }
    if (c == '\t') { ev_key(SKEY_Tab, true); i++; continue; }
    if (c >= 32 && c < 127) {
      int key = c;
      if (key >= 'A' && key <= 'Z') key += 32;     // キーの名前は小文字で表す
      ev_key(key, true);
      char t[1]; t[0] = (char)c;
      ev_text(t, 1);
      i++;
      continue;
    }
    if (c >= 0xc0) {                               // UTF-8 の続き（打たれた文字として渡す）
      int len = c >= 0xf0 ? 4 : (c >= 0xe0 ? 3 : 2);
      if (i + len > n) break;
      ev_text(b + i, len);
      i += len;
      continue;
    }
    i++;                                           // そのほかの制御文字は捨てる
  }
  return i;
}

// いま来ている分だけ読む。**待たない**（spec/runtime/platform.md）
void t_read_input() {
  struct pollfd pf;
  pf.fd = 0;
  pf.events = POLLIN;
  bool got = false;
  while (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) {
    if (g_ts.inlen >= (int)sizeof g_ts.in) break;
    int r = (int)read(0, g_ts.in + g_ts.inlen, sizeof g_ts.in - (size_t)g_ts.inlen);
    if (r <= 0) break;
    g_ts.inlen += r;
    got = true;
  }
  if (g_ts.inlen == 0) return;
  int used = t_parse(g_ts.in, g_ts.inlen);
  if (used > 0) {
    for (int i = used; i < g_ts.inlen; i++) g_ts.in[i - used] = g_ts.in[i];
    g_ts.inlen -= used;
  }
  // ESC が1つだけ残ったときは、続きが来なかった回に「Esc が押された」とみなす
  if (g_ts.inlen == 1 && g_ts.in[0] == 0x1b) {
    if (g_ts.esc_pending && !got) {
      ev_key(SKEY_Escape, true);
      g_ts.inlen = 0;
      g_ts.esc_pending = false;
    } else {
      g_ts.esc_pending = true;
    }
  } else {
    g_ts.esc_pending = false;
  }
}

bool t_poll(ScreenEvent* out) {
  if (!g_ts.open) return false;
  if (g_evn == 0) t_read_input();
  if (g_evn == 0) return false;
  *out = g_evq[g_evh];
  g_evh = (g_evh + 1) % 128;
  g_evn--;
  return true;
}

// 端末の升目は細かさを持たない。等倍として扱う
int t_scale() { return 1; }

// 端末は「離した」を教えてくれないので has_key_up は false
// 変換は端末のソフトがやってくれて、確定した文字だけが届く。
// こちらで受け皿を持つ必要がない（→ text_input は 0）
const PlatformScreen kTermScreen = {t_scale, t_open, t_close, t_present, t_poll, false,
                                   0, 0, 0, 0, 0, 0, 0};

const PlatformScreen* term_screen() { return &kTermScreen; }

#endif  // _WIN32

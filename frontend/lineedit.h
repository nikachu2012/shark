// lineedit.h — REPL の1行入力（行の中の編集と、入力の履歴）
//
// 外のライブラリ（readline など）は使わない。端末を直に読む小さな実装。
//   ← → Home End        カーソルを動かす（Ctrl-A / Ctrl-E / Ctrl-B / Ctrl-F も）
//   ↑ ↓                 前に打った行を呼び戻す（Ctrl-P / Ctrl-N も）
//   Backspace Delete    1文字消す（Ctrl-H / Ctrl-D も）
//   Ctrl-U / Ctrl-K     カーソルより前 / 後ろを消す
//   Ctrl-W              前の単語を消す
//   Ctrl-L              画面を消す
//   Ctrl-C              打ちかけの入力を捨てる
//   Ctrl-D              空の行なら終わる
//
// 履歴は ~/.shark_history に残す（環境変数 SHARK_HISTORY で場所を変えられる。空なら残さない）。
// Windows の端末は、読み込み（ReadConsoleW）そのものが ↑ ↓ の履歴を持つので、そちらに任せる。
// 端末でないとき（流し込んだとき）は、ふつうに1行読むだけ。
#ifndef SHARK_FRONTEND_LINEEDIT_H
#define SHARK_FRONTEND_LINEEDIT_H

#include <stdio.h>

#include "../core/platform/platform.h"
#include "../core/value.h"
#include "host.h"

#if !defined(_WIN32)
#include <termios.h>
#include <unistd.h>
#endif

namespace shark {

enum LineResult { LINE_OK = 0, LINE_EOF, LINE_CANCEL };

class LineEditor {
 public:
  LineEditor() : limit_(1000) {}

  // 履歴のファイルを読む。無ければ何もしない
  void load_history() {
    path_ = history_path();
    if (!path_.size()) return;
    Str all;
    if (!read_file(path_, &all)) return;
    Str cur;
    for (int i = 0; i < all.size(); i++) {
      if (all[i] == '\n') { push_history(cur); cur.clear(); continue; }
      if (all[i] != '\r') cur.push(all[i]);
    }
    push_history(cur);
    // 長くなりすぎたファイルは、残す分だけに書き直す
    int lines = 0;
    for (int i = 0; i < all.size(); i++) if (all[i] == '\n') lines++;
    if (lines > limit_ * 2) save_all();
  }

  // 打ち終えた行を履歴に足し、ファイルにも書き足す
  void add(const Str& line) {
    if (!push_history(line)) return;
    if (!path_.size()) return;
    FILE* f = host_fopen(path_.c_str(), "ab");
    if (!f) return;
    fwrite(line.data(), 1, (size_t)line.size(), f);
    fputc('\n', f);
    fclose(f);
  }

  // prompt を出して1行読む
  LineResult read(const char* prompt, Str* out) {
    out->clear();
#if defined(_WIN32)
    return read_plain(prompt, out);
#else
    if (!isatty(0) || !isatty(1) || dumb_terminal()) return read_plain(prompt, out);
    struct termios saved;
    if (tcgetattr(0, &saved) != 0) return read_plain(prompt, out);
    struct termios raw = saved;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &raw) != 0) return read_plain(prompt, out);
    LineResult r = edit(prompt, out);
    tcsetattr(0, TCSAFLUSH, &saved);
    return r;
#endif
  }

 private:
  static Str history_path() {
    const PlatformOS* os = platform().os;
    if (!os || !os->env) return Str();
    Str p;
    if (os->env("SHARK_HISTORY", &p)) return p;   // 空なら残さない
    Str home;
    if (os->env("HOME", &home) && home.size()) return home + "/.shark_history";
    if (os->env("USERPROFILE", &home) && home.size()) return home + "\\.shark_history";
    return Str();
  }

  bool push_history(const Str& line) {
    bool blank = true;
    for (int i = 0; i < line.size(); i++)
      if (line[i] != ' ' && line[i] != '\t') { blank = false; break; }
    if (blank) return false;
    if (hist_.size() && hist_.back() == line) return false;   // 同じ行が続くときは1つだけ
    hist_.push(line);
    if (hist_.size() > limit_) hist_.remove(0);
    return true;
  }

  void save_all() {
    Str all;
    for (int i = 0; i < hist_.size(); i++) { all += hist_[i]; all += "\n"; }
    write_file(path_, all, false);
  }

  LineResult read_plain(const char* prompt, Str* out) {
    if (prompt && stdin_is_tty()) {
      fputs(prompt, stdout);
      fflush(stdout);
    }
    return platform().read_line(out) ? LINE_OK : LINE_EOF;
  }

#if !defined(_WIN32)
  static bool dumb_terminal() {
    Str t;
    if (!platform().os || !platform().os->env("TERM", &t)) return false;
    return t == "dumb";
  }

  static void put(const Str& s) {
    int at = 0;
    while (at < s.size()) {
      ssize_t n = ::write(1, s.data() + at, (size_t)(s.size() - at));
      if (n <= 0) return;
      at += (int)n;
    }
  }

  static bool read_byte(unsigned char* c) { return ::read(0, c, 1) == 1; }

  // 1文字（UTF-8）の切れ目
  static int prev_char(const Str& s, int at) {
    if (at <= 0) return 0;
    at--;
    while (at > 0 && ((unsigned char)s[at] & 0xC0) == 0x80) at--;
    return at;
  }
  static int next_char(const Str& s, int at) {
    if (at >= s.size()) return s.size();
    at++;
    while (at < s.size() && ((unsigned char)s[at] & 0xC0) == 0x80) at++;
    return at;
  }

  // 行を書き直す。全角は2つ分の幅として、カーソルの位置を合わせる
  void refresh(const char* prompt, const Str& buf, int pos) {
    Str p(prompt);
    Str s("\r");
    s += p;
    s += buf;
    s += "\x1b[K\r";
    int col = utf8_display_width(p) + utf8_display_width(buf.sub(0, pos));
    if (col > 0) {
      s += "\x1b[";
      s += str_from_int(col);
      s += "C";
    }
    put(s);
  }

  LineResult edit(const char* prompt, Str* out) {
    fflush(stdout);
    Str buf;
    int pos = 0;
    int hi = hist_.size();   // 履歴のどこを見ているか（末尾 = いま打っている行）
    Str draft;               // 履歴をさかのぼる前に打っていた行
    refresh(prompt, buf, pos);
    for (;;) {
      unsigned char c;
      if (!read_byte(&c)) {
        put(Str("\r\n"));
        return buf.size() ? (*out = buf, LINE_OK) : LINE_EOF;
      }
      switch (c) {
        case '\r':
        case '\n':
          put(Str("\r\n"));
          *out = buf;
          return LINE_OK;
        case 3:   // Ctrl-C
          put(Str("^C\r\n"));
          return LINE_CANCEL;
        case 4:   // Ctrl-D
          if (buf.size() == 0) {
            put(Str("\r\n"));
            return LINE_EOF;
          }
          if (pos < buf.size()) {
            int e = next_char(buf, pos);
            buf = buf.sub(0, pos) + buf.sub(e, buf.size() - e);
          }
          break;
        case 127:
        case 8:   // Backspace / Ctrl-H
          if (pos > 0) {
            int b = prev_char(buf, pos);
            buf = buf.sub(0, b) + buf.sub(pos, buf.size() - pos);
            pos = b;
          }
          break;
        case 1: pos = 0; break;                              // Ctrl-A
        case 5: pos = buf.size(); break;                     // Ctrl-E
        case 2: pos = prev_char(buf, pos); break;            // Ctrl-B
        case 6: pos = next_char(buf, pos); break;            // Ctrl-F
        case 11: buf = buf.sub(0, pos); break;               // Ctrl-K
        case 21:                                             // Ctrl-U
          buf = buf.sub(pos, buf.size() - pos);
          pos = 0;
          break;
        case 23: {                                           // Ctrl-W
          int b = pos;
          while (b > 0 && (buf[b - 1] == ' ' || buf[b - 1] == '\t')) b--;
          while (b > 0 && buf[b - 1] != ' ' && buf[b - 1] != '\t') b--;
          buf = buf.sub(0, b) + buf.sub(pos, buf.size() - pos);
          pos = b;
          break;
        }
        case 12: put(Str("\x1b[H\x1b[2J")); break;           // Ctrl-L
        case 16: history_move(-1, &hi, &draft, &buf, &pos); break;   // Ctrl-P
        case 14: history_move(1, &hi, &draft, &buf, &pos); break;    // Ctrl-N
        case 27: {   // ESC で始まる並び（矢印など）
          unsigned char a, b;
          if (!read_byte(&a) || !read_byte(&b)) break;
          if (a == '[' && b >= '0' && b <= '9') {
            unsigned char t;
            if (!read_byte(&t)) break;
            if (t != '~') break;   // ESC [ 1 ; 5 C のような修飾付きは読み捨てない（まれ）
            if (b == '3' && pos < buf.size()) {   // Delete
              int e = next_char(buf, pos);
              buf = buf.sub(0, pos) + buf.sub(e, buf.size() - e);
            }
            if (b == '1' || b == '7') pos = 0;            // Home
            if (b == '4' || b == '8') pos = buf.size();   // End
            break;
          }
          if (a == '[' || a == 'O') {
            if (b == 'A') history_move(-1, &hi, &draft, &buf, &pos);
            if (b == 'B') history_move(1, &hi, &draft, &buf, &pos);
            if (b == 'C') pos = next_char(buf, pos);
            if (b == 'D') pos = prev_char(buf, pos);
            if (b == 'H') pos = 0;
            if (b == 'F') pos = buf.size();
          }
          break;
        }
        default:
          if (c < 32) break;   // ほかの制御文字は無視する
          buf = buf.sub(0, pos) + Str((const char*)&c, 1) + buf.sub(pos, buf.size() - pos);
          pos++;
          // 全角などは、残りのバイトがそろってから描く
          if (c >= 0xC0) {
            int need = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
            for (int k = 0; k < need; k++) {
              unsigned char d;
              if (!read_byte(&d)) break;
              buf = buf.sub(0, pos) + Str((const char*)&d, 1) + buf.sub(pos, buf.size() - pos);
              pos++;
            }
          }
          break;
      }
      refresh(prompt, buf, pos);
    }
  }

  void history_move(int dir, int* hi, Str* draft, Str* buf, int* pos) {
    int next = *hi + dir;
    if (next < 0 || next > hist_.size()) return;
    if (*hi == hist_.size()) *draft = *buf;   // いま打っている行を取っておく
    *hi = next;
    *buf = next == hist_.size() ? *draft : hist_[next];
    *pos = buf->size();
  }
#endif

  Vec<Str> hist_;
  int limit_;
  Str path_;
};

}  // namespace shark
#endif

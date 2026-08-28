// screen_x11.inc — X11 の窓（desktop.cpp から読む）
//
// Linux などで使う。X11 の開発ファイルが無い機械でも作れるように、
// **ヘッダも共有ライブラリも作るときには要らない**形にしてある。
//
//   ・libX11 は dlopen で開き、要る関数だけ dlsym で引く
//   ・Xlib の構造体は、ここで必要なぶんだけ書き写す（並びは昔から変わっていない）
//   ・開けなければ false。そのとき desktop.cpp は端末に落とす
#if defined(_WIN32) || defined(__EMSCRIPTEN__)

const PlatformScreen* x11_screen() { return 0; }

#else

#include <dlfcn.h>

namespace x11 {

typedef unsigned long XID;      // Window / Drawable / Atom / KeySym / Time
typedef void Display;
typedef void Visual;
typedef void* GC;
typedef void XImage;

// 色ひとつぶん（XCreatePixmapCursor に渡す。中身は使わないので 0 でよい）
struct XColor { unsigned long pixel; unsigned short red, green, blue; char flags, pad; };

// XEvent は共用体。使うものだけ書き写す（Bool は int、XID は unsigned long）
struct AnyEvent { int type; unsigned long serial; int send_event; Display* display; XID window; };
struct KeyEvent {
  int type; unsigned long serial; int send_event; Display* display;
  XID window, root, subwindow; unsigned long time;
  int x, y, x_root, y_root; unsigned int state, keycode; int same_screen;
};
struct ButtonEvent {
  int type; unsigned long serial; int send_event; Display* display;
  XID window, root, subwindow; unsigned long time;
  int x, y, x_root, y_root; unsigned int state, button; int same_screen;
};
struct MotionEvent {
  int type; unsigned long serial; int send_event; Display* display;
  XID window, root, subwindow; unsigned long time;
  int x, y, x_root, y_root; unsigned int state; char is_hint; int same_screen;
};
struct ClientMessageEvent {
  int type; unsigned long serial; int send_event; Display* display;
  XID window; XID message_type; int format;
  union { char b[20]; short s[10]; long l[5]; } data;
};
struct ConfigureEvent {
  int type; unsigned long serial; int send_event; Display* display;
  XID event, window; int x, y, width, height, border_width; XID above; int override_redirect;
};
union Event {
  int type;
  AnyEvent xany;
  KeyEvent xkey;
  ButtonEvent xbutton;
  MotionEvent xmotion;
  ClientMessageEvent xclient;
  ConfigureEvent xconfigure;
  long pad[24];
};

enum {
  KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5,
  MotionNotify = 6, Expose = 12, ConfigureNotify = 22, ClientMessage = 33,
};
enum {
  KeyPressMask = 1L << 0, KeyReleaseMask = 1L << 1, ButtonPressMask = 1L << 2,
  ButtonReleaseMask = 1L << 3, PointerMotionMask = 1L << 6, ExposureMask = 1L << 15,
  StructureNotifyMask = 1L << 17,
};

// 窓の大きさの希望（XSetWMNormalHints）。使うものだけ書き写す
struct SizeHints {
  long flags;
  int x, y;
  int width, height;
  int min_width, min_height;
  int max_width, max_height;
  int width_inc, height_inc;
  struct { int x, y; } min_aspect, max_aspect;
  int base_width, base_height;
  int win_gravity;
};
enum { PMinSize = 1L << 4, PMaxSize = 1L << 5 };

// --- 引いてくる関数 -------------------------------------------------------
Display* (*OpenDisplay)(const char*) = 0;
int (*CloseDisplay)(Display*) = 0;
int (*DefaultScreen)(Display*) = 0;
XID (*RootWindow)(Display*, int) = 0;
Visual* (*DefaultVisual)(Display*, int) = 0;
int (*DefaultDepth)(Display*, int) = 0;
unsigned long (*BlackPixel)(Display*, int) = 0;
XID (*CreateSimpleWindow)(Display*, XID, int, int, unsigned int, unsigned int, unsigned int,
                          unsigned long, unsigned long) = 0;
int (*StoreName)(Display*, XID, const char*) = 0;
int (*ChangeProperty)(Display*, XID, XID, XID, int, int, const unsigned char*, int) = 0;
int (*SelectInput)(Display*, XID, long) = 0;
int (*MapWindow)(Display*, XID) = 0;
int (*DestroyWindow)(Display*, XID) = 0;
GC (*CreateGC)(Display*, XID, unsigned long, void*) = 0;
int (*FreeGC)(Display*, GC) = 0;
XImage* (*CreateImage)(Display*, Visual*, unsigned int, int, int, char*, unsigned int,
                       unsigned int, int, int) = 0;
int (*PutImage)(Display*, XID, GC, XImage*, int, int, int, int, unsigned int, unsigned int) = 0;
int (*Flush)(Display*) = 0;
int (*Pending)(Display*) = 0;
int (*NextEvent)(Display*, Event*) = 0;
XID (*InternAtom)(Display*, const char*, int) = 0;
int (*SetWMProtocols)(Display*, XID, XID*, int) = 0;
void (*SetWMNormalHints)(Display*, XID, SizeHints*) = 0;   // 無くてもよい
XID (*LookupKeysym)(KeyEvent*, int) = 0;
int (*LookupString)(KeyEvent*, char*, int, XID*, void*) = 0;
int (*XFree_)(void*) = 0;
int (*SetDetectableAutoRepeat)(Display*, int, int*) = 0;   // 無くてもよい
void* (*SetErrorHandler)(void*) = 0;                       // 無くてもよい
// マウスの形。どれも無くてもよい（取れなければ、形は変えないだけ）
XID (*CreateFontCursor)(Display*, unsigned int) = 0;
int (*DefineCursor)(Display*, XID, XID) = 0;
int (*FreeCursor)(Display*, XID) = 0;
XID (*CreateBitmapFromData)(Display*, XID, const char*, unsigned int, unsigned int) = 0;
XID (*CreatePixmapCursor)(Display*, XID, XID, XColor*, XColor*, unsigned int, unsigned int) = 0;
int (*FreePixmap)(Display*, XID) = 0;

// X の返す誤りで**プログラムごと終わらせない**。Xlib の既定は exit するので、
// 何もしない受け手に差し替えておく（画面が出ないだけで、続きは動く）
int quiet_error(Display*, void*) { return 0; }

struct State {
  bool open;
  Display* dpy;
  XID win;
  GC gc;
  XImage* img;
  uint32_t* buf;      // 画面に出す大きさの画素（scale 倍したもの）
  int sw, sh;         // 面の大きさ
  int rw, rh;         // 窓から最後に知らせた面の大きさ（SEV_Resize を重ねて出さないため）
  int scale;
  XID wm_delete;
  State() : open(false), dpy(0), win(0), gc(0), img(0), buf(0), sw(0), sh(0), rw(0), rh(0),
            scale(1), wm_delete(0) {}
};
State g;

ScreenEvent q[128];
int qh = 0, qn = 0;
void push(const ScreenEvent& e) {
  if (qn >= 128) return;
  q[(qh + qn) % 128] = e;
  qn++;
}
void key_ev(int code, bool down) {
  ScreenEvent e; e.kind = SEV_Key; e.code = code; e.down = down; push(e);
}

void* lib = 0;
template <class T>
bool grab(T* fn, const char* name) {
  *fn = (T)dlsym(lib, name);
  return *fn != 0;
}

bool load() {
  static int tried = 0;
  if (tried) return tried > 0;
  tried = -1;
  const char* names[] = {"libX11.so.6", "libX11.so", "/opt/X11/lib/libX11.6.dylib", 0};
  for (int i = 0; names[i] && !lib; i++) lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
  if (!lib) return false;
  bool ok = true;
  ok &= grab(&OpenDisplay, "XOpenDisplay");
  ok &= grab(&CloseDisplay, "XCloseDisplay");
  ok &= grab(&DefaultScreen, "XDefaultScreen");
  ok &= grab(&RootWindow, "XRootWindow");
  ok &= grab(&DefaultVisual, "XDefaultVisual");
  ok &= grab(&DefaultDepth, "XDefaultDepth");
  ok &= grab(&BlackPixel, "XBlackPixel");
  ok &= grab(&CreateSimpleWindow, "XCreateSimpleWindow");
  ok &= grab(&StoreName, "XStoreName");
  ok &= grab(&ChangeProperty, "XChangeProperty");
  ok &= grab(&SelectInput, "XSelectInput");
  ok &= grab(&MapWindow, "XMapWindow");
  ok &= grab(&DestroyWindow, "XDestroyWindow");
  ok &= grab(&CreateGC, "XCreateGC");
  ok &= grab(&FreeGC, "XFreeGC");
  ok &= grab(&CreateImage, "XCreateImage");
  ok &= grab(&PutImage, "XPutImage");
  ok &= grab(&Flush, "XFlush");
  ok &= grab(&Pending, "XPending");
  ok &= grab(&NextEvent, "XNextEvent");
  ok &= grab(&InternAtom, "XInternAtom");
  ok &= grab(&SetWMProtocols, "XSetWMProtocols");
  ok &= grab(&LookupKeysym, "XLookupKeysym");
  ok &= grab(&LookupString, "XLookupString");
  ok &= grab(&XFree_, "XFree");
  grab(&SetDetectableAutoRepeat, "XkbSetDetectableAutoRepeat");   // 無くてもよい
  grab(&SetWMNormalHints, "XSetWMNormalHints");                    // 無くてもよい
  grab(&SetErrorHandler, "XSetErrorHandler");
  grab(&CreateFontCursor, "XCreateFontCursor");     // ここから下は無くてもよい
  grab(&DefineCursor, "XDefineCursor");
  grab(&FreeCursor, "XFreeCursor");
  grab(&CreateBitmapFromData, "XCreateBitmapFromData");
  grab(&CreatePixmapCursor, "XCreatePixmapCursor");
  grab(&FreePixmap, "XFreePixmap");
  if (!ok) return false;
  if (SetErrorHandler) SetErrorHandler((void*)quiet_error);
  tried = 1;
  return true;
}

// X11 は画面の細かさを1つに決めていない。等倍として扱う
int screen_scale() { return 1; }

// 面が小さいときだけ、整数倍に広げる（大きい面は画面の画素と1対1）
int pick_scale(int w, int h) {
  int s = 1;
  while (s < 6 && w * (s + 1) <= 640 && h * (s + 1) <= 480) s++;
  return s;
}

bool open(const char* title, int w, int h) {
  if (!load()) return false;
  if (g.open) return false;
  const char* disp = getenv("DISPLAY");
  if (!disp || !disp[0]) return false;
  g.dpy = OpenDisplay(0);
  if (!g.dpy) return false;

  int scr = DefaultScreen(g.dpy);
  g.scale = pick_scale(w, h);
  unsigned int ww = (unsigned int)(w * g.scale), wh = (unsigned int)(h * g.scale);
  g.win = CreateSimpleWindow(g.dpy, RootWindow(g.dpy, scr), 0, 0, ww, wh, 0,
                             BlackPixel(g.dpy, scr), BlackPixel(g.dpy, scr));
  if (!g.win) { CloseDisplay(g.dpy); g.dpy = 0; return false; }

  const char* t = (title && title[0]) ? title : "Shark";
  StoreName(g.dpy, g.win, t);
  // 名札は UTF-8 でも渡しておく（XStoreName は Latin-1 しか通らない）
  XID net_name = InternAtom(g.dpy, "_NET_WM_NAME", 0);
  XID utf8s = InternAtom(g.dpy, "UTF8_STRING", 0);
  if (net_name && utf8s)
    ChangeProperty(g.dpy, g.win, net_name, utf8s, 8, 0, (const unsigned char*)t,
                   (int)sk_strlen(t));

  g.wm_delete = InternAtom(g.dpy, "WM_DELETE_WINDOW", 0);
  if (g.wm_delete) SetWMProtocols(g.dpy, g.win, &g.wm_delete, 1);
  SelectInput(g.dpy, g.win, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                                PointerMotionMask | ExposureMask | StructureNotifyMask);
  if (SetDetectableAutoRepeat) {   // 押しっぱなしで離した合図が挟まらないようにする
    int supported = 0;
    SetDetectableAutoRepeat(g.dpy, 1, &supported);
  }
  MapWindow(g.dpy, g.win);

  g.gc = CreateGC(g.dpy, g.win, 0, 0);
  size_t bytes = (size_t)ww * (size_t)wh * 4;
  g.buf = (uint32_t*)malloc(bytes);
  if (!g.buf) { DestroyWindow(g.dpy, g.win); CloseDisplay(g.dpy); g.dpy = 0; return false; }
  for (size_t i = 0; i < (size_t)ww * (size_t)wh; i++) g.buf[i] = 0;
  g.img = CreateImage(g.dpy, DefaultVisual(g.dpy, scr), (unsigned int)DefaultDepth(g.dpy, scr),
                      2 /* ZPixmap */, 0, (char*)g.buf, ww, wh, 32, (int)(ww * 4));
  if (!g.img) {
    free(g.buf); g.buf = 0;
    DestroyWindow(g.dpy, g.win); CloseDisplay(g.dpy); g.dpy = 0;
    return false;
  }
  Flush(g.dpy);
  g.sw = w; g.sh = h;
  g.rw = w; g.rh = h;
  qh = qn = 0;
  g.open = true;
  return true;
}

void blit() {
  if (!g.open) return;
  PutImage(g.dpy, g.win, g.gc, g.img, 0, 0, 0, 0, (unsigned int)(g.sw * g.scale),
           (unsigned int)(g.sh * g.scale));
  Flush(g.dpy);
}

void free_cursors();   // 下で定義（マウスの形）

void close() {
  if (!g.open) return;
  free_cursors();
  g.open = false;
  if (g.img) { XFree_(g.img); g.img = 0; }   // 中身（g.buf）はこちらで解放する
  if (g.buf) { free(g.buf); g.buf = 0; }
  if (g.gc) { FreeGC(g.dpy, g.gc); g.gc = 0; }
  if (g.win) { DestroyWindow(g.dpy, g.win); g.win = 0; }
  if (g.dpy) { CloseDisplay(g.dpy); g.dpy = 0; }
}

// 面の大きさが変わった（窓に合わせて作り直された）。出す先の画素の入れ物も作り直す
bool rebuild(int w, int h) {
  unsigned int ww = (unsigned int)(w * g.scale), wh = (unsigned int)(h * g.scale);
  uint32_t* nb = (uint32_t*)malloc((size_t)ww * (size_t)wh * 4);
  if (!nb) return false;
  for (size_t i = 0; i < (size_t)ww * (size_t)wh; i++) nb[i] = 0;
  int scr = DefaultScreen(g.dpy);
  XImage* ni = CreateImage(g.dpy, DefaultVisual(g.dpy, scr),
                           (unsigned int)DefaultDepth(g.dpy, scr), 2 /* ZPixmap */, 0,
                           (char*)nb, ww, wh, 32, (int)(ww * 4));
  if (!ni) { free(nb); return false; }
  if (g.img) XFree_(g.img);   // 中身（g.buf）はこちらで解放する
  if (g.buf) free(g.buf);
  g.img = ni;
  g.buf = nb;
  g.sw = w;
  g.sh = h;
  return true;
}

void present(const uint32_t* px, int w, int h) {
  if (!g.open) return;
  if ((w != g.sw || h != g.sh) && !rebuild(w, h)) return;
  int s = g.scale;
  int ww = w * s;
  for (int y = 0; y < h; y++) {
    const uint32_t* src = px + (size_t)y * (size_t)w;
    for (int sy = 0; sy < s; sy++) {
      uint32_t* dst = g.buf + (size_t)(y * s + sy) * (size_t)ww;
      for (int x = 0; x < w; x++) {
        uint32_t c = src[x];
        for (int sx = 0; sx < s; sx++) dst[x * s + sx] = c;
      }
    }
  }
  blit();
}

int key_of(XID ks) {
  switch (ks) {
    case 0xff51: return SKEY_Left;
    case 0xff52: return SKEY_Up;
    case 0xff53: return SKEY_Right;
    case 0xff54: return SKEY_Down;
    case 0xff0d: case 0xff8d: return SKEY_Enter;
    case 0xff1b: return SKEY_Escape;
    case 0xff09: return SKEY_Tab;
    case 0xff08: return SKEY_Back;
    case 0xffff: return SKEY_Delete;
    case 0xff50: return SKEY_Home;
    case 0xff57: return SKEY_End;
    case 0xff55: return SKEY_PageUp;
    case 0xff56: return SKEY_PageDown;
    case 0xffe1: case 0xffe2: return SKEY_Shift;
    case 0xffe3: case 0xffe4: return SKEY_Ctrl;
    case 0xffe9: case 0xffea: return SKEY_Alt;
    default: break;
  }
  if (ks >= 0xffbe && ks <= 0xffc9) return SKEY_F1 + (int)(ks - 0xffbe);
  if (ks >= 32 && ks < 127) {
    int c = (int)ks;
    if (c >= 'A' && c <= 'Z') c += 32;
    return c;
  }
  return -1;
}

void mouse_ev(int px, int py, int code, bool down) {
  ScreenEvent e;
  e.kind = SEV_Mouse;
  e.code = code;
  e.down = down;
  int x = px / g.scale, y = py / g.scale;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= g.sw) x = g.sw - 1;
  if (y >= g.sh) y = g.sh - 1;
  e.x = x; e.y = y;
  push(e);
}

void pump() {
  for (int guard = 0; guard < 256 && Pending(g.dpy) > 0; guard++) {
    Event ev;
    NextEvent(g.dpy, &ev);
    switch (ev.type) {
      case KeyPress:
      case KeyRelease: {
        bool down = (ev.type == KeyPress);
        XID ks = LookupKeysym(&ev.xkey, 0);
        int k = key_of(ks);
        if (k >= 0) key_ev(k, down);
        if (down) {   // 打たれた文字（Latin-1 で受け取り、UTF-8 に直す）
          char buf[16];
          XID sym = 0;
          int n = LookupString(&ev.xkey, buf, (int)sizeof buf, &sym, 0);
          if (n > 0 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] != 127) {
            ScreenEvent e;
            e.kind = SEV_Text;
            int at = 0;
            for (int i = 0; i < n && at < 6; i++) {
              unsigned char c = (unsigned char)buf[i];
              if (c < 0x80) { e.text[at++] = (char)c; }
              else { e.text[at++] = (char)(0xc0 | (c >> 6)); e.text[at++] = (char)(0x80 | (c & 0x3f)); }
            }
            e.text[at] = 0;
            if (at > 0) push(e);
          }
        }
        break;
      }
      case ButtonPress:
      case ButtonRelease: {
        bool down = (ev.type == ButtonPress);
        unsigned int b = ev.xbutton.button;
        if (b >= 4 && b <= 7) break;   // 回した合図は渡さない
        int code = b == 1 ? 0 : (b == 2 ? 1 : 2);
        mouse_ev(ev.xbutton.x, ev.xbutton.y, code, down);
        break;
      }
      case MotionNotify:
        mouse_ev(ev.xmotion.x, ev.xmotion.y, -1, false);
        break;
      case Expose:
        blit();
        break;
      case ConfigureNotify: {   // 窓の大きさが変わった（動いただけのときは何もしない）
        int nw = ev.xconfigure.width / g.scale;
        int nh = ev.xconfigure.height / g.scale;
        if (nw > 0 && nh > 0 && (nw != g.rw || nh != g.rh)) {
          g.rw = nw; g.rh = nh;
          ScreenEvent e; e.kind = SEV_Resize; e.x = nw; e.y = nh; push(e);
        }
        break;
      }
      case ClientMessage:
        if (g.wm_delete && (XID)ev.xclient.data.l[0] == g.wm_delete) {
          ScreenEvent e; e.kind = SEV_Close; push(e);
        }
        break;
      default: break;
    }
  }
}

// 縁を引いて大きさを変えられるかを切り替える。窓の管理者（WM）への希望として、
// 変えられなくするときは最小と最大を今の大きさに固定する
void set_resizable(bool on) {
  if (!g.open || !SetWMNormalHints) return;
  SizeHints h = {};
  h.flags = PMinSize | PMaxSize;
  if (on) {
    h.min_width = 1; h.min_height = 1;
    h.max_width = 32767; h.max_height = 32767;
  } else {
    h.min_width = h.max_width = g.sw * g.scale;
    h.min_height = h.max_height = g.sh * g.scale;
  }
  SetWMNormalHints(g.dpy, g.win, &h);
  Flush(g.dpy);
}

// --- マウスの形 -----------------------------------------------------------
// X の字形フォント（cursorfont）の番号。X11 は昔からこの番号で形を持っている
unsigned int cursor_shape(int kind) {
  switch (kind) {
    case SCUR_Hand: return 60;      // XC_hand2
    case SCUR_Text: return 152;     // XC_xterm
    case SCUR_Cross: return 34;     // XC_crosshair
    case SCUR_Wait: return 150;     // XC_watch
    case SCUR_ResizeX: return 108;  // XC_sb_h_double_arrow
    case SCUR_ResizeY: return 116;  // XC_sb_v_double_arrow
    case SCUR_Move: return 52;      // XC_fleur
    default: return 68;             // XC_left_ptr
  }
}

XID cur_ids[SCUR_Max] = {0};   // 作った形は覚えておく（閉じるときにまとめて返す）
int cur_now = -1;

// 何も描いていない形（1×1 の空の絵）。自分でマウスを描くときに使う
XID blank_cursor() {
  if (!CreateBitmapFromData || !CreatePixmapCursor) return 0;
  static const char zero[1] = {0};
  XID pix = CreateBitmapFromData(g.dpy, g.win, zero, 1, 1);
  if (!pix) return 0;
  XColor c;
  c.pixel = 0; c.red = 0; c.green = 0; c.blue = 0; c.flags = 0; c.pad = 0;
  XID cur = CreatePixmapCursor(g.dpy, pix, pix, &c, &c, 0, 0);
  if (FreePixmap) FreePixmap(g.dpy, pix);
  return cur;
}

void set_cursor(int kind) {
  if (!g.open || !DefineCursor) return;
  if (kind < 0 || kind >= SCUR_Max) kind = SCUR_Arrow;
  if (kind == cur_now) return;
  if (!cur_ids[kind]) {
    cur_ids[kind] = (kind == SCUR_None) ? blank_cursor()
                                        : (CreateFontCursor ? CreateFontCursor(g.dpy, cursor_shape(kind)) : 0);
  }
  if (!cur_ids[kind] && kind != SCUR_Arrow) return;   // 作れない形は、そのまま
  DefineCursor(g.dpy, g.win, cur_ids[kind]);          // 0 なら窓の既定に戻る
  if (Flush) Flush(g.dpy);
  cur_now = kind;
}

void free_cursors() {
  for (int i = 0; i < SCUR_Max; i++) {
    if (cur_ids[i] && FreeCursor) FreeCursor(g.dpy, cur_ids[i]);
    cur_ids[i] = 0;
  }
  cur_now = -1;
}

bool poll(ScreenEvent* out) {
  if (!g.open) return false;
  if (qn == 0) pump();
  if (qn == 0) return false;
  *out = q[qh];
  qh = (qh + 1) % 128;
  qn--;
  return true;
}

}  // namespace x11

// X11 は「離した」も出せる
// 変換つきの文字入力（XIM）はまだ持たない。打たれた文字がそのまま入る
const PlatformScreen kX11Screen = {x11::screen_scale, x11::open, x11::close,
                                  x11::present,      x11::poll, true,
                                  0, 0, 0, 0, 0, 0, 0,   // 文字入力と切り貼りはまだ持たない
                                  0,                     // set_redraw（X11 は止まらないので要らない）
                                  x11::set_cursor,   x11::set_resizable};

const PlatformScreen* x11_screen() { return x11::load() ? &kX11Screen : 0; }

#endif

// screen_mac.inc — macOS の窓（desktop.cpp から読む）
//
// 窓の道具（GUI ツールキット）には頼らない（spec/runtime/platform.md）。
// macOS の窓は OS そのものが持っているので、**実行時に**取りに行く。
//
//   ・libobjc と AppKit / CoreGraphics を dlopen で開く。作るときには何も要らない
//   ・呼び出しは objc_msgSend を、そのつど正しい形に見立てて呼ぶ
//   ・画素は CGImage にして、窓の層（CALayer）に貼る。拡大は補間しない
//
// 開けなければ false を返すだけ。そのとき desktop.cpp は端末に落とす。
#if !defined(__APPLE__)

const PlatformScreen* mac_screen() { return 0; }

#else

#include <dlfcn.h>

namespace mac {

// --- Objective-C の呼び出し ------------------------------------------------
typedef void* Obj;   // id
typedef void* Sel;   // SEL

Obj (*getClass)(const char*) = 0;
Sel (*selRegister)(const char*) = 0;
void* msgSend = 0;   // objc_msgSend。形はそのつど見立てる
void* msgSendStret = 0;   // objc_msgSend_stret（x86_64 で構造体を返す呼び出しに使う）

struct Pt { double x, y; };
struct Sz { double w, h; };
struct Rect { Pt origin; Sz size; };
struct Range { unsigned long loc, len; };   // NSRange。単位は UTF-16

Obj C(const char* n) { return getClass(n); }
Sel S(const char* n) { return selRegister(n); }

Obj send(Obj o, const char* s) { return ((Obj(*)(Obj, Sel))msgSend)(o, S(s)); }
void sendv(Obj o, const char* s) { ((void (*)(Obj, Sel))msgSend)(o, S(s)); }
void sendv1(Obj o, const char* s, Obj a) { ((void (*)(Obj, Sel, Obj))msgSend)(o, S(s), a); }
// BOOL は arm64 では bool、x86_64 では signed char。どちらでも 1 バイトで渡る
void sendb(Obj o, const char* s, bool b) {
  ((void (*)(Obj, Sel, signed char))msgSend)(o, S(s), (signed char)(b ? 1 : 0));
}
long sendl(Obj o, const char* s) { return ((long (*)(Obj, Sel))msgSend)(o, S(s)); }
bool sendbool(Obj o, const char* s) { return ((signed char (*)(Obj, Sel))msgSend)(o, S(s)) != 0; }

Obj nsstr(const char* s) {
  Obj a = send(C("NSString"), "alloc");
  return ((Obj(*)(Obj, Sel, const char*))msgSend)(a, S("initWithUTF8String:"), s);
}
const char* utf8(Obj nsstring) {
  if (!nsstring) return 0;
  return ((const char* (*)(Obj, Sel))msgSend)(nsstring, S("UTF8String"));
}

// --- CoreGraphics ---------------------------------------------------------
void* (*cgColorSpaceRGB)() = 0;
void (*cgColorSpaceRelease)(void*) = 0;
void* (*cgProviderWithData)(void*, const void*, size_t, void (*)(void*, const void*, size_t)) = 0;
void (*cgProviderRelease)(void*) = 0;
void* (*cgImageCreate)(size_t, size_t, size_t, size_t, size_t, void*, uint32_t, void*,
                       const double*, signed char, int) = 0;
void (*cgImageRelease)(void*) = 0;
// 画面のある席にいるか（ssh の先では 0 が返る）。無くてもよい
void* (*cgSession)() = 0;
void (*cfRelease)(void*) = 0;
// 窓の縁を引いている間の出し直しに使うタイマー（CoreFoundation）。無くてもよい
void* (*cfTimerCreate)(void*, double, double, unsigned long, long,
                       void (*)(void*, void*), void*) = 0;
void (*cfTimerInvalidate)(void*) = 0;
void (*cfRunLoopAddTimer)(void*, void*, void*) = 0;
void* (*cfRunLoopGetCurrent)() = 0;
double (*cfAbsNow)() = 0;
void* (*cfStrCreate)(void*, const char*, unsigned int) = 0;
void* cfCommonModes = 0;    // kCFRunLoopCommonModes
void* cfTrackingMode = 0;   // NSEventTrackingRunLoopMode（引いている間のモード）

void free_copy(void* info, const void* data, size_t size) {
  (void)info; (void)size;
  free((void*)data);
}

// --- 持ちもの -------------------------------------------------------------
struct State {
  bool open;
  Obj app, win, layer;
  Obj distant_past, run_mode;
  int sw, sh;        // 面の大きさ（画素）
  int rw, rh;        // 窓から最後に知らせた面の大きさ（SEV_Resize を重ねて出さないため）
  int scale;         // 面の1画素を、画面の何画素にするか
  int dpi;           // 画面の細かさ（HiDPI なら 2）
  unsigned long mods;  // 前回の修飾キー
  uint32_t* last;    // 最後に出した絵。縁を引いている間、引き伸ばさずに出し直すのに使う
  int lw, lh;        // その大きさ
  int pw, ph;        // いま層（CALayer）に載っている絵の大きさ
  void* timer;       // 引いている間も動く刻み（CFRunLoopTimer）
  bool (*redraw)(int, int);   // コアに描き直しを頼む口（set_redraw で渡される）
  State() : open(false), app(0), win(0), layer(0), distant_past(0), run_mode(0),
            sw(0), sh(0), rw(0), rh(0), scale(1), dpi(1), mods(0),
            last(0), lw(0), lh(0), pw(0), ph(0), timer(0), redraw(0) {}
};
State g;

// 文字入力（IME）の受け皿。中身は下の text_input で作る
Obj g_sink = 0;
bool g_text_on = false;

// 出来事の待ち行列（端末側と同じ形）
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

bool load() {
  static int tried = 0;
  if (tried) return tried > 0;
  tried = -1;
  void* objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_LOCAL);
  void* appkit = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_LOCAL);
  void* cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
                    RTLD_LAZY | RTLD_LOCAL);
  if (!objc || !appkit || !cg) return false;
  getClass = (Obj(*)(const char*))dlsym(objc, "objc_getClass");
  selRegister = (Sel(*)(const char*))dlsym(objc, "sel_registerName");
  msgSend = dlsym(objc, "objc_msgSend");
  msgSendStret = dlsym(objc, "objc_msgSend_stret");   // arm64 には無い。無くてもよい
  cgColorSpaceRGB = (void* (*)())dlsym(cg, "CGColorSpaceCreateDeviceRGB");
  cgColorSpaceRelease = (void (*)(void*))dlsym(cg, "CGColorSpaceRelease");
  cgProviderWithData = (void* (*)(void*, const void*, size_t,
                                  void (*)(void*, const void*, size_t)))
      dlsym(cg, "CGDataProviderCreateWithData");
  cgProviderRelease = (void (*)(void*))dlsym(cg, "CGDataProviderRelease");
  cgImageCreate = (void* (*)(size_t, size_t, size_t, size_t, size_t, void*, uint32_t, void*,
                             const double*, signed char, int))dlsym(cg, "CGImageCreate");
  cgImageRelease = (void (*)(void*))dlsym(cg, "CGImageRelease");
  cgSession = (void* (*)())dlsym(cg, "CGSessionCopyCurrentDictionary");
  void* cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                    RTLD_LAZY | RTLD_LOCAL);
  if (cf) {
    cfRelease = (void (*)(void*))dlsym(cf, "CFRelease");
    cfTimerCreate = (void* (*)(void*, double, double, unsigned long, long,
                               void (*)(void*, void*), void*))dlsym(cf, "CFRunLoopTimerCreate");
    cfTimerInvalidate = (void (*)(void*))dlsym(cf, "CFRunLoopTimerInvalidate");
    cfRunLoopAddTimer = (void (*)(void*, void*, void*))dlsym(cf, "CFRunLoopAddTimer");
    cfRunLoopGetCurrent = (void* (*)())dlsym(cf, "CFRunLoopGetCurrent");
    cfAbsNow = (double (*)())dlsym(cf, "CFAbsoluteTimeGetCurrent");
    cfStrCreate = (void* (*)(void*, const char*, unsigned int))
        dlsym(cf, "CFStringCreateWithCString");
    void** modes = (void**)dlsym(cf, "kCFRunLoopCommonModes");
    if (modes) cfCommonModes = *modes;
    // 引いている間は、走り方（モード）が変わる。そのモードにも掛かるようにしておく
    if (cfStrCreate)
      cfTrackingMode = cfStrCreate(0, "NSEventTrackingRunLoopMode", 0x08000100 /* UTF-8 */);
  }
  if (!getClass || !selRegister || !msgSend || !cgColorSpaceRGB || !cgProviderWithData ||
      !cgImageCreate || !cgImageRelease || !cgProviderRelease || !cgColorSpaceRelease)
    return false;
  tried = 1;
  return true;
}

// 画面の細かさ。HiDPI（Retina）なら 2。窓を開く前でも答えられる
int screen_scale() {
  if (!load()) return 1;
  Obj scr = send(C("NSScreen"), "mainScreen");
  if (!scr) return 1;
  double f = ((double (*)(Obj, Sel))msgSend)(scr, S("backingScaleFactor"));
  int n = (int)(f + 0.5);
  return n < 1 ? 1 : (n > 4 ? 4 : n);
}

// 面が小さいときだけ、整数倍に広げる（拡大は補間しない）。
// 大きい面はそのまま**画面の画素と1対1**にする。HiDPI ではこれで細かく出る
int pick_scale(int w, int h) {
  int s = 1;
  while (s < 6 && w * (s + 1) <= 640 && h * (s + 1) <= 480) s++;
  return s;
}

void resize_tick(void*, void*);   // 下で定義。窓の縁を引いている間の出し直し

bool open(const char* title, int w, int h) {
  if (!load()) return false;
  if (g.open) return false;
  // 画面のある席にいるか先に確かめる。ssh の先などで窓を作りにいくと、
  // AppKit が「窓の係につながらない」と言って落ちることがある
  if (cgSession) {
    void* d = cgSession();
    if (!d) return false;
    if (cfRelease) cfRelease(d);
  }

  g.app = send(C("NSApplication"), "sharedApplication");
  if (!g.app) return false;
  ((void (*)(Obj, Sel, long))msgSend)(g.app, S("setActivationPolicy:"), 0);  // Regular

  g.scale = pick_scale(w, h);
  g.dpi = screen_scale();
  Rect r;
  r.origin.x = 0; r.origin.y = 0;
  // 窓の大きさは**点**で決める。画素の数を細かさで割ると、
  // 面の1画素が画面の1画素にそのまま乗る（HiDPI ならその細かさで出る）
  r.size.w = (double)(w * g.scale) / (double)g.dpi;
  r.size.h = (double)(h * g.scale) / (double)g.dpi;
  // 縁を引いて大きさを変えられる窓。変わったら SEV_Resize で知らせ、
  // コアが面を同じ大きさに作り直す（core/lib/ui.cpp）
  unsigned long style = 1 | 2 | 4 | 8;   // titled | closable | miniaturizable | resizable
  Obj win = send(C("NSWindow"), "alloc");
  win = ((Obj(*)(Obj, Sel, Rect, unsigned long, unsigned long, signed char))msgSend)(
      win, S("initWithContentRect:styleMask:backing:defer:"), r, style, 2, 0);
  if (!win) return false;
  g.win = win;

  Obj t = nsstr(title && title[0] ? title : "Shark");
  sendv1(win, "setTitle:", t);
  sendb(win, "setAcceptsMouseMovedEvents:", true);
  sendv(win, "center");

  Obj view = send(win, "contentView");
  sendb(view, "setWantsLayer:", true);
  g.layer = send(view, "layer");
  if (g.layer) {
    sendv1(g.layer, "setMagnificationFilter:", nsstr("nearest"));
    sendv1(g.layer, "setContentsGravity:", nsstr("resize"));
    // 層の裏側も画面の細かさに合わせる。これをしないと、せっかくの
    // 細かい面が半分の細かさに落ちてしまう
    ((void (*)(Obj, Sel, double))msgSend)(g.layer, S("setContentsScale:"), (double)g.dpi);
  }

  sendv1(win, "makeKeyAndOrderFront:", 0);
  sendb(g.app, "activateIgnoringOtherApps:", true);
  sendv(g.app, "finishLaunching");

  g.distant_past = send(C("NSDate"), "distantPast");
  g.run_mode = nsstr("kCFRunLoopDefaultMode");
  g.sw = w; g.sh = h;
  g.rw = w; g.rh = h;
  g.lw = g.lh = 0;
  g.pw = g.ph = 0;
  g.mods = 0;
  qh = qn = 0;
  // 窓の縁を引いている間、AppKit は sendEvent: の中の追跡ループから戻らず、
  // Shark の側は描き直せない。そこで細かい刻みを掛けておき、引いている間は
  // 最後の絵を**引き伸ばさずに**置き直す（下の resize_tick）。
  // ふつうの時は大きさが合っているので、何もしないで返る
  if (cfTimerCreate && cfRunLoopAddTimer && cfRunLoopGetCurrent && cfAbsNow) {
    g.timer = cfTimerCreate(0, cfAbsNow() + 0.03, 0.03, 0, 0, resize_tick, 0);
    if (g.timer) {
      if (cfCommonModes) cfRunLoopAddTimer(cfRunLoopGetCurrent(), g.timer, cfCommonModes);
      if (cfTrackingMode) cfRunLoopAddTimer(cfRunLoopGetCurrent(), g.timer, cfTrackingMode);
    }
  }
  g.open = true;
  return true;
}

void text_input(bool on, const char* initial, int x, int y, int h);   // 下で定義

void set_cursor(int kind);   // 下で定義

void close() {
  if (!g.open) return;
  text_input(false, 0, 0, 0, 0);
  set_cursor(SCUR_Arrow);   // 隠したままにしない
  g.open = false;
  if (g.timer) {
    if (cfTimerInvalidate) cfTimerInvalidate(g.timer);
    if (cfRelease) cfRelease(g.timer);
    g.timer = 0;
  }
  g.redraw = 0;
  free(g.last);
  g.last = 0;
  g.lw = g.lh = 0;
  g.pw = g.ph = 0;
  sendv1(g.win, "orderOut:", 0);
  sendv(g.win, "close");
  g.win = 0;
  g.layer = 0;
}

// 画素の並びを層（CALayer）に貼る。copy は malloc で作ったもので、層が使い終わると解放される
void put_on_layer(void* copy, int w, int h) {
  void* cs = cgColorSpaceRGB();
  void* dp = cgProviderWithData(0, copy, (size_t)w * (size_t)h * 4, free_copy);
  // 0x00RRGGBB の並びを、そのまま「先頭を捨てる ARGB（下位から並ぶ）」として読ませる
  const uint32_t kSkipFirst = 6, kLittle = 2u << 12;
  void* img = cgImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                            kSkipFirst | kLittle, dp, 0, 0, 0);
  if (img) {
    sendv1(g.layer, "setContents:", img);
    cgImageRelease(img);
  }
  cgProviderRelease(dp);
  cgColorSpaceRelease(cs);
}

void present(const uint32_t* px, int w, int h) {
  if (!g.open || !g.layer) return;
  g.sw = w; g.sh = h;
  size_t bytes = (size_t)w * (size_t)h * 4;
  // 出した絵を覚えておく（窓の縁を引いている間、引き伸ばさずに出し直すため）
  if (!g.last || g.lw != w || g.lh != h) {
    free(g.last);
    g.last = (uint32_t*)malloc(bytes);
    g.lw = g.lh = 0;
  }
  if (g.last) { sk_memcpy(g.last, px, bytes); g.lw = w; g.lh = h; }
  void* copy = malloc(bytes);   // 層が後で読むので、そのときの姿を写して渡す
  if (!copy) return;
  sk_memcpy(copy, px, bytes);
  put_on_layer(copy, w, h);
  g.pw = w;
  g.ph = h;
}

// 文字から、こちらのキーの番号にする
int key_of(int cp) {
  switch (cp) {
    case 0xF700: return SKEY_Up;
    case 0xF701: return SKEY_Down;
    case 0xF702: return SKEY_Left;
    case 0xF703: return SKEY_Right;
    case 0xF728: return SKEY_Delete;
    case 0xF729: return SKEY_Home;
    case 0xF72B: return SKEY_End;
    case 0xF72C: return SKEY_PageUp;
    case 0xF72D: return SKEY_PageDown;
    case 13: case 3: return SKEY_Enter;
    case 27: return SKEY_Escape;
    case 9: case 25: return SKEY_Tab;
    case 127: case 8: return SKEY_Back;
    default: break;
  }
  if (cp >= 0xF704 && cp <= 0xF70F) return SKEY_F1 + (cp - 0xF704);
  if (cp >= 32 && cp < 127) {
    if (cp >= 'A' && cp <= 'Z') cp += 32;
    return cp;
  }
  return -1;
}

// UTF-8 の先頭1文字を取り出す
int first_cp(const char* s) {
  if (!s || !s[0]) return -1;
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) return c;
  int len = c >= 0xf0 ? 4 : (c >= 0xe0 ? 3 : 2);
  int cp = c & (0xff >> (len + 1));
  for (int i = 1; i < len; i++) {
    if (!s[i]) return -1;
    cp = (cp << 6) | ((unsigned char)s[i] & 0x3f);
  }
  return cp;
}

void apply_cursor();   // 下で定義

void mouse_ev(Obj ev, int code, bool down) {
  apply_cursor();   // 窓の上で動くと AppKit が矢印に戻すので、そのつど掛け直す
  Pt p = ((Pt(*)(Obj, Sel))msgSend)(ev, S("locationInWindow"));
  ScreenEvent e;
  e.kind = SEV_Mouse;
  e.code = code;
  e.down = down;
  // 窓の座標は左下から、単位は点。面は左上・画素なので、裏返してから直す
  double px = p.x * g.dpi;
  double py = (double)(g.sh * g.scale) - p.y * g.dpi;
  int x = (int)(px / g.scale);
  int y = (int)(py / g.scale);
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= g.sw) x = g.sw - 1;
  if (y >= g.sh) y = g.sh - 1;
  e.x = x; e.y = y;
  push(e);
}

// 修飾キーは「変わった」としか来ないので、前回と比べて押し・離しにする
void flags_ev(unsigned long now) {
  struct { unsigned long bit; int key; } m[] = {
      {1ul << 17, SKEY_Shift}, {1ul << 18, SKEY_Ctrl}, {1ul << 19, SKEY_Alt}};
  for (int i = 0; i < 3; i++) {
    bool was = (g.mods & m[i].bit) != 0;
    bool is = (now & m[i].bit) != 0;
    if (was != is) key_ev(m[i].key, is);
  }
  g.mods = now;
}

// 窓の中身（contentView）の大きさ（点）。構造体を返すので、x86_64 は別の入り口を使う
Sz view_size(Obj view) {
  Sz z; z.w = 0; z.h = 0;
  if (!view) return z;
#if defined(__x86_64__)
  if (!msgSendStret) return z;
  Rect r = ((Rect(*)(Obj, Sel))msgSendStret)(view, S("frame"));
#else
  Rect r = ((Rect(*)(Obj, Sel))msgSend)(view, S("frame"));
#endif
  return r.size;
}

// 窓の縁を引いている間に呼ばれる刻み（open で掛けたタイマー）。
// 引いている間は sendEvent: から戻れず、Shark の側は描き直せない。層の伸び縮み
// （contentsGravity）に任せると絵が引き伸ばされるので、ここで**等倍のまま**出し直す。
// 最後の絵を左上に置き、余白は右下の色で埋める。本当の描き直しは、引き終わって
// ui.poll() に戻ったときに SEV_Resize で起きる
void resize_tick(void*, void*) {
  if (!g.open || !g.layer || !g.win) return;
  Sz sz = view_size(send(g.win, "contentView"));
  int nw = (int)(sz.w * (double)g.dpi / (double)g.scale + 0.5);
  int nh = (int)(sz.h * (double)g.dpi / (double)g.scale + 0.5);
  if (nw <= 0 || nh <= 0) return;
  if (nw != g.rw || nh != g.rh) {   // 新しい大きさを知らせる（pump と同じ形）
    g.rw = nw; g.rh = nh;
    // まだ受け取られていない前の知らせがあれば、差し替える（引いている間にたまらないように）
    if (qn > 0 && q[(qh + qn - 1) % 128].kind == SEV_Resize) qn--;
    ScreenEvent e; e.kind = SEV_Resize; e.x = nw; e.y = nh; push(e);
  }
  if (nw == g.pw && nh == g.ph) return;   // もう合っている（ふつうの時はここで返る）
  // コアが部品を覚えていれば、新しい大きさで**組み直して**もらう（毎度その解像度で描き直す）
  if (g.redraw && g.redraw(nw, nh)) return;   // present まで済み、g.pw も新しくなっている
  // 覚えていない（自分で描くだけの）ときは、最後の絵を等倍のまま置き直す
  if (!g.last || g.lw < 1 || g.lh < 1) return;
  uint32_t* buf = (uint32_t*)malloc((size_t)nw * (size_t)nh * 4);
  if (!buf) return;
  uint32_t fill = g.last[(size_t)(g.lh - 1) * (size_t)g.lw + (size_t)(g.lw - 1)];
  for (int y = 0; y < nh; y++) {
    uint32_t* dst = buf + (size_t)y * (size_t)nw;
    int cw = 0;
    if (y < g.lh) {
      cw = g.lw < nw ? g.lw : nw;
      sk_memcpy(dst, g.last + (size_t)y * (size_t)g.lw, (size_t)cw * 4);
    }
    for (int x = cw; x < nw; x++) dst[x] = fill;
  }
  put_on_layer(buf, nw, nh);
  g.pw = nw;
  g.ph = nh;
}

void pump() {
  Obj pool = send(send(C("NSAutoreleasePool"), "alloc"), "init");
  for (int guard = 0; guard < 256; guard++) {
    Obj ev = ((Obj(*)(Obj, Sel, unsigned long long, Obj, Obj, signed char))msgSend)(
        g.app, S("nextEventMatchingMask:untilDate:inMode:dequeue:"),
        ~0ull, g.distant_past, g.run_mode, 1);
    if (!ev) break;
    long type = sendl(ev, "type");
    switch (type) {
      case 10: case 11: {   // KeyDown / KeyUp
        bool down = (type == 10);
        unsigned long mods = (unsigned long)sendl(ev, "modifierFlags");
        int cp = first_cp(utf8(send(ev, "charactersIgnoringModifiers")));
        int k = key_of(cp);
        // 変換の最中は、そのキーは変換のもの。アプリには渡さない。
        // enter は確定、esc は取り消し、空白と矢印は候補選びに使われる。
        // ここで渡すと、確定した拍子に入力欄から出てしまう
        bool composing = g_text_on && g_sink && sendbool(g_sink, "hasMarkedText");
        if (k >= 0 && !composing) key_ev(k, down);
        if (down && (mods & (1ul << 20)) && (cp == 'q' || cp == 'Q')) {   // Cmd-Q
          ScreenEvent e; e.kind = SEV_Close; push(e);
        }
        // Cmd の切り貼り。menu bar を持たないので、こちらから受け皿に頼む
        if (down && g_text_on && g_sink && (mods & (1ul << 20))) {
          const char* act = 0;
          if (cp == 'c') act = "copy:";
          else if (cp == 'v') act = "paste:";
          else if (cp == 'x') act = "cut:";
          else if (cp == 'a') act = "selectAll:";
          else if (cp == 'z') act = "undo:";
          if (act) {
            ((void (*)(Obj, Sel, Obj))msgSend)(g_sink, S(act), 0);
            break;
          }
        }
        // 打たれた文字。修飾キーつきのものと、機能キーは渡さない。
        // 文字入力（IME）の最中は、受け皿が受け取るのでこちらからは渡さない
        if (down && !g_text_on && !(mods & ((1ul << 18) | (1ul << 20)))) {
          const char* s = utf8(send(ev, "characters"));
          int c0 = first_cp(s);
          if (s && c0 >= 32 && c0 != 127 && (c0 < 0xF700 || c0 > 0xF8FF)) {
            ScreenEvent e;
            e.kind = SEV_Text;
            int i = 0;
            for (; i < 7 && s[i]; i++) e.text[i] = s[i];
            e.text[i] = 0;
            push(e);
          }
        }
        break;
      }
      case 12:   // FlagsChanged
        flags_ev((unsigned long)sendl(ev, "modifierFlags"));
        break;
      case 1: mouse_ev(ev, 0, true); break;    // LeftMouseDown
      case 2: mouse_ev(ev, 0, false); break;   // LeftMouseUp
      case 3: mouse_ev(ev, 2, true); break;    // RightMouseDown
      case 4: mouse_ev(ev, 2, false); break;   // RightMouseUp
      case 25: mouse_ev(ev, 1, true); break;   // OtherMouseDown
      case 26: mouse_ev(ev, 1, false); break;  // OtherMouseUp
      case 5: case 6: case 7: case 27:         // MouseMoved / Dragged
        mouse_ev(ev, -1, false);
        break;
      default: break;
    }
    sendv1(g.app, "sendEvent:", ev);   // 窓の移動や閉じるボタンは OS に任せる
  }
  // 窓の縁を引かれて大きさが変わっていたら、新しい面の大きさを知らせる
  if (g.win) {
    Sz sz = view_size(send(g.win, "contentView"));
    int nw = (int)(sz.w * (double)g.dpi / (double)g.scale + 0.5);
    int nh = (int)(sz.h * (double)g.dpi / (double)g.scale + 0.5);
    if (nw > 0 && nh > 0 && (nw != g.rw || nh != g.rh)) {
      g.rw = nw; g.rh = nh;
      ScreenEvent e; e.kind = SEV_Resize; e.x = nw; e.y = nh; push(e);
    }
  }
  // 閉じるボタンで消えたら、閉じてくれと言われたことにする
  if (g.win && !sendbool(g.win, "isVisible")) {
    ScreenEvent e; e.kind = SEV_Close; push(e);
  }
  sendv(pool, "release");
}


// --- 文字入力（IME）------------------------------------------------------
//
// 変換そのものは OS に任せる。見えない NSTextView を1つ置いて、そこに
// キーを受けさせ、**確定した文字列と変換中の文字列だけ**を取り出す。
// 候補一覧は OS がその場所に出す（なので置き場所を合わせる）。
void text_input(bool on, const char* initial, int x, int y, int h) {
  if (!g.open) return;
  if (!on) {
    if (g_text_on) {
      ((signed char (*)(Obj, Sel, Obj))msgSend)(g.win, S("makeFirstResponder:"), 0);
      if (g_sink) sendv(g_sink, "removeFromSuperview");
      g_text_on = false;
    }
    return;
  }
  if (!g_sink) {
    Rect r;
    r.origin.x = 0; r.origin.y = 0; r.size.w = 1; r.size.h = 1;
    Obj tv = send(C("NSTextView"), "alloc");
    if (!tv) return;
    g_sink = ((Obj(*)(Obj, Sel, Rect))msgSend)(tv, S("initWithFrame:"), r);
    if (!g_sink) return;
    sendb(g_sink, "setDrawsBackground:", false);
    sendb(g_sink, "setRichText:", false);
    // 受け皿は**見えないもの**。ここを透けさせておかないと、OS の入力欄が持っている
    // カーソルまで出てきて、こちらが描くカーソルと2本に見える。
    // 透かすのは見た目だけなので、キーも変換も今までどおり受け取れる
    ((void (*)(Obj, Sel, double))msgSend)(g_sink, S("setAlphaValue:"), 0.0);
    // 新しい macOS はカーソルを別の層に描く。そちらは透明の色で消す
    Obj clear = send(C("NSColor"), "clearColor");
    if (clear) sendv1(g_sink, "setInsertionPointColor:", clear);
  }
  // 面の画素 → 窓の点。窓の縦は下から数えるので裏返す
  double k = (double)g.scale / (double)g.dpi;
  Rect fr;
  fr.origin.x = (double)x * k;
  fr.origin.y = (double)(g.sh * g.scale) / (double)g.dpi - (double)(y + h) * k;
  fr.size.w = 1;
  fr.size.h = (double)h * k;
  ((void (*)(Obj, Sel, Rect))msgSend)(g_sink, S("setFrame:"), fr);
  if (!g_text_on) {
    // 先に置いて、焦点を移してから中身を入れる。
    // 逆にすると、焦点が移るときに中身が捨てられることがある
    Obj view = send(g.win, "contentView");
    ((void (*)(Obj, Sel, Obj))msgSend)(view, S("addSubview:"), g_sink);
    ((signed char (*)(Obj, Sel, Obj))msgSend)(g.win, S("makeFirstResponder:"), g_sink);
    g_text_on = true;
  }
  if (initial) sendv1(g_sink, "setString:", nsstr(initial));
}

// 改行は入れない（1行の入力欄なので）
void append_clean(Str* out, const char* s) {
  if (!s) return;
  for (int i = 0; s[i]; i++)
    if (s[i] != '\n' && s[i] != '\r') out->push(s[i]);
}

// UTF-16 の位置（NSString の数え方）と、文字の数（こちらの数え方）を行き来する。
// 移植層は値の作りに触らないので、数えるところもここに持つ
int u8_chars(const char* s) {
  int n = 0;
  for (int i = 0; s && s[i]; i++)
    if (((unsigned char)s[i] & 0xc0) != 0x80) n++;
  return n;
}
int chars_before(Obj str, unsigned long at16) {
  Obj before = ((Obj(*)(Obj, Sel, unsigned long))msgSend)(str, S("substringToIndex:"), at16);
  return u8_chars(utf8(before));
}
unsigned long utf16_at(Obj str, int chars) {
  const char* s = utf8(str);
  if (!s) return 0;
  unsigned long n = 0;
  int seen = 0, i = 0;
  while (s[i] && seen < chars) {
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c >= 0xf0 ? 4 : (c >= 0xe0 ? 3 : 2));
    n += (len == 4) ? 2 : 1;   // 面の外の字は UTF-16 で2つぶん
    i += len;
    seen++;
  }
  return n;
}

bool text_selection(int* start, int* len) {
  if (!g_text_on || !g_sink) return false;
  Obj str = send(g_sink, "string");
  if (!str) return false;
  Range r = ((Range(*)(Obj, Sel))msgSend)(g_sink, S("selectedRange"));
  int a = chars_before(str, r.loc);
  int b = chars_before(str, r.loc + r.len);
  *start = a;
  *len = b - a;
  return true;
}

void text_select(int start, int len) {
  if (!g_text_on || !g_sink) return;
  Obj str = send(g_sink, "string");
  if (!str) return;
  if (start < 0) start = 0;
  if (len < 0) len = 0;
  Range r;
  r.loc = utf16_at(str, start);
  r.len = utf16_at(str, start + len) - r.loc;
  ((void (*)(Obj, Sel, Range))msgSend)(g_sink, S("setSelectedRange:"), r);
}

// 選んでいるところを置き換える。変換中は触らない
void text_replace(const char* s) {
  if (!g_text_on || !g_sink || !s) return;
  if (sendbool(g_sink, "hasMarkedText")) return;
  Range r = ((Range(*)(Obj, Sel))msgSend)(g_sink, S("selectedRange"));
  ((void (*)(Obj, Sel, Obj, Range))msgSend)(g_sink, S("insertText:replacementRange:"), nsstr(s), r);
}

// --- 切り貼りの置き場 -----------------------------------------------------
Obj pb_type() { return nsstr("public.utf8-plain-text"); }

bool clipboard_get(Str* out) {
  if (!load()) return false;
  Obj pb = send(C("NSPasteboard"), "generalPasteboard");
  if (!pb) return false;
  Obj s = ((Obj(*)(Obj, Sel, Obj))msgSend)(pb, S("stringForType:"), pb_type());
  if (!s) return false;
  const char* c = utf8(s);
  if (!c) return false;
  *out = Str(c);
  return true;
}

void clipboard_set(const char* s) {
  if (!load() || !s) return;
  Obj pb = send(C("NSPasteboard"), "generalPasteboard");
  if (!pb) return;
  ((long (*)(Obj, Sel))msgSend)(pb, S("clearContents"));
  ((signed char (*)(Obj, Sel, Obj, Obj))msgSend)(pb, S("setString:forType:"), nsstr(s), pb_type());
}

bool text_state(Str* confirmed, Str* marked) {
  if (!g_text_on || !g_sink) return false;
  Obj str = send(g_sink, "string");
  if (!str) return false;
  confirmed->clear();
  marked->clear();
  if (sendbool(g_sink, "hasMarkedText")) {
    // 変換中のところだけ切り分ける。位置は UTF-16 の数なので、
    // 自分で数えずに、NSString に切ってもらう
    Range r = ((Range(*)(Obj, Sel))msgSend)(g_sink, S("markedRange"));
    Obj before = ((Obj(*)(Obj, Sel, unsigned long))msgSend)(str, S("substringToIndex:"), r.loc);
    Obj mid = ((Obj(*)(Obj, Sel, Range))msgSend)(str, S("substringWithRange:"), r);
    Obj after = ((Obj(*)(Obj, Sel, unsigned long))msgSend)(str, S("substringFromIndex:"),
                                                          r.loc + r.len);
    append_clean(confirmed, utf8(before));
    append_clean(marked, utf8(mid));
    append_clean(confirmed, utf8(after));
  } else {
    append_clean(confirmed, utf8(str));
  }
  return true;
}

// --- マウスの形 -----------------------------------------------------------
// AppKit は窓の上で動くたびに矢印へ戻すので、**覚えておいて掛け直す**
// （mouse_ev から apply_cursor を呼ぶ）。none は隠す・出すの数を合わせる
int g_cursor = SCUR_Arrow;
bool g_cursor_hidden = false;

// AppKit が持っている形。待ちの形（砂時計）は公けには無いので、矢印のままにする
// （待たせている間の輪は OS が勝手に出す）
const char* cursor_sel(int kind) {
  switch (kind) {
    case SCUR_Hand: return "pointingHandCursor";
    case SCUR_Text: return "IBeamCursor";
    case SCUR_Cross: return "crosshairCursor";
    case SCUR_ResizeX: return "resizeLeftRightCursor";
    case SCUR_ResizeY: return "resizeUpDownCursor";
    case SCUR_Move: return "openHandCursor";
    default: return "arrowCursor";
  }
}

// 持っていない呼び名を送ると落ちるので、先に尋ねる（機種ごとに増減する）
bool responds(Obj o, const char* sel) {
  return ((signed char (*)(Obj, Sel, Sel))msgSend)(o, S("respondsToSelector:"), S(sel)) != 0;
}

void apply_cursor() {
  if (!load()) return;
  Obj cls = C("NSCursor");
  if (!cls) return;
  if (g_cursor == SCUR_None) {
    if (!g_cursor_hidden && responds(cls, "hide")) { sendv(cls, "hide"); g_cursor_hidden = true; }
    return;
  }
  if (g_cursor_hidden) {
    if (responds(cls, "unhide")) sendv(cls, "unhide");
    g_cursor_hidden = false;
  }
  const char* sel = cursor_sel(g_cursor);
  if (!responds(cls, sel)) sel = "arrowCursor";
  if (!responds(cls, sel)) return;
  Obj cur = send(cls, sel);
  if (cur && responds(cur, "set")) sendv(cur, "set");
}

void set_cursor(int kind) {
  g_cursor = kind;
  apply_cursor();
}

void set_redraw(bool (*fn)(int, int)) { g.redraw = fn; }

// 縁を引いて大きさを変えられるかを切り替える（8 = NSWindowStyleMaskResizable）
void set_resizable(bool on) {
  if (!g.open || !g.win) return;
  unsigned long style = ((unsigned long (*)(Obj, Sel))msgSend)(g.win, S("styleMask"));
  if (on) style |= 8; else style &= ~(unsigned long)8;
  ((void (*)(Obj, Sel, unsigned long))msgSend)(g.win, S("setStyleMask:"), style);
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

}  // namespace mac

// 窓は「離した」も出せる
const PlatformScreen kMacScreen = {mac::screen_scale,  mac::open,           mac::close,
                                  mac::present,       mac::poll,           true,
                                  mac::text_input,    mac::text_state,
                                  mac::text_selection, mac::text_select,
                                  mac::text_replace,  mac::clipboard_get,  mac::clipboard_set,
                                  mac::set_redraw,    mac::set_cursor,     mac::set_resizable};

const PlatformScreen* mac_screen() { return mac::load() ? &kMacScreen : 0; }

#endif  // __APPLE__

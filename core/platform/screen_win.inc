// screen_win.inc — Windows の窓（desktop.cpp から読む）
//
// 窓の道具（GUI ツールキット）には頼らない（spec/runtime/platform.md）。
// Windows の窓は OS そのものが持っているので、**実行時に**取りに行く。
//
//   ・user32.dll と gdi32.dll を LoadLibrary で開き、要る関数だけ引く
//     （そのため、作るときに繋ぐライブラリが1つも無い。X11 版と同じ考え）
//   ・画素は 32 ビットの並び（0x00RRGGBB）のまま StretchDIBits で貼る。
//     拡大は混ぜない（COLORONCOLOR ＝ そのまま引き伸ばす）
//   ・画面の細かさ（HiDPI）は OS に尋ね、窓は**画面の画素**で作る
//
// 開けなければ false を返すだけ。そのとき desktop.cpp は次の出し先を試す。
#if !defined(_WIN32)

const PlatformScreen* win_screen() { return 0; }

#else

namespace win {

// 横の車輪は新しめの SDK にしか無い。無ければ番号を直に置く
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

// --- 引いてくる関数 -------------------------------------------------------
// 形は <windows.h> のものをそのまま使う。ヘッダはどの Windows にも入っていて、
// 足りないのは「作るときに繋ぐ .lib」の方なので、そこだけ実行時に引く
ATOM (WINAPI* RegisterClassExW_)(const WNDCLASSEXW*) = 0;
HWND (WINAPI* CreateWindowExW_)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                HINSTANCE, LPVOID) = 0;
BOOL (WINAPI* DestroyWindow_)(HWND) = 0;
LRESULT (WINAPI* DefWindowProcW_)(HWND, UINT, WPARAM, LPARAM) = 0;
BOOL (WINAPI* ShowWindow_)(HWND, int) = 0;
BOOL (WINAPI* UpdateWindow_)(HWND) = 0;
BOOL (WINAPI* PeekMessageW_)(MSG*, HWND, UINT, UINT, UINT) = 0;
BOOL (WINAPI* TranslateMessage_)(const MSG*) = 0;
LRESULT (WINAPI* DispatchMessageW_)(const MSG*) = 0;
HDC (WINAPI* GetDC_)(HWND) = 0;
int (WINAPI* ReleaseDC_)(HWND, HDC) = 0;
BOOL (WINAPI* ValidateRect_)(HWND, const RECT*) = 0;
BOOL (WINAPI* AdjustWindowRect_)(RECT*, DWORD, BOOL) = 0;
LONG (WINAPI* SetWindowLongW_)(HWND, int, LONG) = 0;
LONG (WINAPI* GetWindowLongW_)(HWND, int) = 0;
BOOL (WINAPI* SetWindowPos_)(HWND, HWND, int, int, int, int, UINT) = 0;
HCURSOR (WINAPI* LoadCursorW_)(HINSTANCE, LPCWSTR) = 0;
HCURSOR (WINAPI* SetCursor_)(HCURSOR) = 0;
UINT (WINAPI* MapVirtualKeyW_)(UINT, UINT) = 0;
SHORT (WINAPI* GetKeyState_)(int) = 0;
BOOL (WINAPI* OpenClipboard_)(HWND) = 0;
BOOL (WINAPI* CloseClipboard_)(void) = 0;
BOOL (WINAPI* EmptyClipboard_)(void) = 0;
HANDLE (WINAPI* GetClipboardData_)(UINT) = 0;
HANDLE (WINAPI* SetClipboardData_)(UINT, HANDLE) = 0;
BOOL (WINAPI* IsClipboardFormatAvailable_)(UINT) = 0;
// 下の4つは無くてもよい（古い Windows。無ければ等倍として扱う）
UINT (WINAPI* GetDpiForSystem_)(void) = 0;
UINT (WINAPI* GetDpiForWindow_)(HWND) = 0;
BOOL (WINAPI* AdjustWindowRectExForDpi_)(RECT*, DWORD, BOOL, DWORD, UINT) = 0;
BOOL (WINAPI* SetProcessDpiAwarenessContext_)(void*) = 0;
BOOL (WINAPI* SetProcessDPIAware_)(void) = 0;

int (WINAPI* SetStretchBltMode_)(HDC, int) = 0;
int (WINAPI* StretchDIBits_)(HDC, int, int, int, int, int, int, int, int, const void*,
                             const BITMAPINFO*, UINT, DWORD) = 0;

// 変換つきの文字入力（IME）。imm32.dll も無くてよい。
// 無ければ「変換中の字は見えないが、確定した字は届く」ところまでに落ちる
HIMC (WINAPI* ImmGetContext_)(HWND) = 0;
BOOL (WINAPI* ImmReleaseContext_)(HWND, HIMC) = 0;
LONG (WINAPI* ImmGetCompositionStringW_)(HIMC, DWORD, LPVOID, DWORD) = 0;
BOOL (WINAPI* ImmSetCompositionWindow_)(HIMC, COMPOSITIONFORM*) = 0;
BOOL (WINAPI* ImmSetCandidateWindow_)(HIMC, CANDIDATEFORM*) = 0;
BOOL (WINAPI* ImmNotifyIME_)(HIMC, DWORD, DWORD, DWORD) = 0;
BOOL (WINAPI* ImmAssociateContextEx_)(HWND, HIMC, DWORD) = 0;
bool have_imm = false;

const wchar_t kClassName[] = L"SharkWindow";

struct State {
  bool open;
  HWND win;
  uint32_t* buf;   // 面の画素の写し（描き直しを頼まれたときに、もう一度貼るため）
  int sw, sh;      // 面の大きさ
  int rw, rh;      // 最後に知らせた大きさ（同じ知らせを重ねて出さないため）
  int scale;       // 面の1画素を、画面の何画素にするか
  bool sizing;     // 窓の縁を引いている最中（この間 OS はこちらを止める）
  bool (*redraw)(int, int);
  wchar_t hi;      // 前に来た上位のかけら（絵文字などは2つに分かれて届く）
  int wheel_ax, wheel_ay;   // 車輪の、1行に満たないぶんのため置き
  State() : open(false), win(0), buf(0), sw(0), sh(0), rw(0), rh(0), scale(1),
            sizing(false), redraw(0), hi(0), wheel_ax(0), wheel_ay(0) {}
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

HMODULE lib_user = 0, lib_gdi = 0, lib_imm = 0;
template <class T>
bool grab(HMODULE m, T* fn, const char* name) {
  *fn = (T)(void*)GetProcAddress(m, name);
  return *fn != 0;
}

bool load() {
  static int tried = 0;
  if (tried) return tried > 0;
  tried = -1;
  lib_user = LoadLibraryW(L"user32.dll");
  lib_gdi = LoadLibraryW(L"gdi32.dll");
  if (!lib_user || !lib_gdi) return false;
  bool ok = true;
  ok &= grab(lib_user, &RegisterClassExW_, "RegisterClassExW");
  ok &= grab(lib_user, &CreateWindowExW_, "CreateWindowExW");
  ok &= grab(lib_user, &DestroyWindow_, "DestroyWindow");
  ok &= grab(lib_user, &DefWindowProcW_, "DefWindowProcW");
  ok &= grab(lib_user, &ShowWindow_, "ShowWindow");
  ok &= grab(lib_user, &UpdateWindow_, "UpdateWindow");
  ok &= grab(lib_user, &PeekMessageW_, "PeekMessageW");
  ok &= grab(lib_user, &TranslateMessage_, "TranslateMessage");
  ok &= grab(lib_user, &DispatchMessageW_, "DispatchMessageW");
  ok &= grab(lib_user, &GetDC_, "GetDC");
  ok &= grab(lib_user, &ReleaseDC_, "ReleaseDC");
  ok &= grab(lib_user, &ValidateRect_, "ValidateRect");
  ok &= grab(lib_user, &AdjustWindowRect_, "AdjustWindowRect");
  ok &= grab(lib_user, &SetWindowLongW_, "SetWindowLongW");
  ok &= grab(lib_user, &GetWindowLongW_, "GetWindowLongW");
  ok &= grab(lib_user, &SetWindowPos_, "SetWindowPos");
  ok &= grab(lib_gdi, &SetStretchBltMode_, "SetStretchBltMode");
  ok &= grab(lib_gdi, &StretchDIBits_, "StretchDIBits");
  if (!ok) return false;
  grab(lib_user, &LoadCursorW_, "LoadCursorW");   // ここから下は無くてもよい
  grab(lib_user, &SetCursor_, "SetCursor");
  grab(lib_user, &MapVirtualKeyW_, "MapVirtualKeyW");
  grab(lib_user, &GetKeyState_, "GetKeyState");
  grab(lib_user, &OpenClipboard_, "OpenClipboard");
  grab(lib_user, &CloseClipboard_, "CloseClipboard");
  grab(lib_user, &EmptyClipboard_, "EmptyClipboard");
  grab(lib_user, &GetClipboardData_, "GetClipboardData");
  grab(lib_user, &SetClipboardData_, "SetClipboardData");
  grab(lib_user, &IsClipboardFormatAvailable_, "IsClipboardFormatAvailable");
  grab(lib_user, &GetDpiForSystem_, "GetDpiForSystem");
  grab(lib_user, &GetDpiForWindow_, "GetDpiForWindow");
  grab(lib_user, &AdjustWindowRectExForDpi_, "AdjustWindowRectExForDpi");
  grab(lib_user, &SetProcessDpiAwarenessContext_, "SetProcessDpiAwarenessContext");
  grab(lib_user, &SetProcessDPIAware_, "SetProcessDPIAware");

  // 変換つきの文字入力。ここが揃わなければ、変換の途中を受け取るのをやめて
  // OS に任せる（確定した字は WM_CHAR で届くので、日本語は打てる）
  lib_imm = LoadLibraryW(L"imm32.dll");
  if (lib_imm) {
    bool i_ok = true;
    i_ok &= grab(lib_imm, &ImmGetContext_, "ImmGetContext");
    i_ok &= grab(lib_imm, &ImmReleaseContext_, "ImmReleaseContext");
    i_ok &= grab(lib_imm, &ImmGetCompositionStringW_, "ImmGetCompositionStringW");
    i_ok &= grab(lib_imm, &ImmSetCompositionWindow_, "ImmSetCompositionWindow");
    i_ok &= grab(lib_imm, &ImmNotifyIME_, "ImmNotifyIME");
    grab(lib_imm, &ImmSetCandidateWindow_, "ImmSetCandidateWindow");   // 無くてもよい
    grab(lib_imm, &ImmAssociateContextEx_, "ImmAssociateContextEx");   // 無くてもよい
    have_imm = i_ok;
  }

  // 「画面の細かさは、こちらで面倒を見る」と OS に伝えておく。
  // 言わないと OS が勝手に引き伸ばし、細かい画面（HiDPI）でぼやける
  if (SetProcessDpiAwarenessContext_) {
    // -4 は「画面ごとに細かさが違ってもよい（第2版）」の目印
    if (!SetProcessDpiAwarenessContext_((void*)(intptr_t)-4) && SetProcessDPIAware_)
      SetProcessDPIAware_();
  } else if (SetProcessDPIAware_) {
    SetProcessDPIAware_();
  }
  tried = 1;
  return true;
}

// 画面の細かさ。OS は 1インチあたりの点の数で答える。96 が等倍、192 なら 2 倍
int screen_scale() {
  if (!load()) return 1;
  UINT dpi = 0;
  if (g.open && GetDpiForWindow_) dpi = GetDpiForWindow_(g.win);
  if (!dpi && GetDpiForSystem_) dpi = GetDpiForSystem_();
  if (!dpi) return 1;
  int s = (int)((dpi + 48) / 96);   // 96 きざみで四捨五入する
  return s < 1 ? 1 : (s > 4 ? 4 : s);
}

// **本当に小さい面だけ**、整数倍に広げる（ゲーム機のようなドット絵のため）。
// 引き伸ばしは画素をそのまま並べるだけなので、字も丸も粗くなる。
// 320×240 はそのままの大きさで出し、広げるのは切手のように小さいものだけ
int pick_scale(int w, int h) {
  int s = 1;
  while (s < 6 && w * (s + 1) <= 320 && h * (s + 1) <= 240) s++;
  return s;
}

void blit_dc(HDC dc) {
  if (!g.buf) return;
  BITMAPINFO bi;
  memset(&bi, 0, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = g.sw;
  bi.bmiHeader.biHeight = -g.sh;   // 負にすると、上の行から並んでいる形になる
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode_(dc, COLORONCOLOR);   // 混ぜずに、そのまま引き伸ばす
  StretchDIBits_(dc, 0, 0, g.sw * g.scale, g.sh * g.scale, 0, 0, g.sw, g.sh, g.buf, &bi,
                 DIB_RGB_COLORS, SRCCOPY);
}

void blit() {
  if (!g.open) return;
  HDC dc = GetDC_(g.win);
  if (!dc) return;
  blit_dc(dc);
  ReleaseDC_(g.win, dc);
}

// 打たれた文字（UTF-16 のかけら）を UTF-8 にして渡す
void push_text(unsigned int cp) {
  if (cp < 32 || cp == 127) return;   // 制御文字は文字として渡さない
  ScreenEvent e;
  e.kind = SEV_Text;
  int at = 0;
  if (cp < 0x80) {
    e.text[at++] = (char)cp;
  } else if (cp < 0x800) {
    e.text[at++] = (char)(0xc0 | (cp >> 6));
    e.text[at++] = (char)(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    e.text[at++] = (char)(0xe0 | (cp >> 12));
    e.text[at++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    e.text[at++] = (char)(0x80 | (cp & 0x3f));
  } else {
    e.text[at++] = (char)(0xf0 | (cp >> 18));
    e.text[at++] = (char)(0x80 | ((cp >> 12) & 0x3f));
    e.text[at++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    e.text[at++] = (char)(0x80 | (cp & 0x3f));
  }
  e.text[at] = 0;
  push(e);
}

// --- 変換つきの文字入力（IME）--------------------------------------------
//
// 変換そのものは OS（IME）に任せ、**確定した文字列と変換中の文字列だけ**を
// 受け取る（spec/library/ui.md）。macOS は見えない NSTextView に預けられるが、
// Windows にそういう受け皿は無いので、入力欄の中身はここで持つ。
//
// 数え方は**文字（コードポイント）**。コアの text_selection / text_select と
// 単位を合わせるため、UTF-8 のバイトではなくコードポイントのまま並べて持つ。
struct Input {
  bool on;
  Vec<int> text;      // 確定した中身（コードポイント）
  int caret, anchor;  // 文字の数。選んでいるのは、この2つの間
  Str marked;         // 変換中の文字列（UTF-8）。コアが下線を引いて出す
  bool composing;
  bool multi;         // 複数行の入力欄か。行にまつわるキーはコアが受け持つ
  int x, y, h;        // 入力欄の位置（面の座標）。候補をこのあたりに出してもらう
  Input() : on(false), caret(0), anchor(0), composing(false), multi(false), x(0), y(0), h(0) {}
};
Input in;

int sel_start() { return in.caret < in.anchor ? in.caret : in.anchor; }
int sel_len() { return in.caret < in.anchor ? in.anchor - in.caret : in.caret - in.anchor; }

// UTF-8 を「文字の並び」にほどく
void cps_of_utf8(const char* s, Vec<int>* out) {
  for (int i = 0; s[i];) {
    unsigned char c = (unsigned char)s[i];
    int cp, n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; n = 2; }
    else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; n = 3; }
    else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; n = 4; }
    else { i++; continue; }   // 筋の通らないバイトは読み飛ばす
    bool ok = true;
    for (int k = 1; k < n; k++) {
      if ((s[i + k] & 0xc0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
    }
    if (ok) out->push(cp);
    i += ok ? n : 1;
  }
}

void utf8_of_cp(unsigned int cp, Str* out) {
  if (cp < 0x80) { out->push((char)cp); return; }
  if (cp < 0x800) {
    out->push((char)(0xc0 | (cp >> 6)));
    out->push((char)(0x80 | (cp & 0x3f)));
    return;
  }
  if (cp < 0x10000) {
    out->push((char)(0xe0 | (cp >> 12)));
    out->push((char)(0x80 | ((cp >> 6) & 0x3f)));
    out->push((char)(0x80 | (cp & 0x3f)));
    return;
  }
  out->push((char)(0xf0 | (cp >> 18)));
  out->push((char)(0x80 | ((cp >> 12) & 0x3f)));
  out->push((char)(0x80 | ((cp >> 6) & 0x3f)));
  out->push((char)(0x80 | (cp & 0x3f)));
}

Str utf8_of_cps(const Vec<int>& v) {
  Str r;
  for (int i = 0; i < v.size(); i++) utf8_of_cp((unsigned int)v[i], &r);
  return r;
}

// UTF-16（OS から届く形）を文字の並びにする。上下に分かれた字も1つにまとめる
void cps_of_wide(const wchar_t* w, int n, Vec<int>* out) {
  for (int i = 0; i < n; i++) {
    unsigned int c = (unsigned int)w[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < n) {
      unsigned int lo = (unsigned int)w[i + 1];
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        out->push((int)(0x10000u + ((c - 0xd800) << 10) + (lo - 0xdc00)));
        i++;
        continue;
      }
    }
    out->push((int)c);
  }
}

// 選んでいるところを ins で置き換える。選んでいなければ、カーソルの位置に入れる
void replace_sel(const Vec<int>& ins) {
  int st = sel_start(), ln = sel_len();
  for (int i = 0; i < ln; i++) in.text.remove(st);
  for (int i = 0; i < ins.size(); i++) in.text.insert(st + i, ins[i]);
  in.caret = in.anchor = st + ins.size();
}

void insert_cp(int cp) {
  Vec<int> one;
  one.push(cp);
  replace_sel(one);
}

// 変換中の候補を、入力欄のあたりに出してもらう
void place_ime() {
  if (!have_imm || !g.open || !in.on) return;
  HIMC h = ImmGetContext_(g.win);
  if (!h) return;
  COMPOSITIONFORM cf;
  memset(&cf, 0, sizeof cf);
  cf.dwStyle = CFS_POINT;
  cf.ptCurrentPos.x = in.x * g.scale;
  cf.ptCurrentPos.y = in.y * g.scale;
  ImmSetCompositionWindow_(h, &cf);
  if (ImmSetCandidateWindow_) {
    CANDIDATEFORM cd;
    memset(&cd, 0, sizeof cd);
    cd.dwIndex = 0;
    // 入力欄そのものを避けて、すぐ下に出してもらう
    cd.dwStyle = CFS_EXCLUDE;
    cd.ptCurrentPos.x = in.x * g.scale;
    cd.ptCurrentPos.y = (in.y + in.h) * g.scale;
    cd.rcArea.left = in.x * g.scale;
    cd.rcArea.top = in.y * g.scale;
    cd.rcArea.right = (in.x + 1) * g.scale;
    cd.rcArea.bottom = (in.y + in.h) * g.scale;
    ImmSetCandidateWindow_(h, &cd);
  }
  ImmReleaseContext_(g.win, h);
}

// 変換の途中をやめさせる（入力欄から離れるとき）
void cancel_composition() {
  if (!have_imm || !g.open) return;
  HIMC h = ImmGetContext_(g.win);
  if (!h) return;
  ImmNotifyIME_(h, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
  ImmReleaseContext_(g.win, h);
  in.composing = false;
  in.marked.clear();
}

// IME を使う・使わないを OS に伝える。使わない間に変換が始まると、
// ゲームのキーが横取りされてしまう
void enable_ime(bool on) {
  if (!ImmAssociateContextEx_ || !g.open) return;
  ImmAssociateContextEx_(g.win, 0, on ? IACE_DEFAULT : 0);
}

// 変換中の文字列を OS から受け取る
void read_composition(HIMC h, DWORD which, Str* out) {
  out->clear();
  LONG bytes = ImmGetCompositionStringW_(h, which, 0, 0);
  if (bytes <= 0) return;
  int n = (int)(bytes / (LONG)sizeof(wchar_t));
  wchar_t* buf = (wchar_t*)malloc((size_t)bytes + sizeof(wchar_t));
  if (!buf) return;
  ImmGetCompositionStringW_(h, which, buf, (DWORD)bytes);
  Vec<int> cps;
  cps_of_wide(buf, n, &cps);
  free(buf);
  *out = utf8_of_cps(cps);
}

bool clipboard_get(Str* out);        // 下で定義（切り貼りの置き場）
void clipboard_set(const char* s);

bool key_held(int vk) { return GetKeyState_ && (GetKeyState_(vk) & 0x8000) != 0; }

// 入力欄の手直し（カーソル移動・消す・選ぶ・切り貼り）。
// 使ったら true を返し、そのキーはコアに渡さない。
// 変換を持つ出し先では、コアは入力欄の中身に触らない（core/lib/ui.cpp の
// input_frame）ので、ここが持たないと手直しができなくなる
bool edit_key(WPARAM vk) {
  bool shift = key_held(VK_SHIFT);
  bool ctrl = key_held(VK_CONTROL);
  int n = in.text.size();
  if (!ctrl) {
    switch (vk) {
      case VK_LEFT:
        if (in.caret > 0) in.caret--;
        if (!shift) in.anchor = in.caret;
        return true;
      case VK_RIGHT:
        if (in.caret < n) in.caret++;
        if (!shift) in.anchor = in.caret;
        return true;
      case VK_HOME:
        if (in.multi) return false;   // 行の頭はコアが数える（折り返しを知っているのは向こう）
        in.caret = 0;
        if (!shift) in.anchor = 0;
        return true;
      case VK_END:
        if (in.multi) return false;
        in.caret = n;
        if (!shift) in.anchor = n;
        return true;
      case VK_BACK:
        if (sel_len() > 0) replace_sel(Vec<int>());
        else if (in.caret > 0) { in.caret--; in.anchor = in.caret + 1; replace_sel(Vec<int>()); }
        return true;
      case VK_DELETE:
        if (sel_len() > 0) replace_sel(Vec<int>());
        else if (in.caret < n) { in.anchor = in.caret + 1; replace_sel(Vec<int>()); }
        return true;
      default:
        return false;
    }
  }
  // ctrl と一緒に押されたもの。menu bar を持たないので、ここで受ける
  switch (vk) {
    case 'A':
      in.anchor = 0;
      in.caret = n;
      return true;
    case 'C':
    case 'X': {
      int st = sel_start(), ln = sel_len();
      if (ln == 0) return true;
      Vec<int> sel;
      for (int i = 0; i < ln; i++) sel.push(in.text[st + i]);
      clipboard_set(utf8_of_cps(sel).c_str());
      if (vk == 'X') replace_sel(Vec<int>());
      return true;
    }
    case 'V': {
      Str s;
      if (!clipboard_get(&s)) return true;
      Vec<int> cps, flat;
      cps_of_utf8(s.c_str(), &cps);
      // 改行は '\n' に揃える。1行の入力欄で落とすのはコアの側（input_frame）
      for (int i = 0; i < cps.size(); i++) {
        if (cps[i] == '\r') {
          if (i + 1 < cps.size() && cps[i + 1] == '\n') continue;
          flat.push('\n');
          continue;
        }
        flat.push(cps[i]);
      }
      replace_sel(flat);
      return true;
    }
    default:
      return false;
  }
}

int key_of(WPARAM vk) {
  switch (vk) {
    case VK_LEFT: return SKEY_Left;
    case VK_RIGHT: return SKEY_Right;
    case VK_UP: return SKEY_Up;
    case VK_DOWN: return SKEY_Down;
    case VK_RETURN: return SKEY_Enter;
    case VK_ESCAPE: return SKEY_Escape;
    case VK_TAB: return SKEY_Tab;
    case VK_BACK: return SKEY_Back;
    case VK_DELETE: return SKEY_Delete;
    case VK_HOME: return SKEY_Home;
    case VK_END: return SKEY_End;
    case VK_PRIOR: return SKEY_PageUp;
    case VK_NEXT: return SKEY_PageDown;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return SKEY_Shift;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return SKEY_Ctrl;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return SKEY_Alt;
    case VK_SPACE: return (int)' ';
    default: break;
  }
  if (vk >= VK_F1 && vk <= VK_F12) return SKEY_F1 + (int)(vk - VK_F1);
  if (vk >= '0' && vk <= '9') return (int)vk;
  if (vk >= 'A' && vk <= 'Z') return (int)vk + 32;   // 小文字で渡す
  // 記号のキーは並びが機種ごとに違うので、OS に「何の字か」を尋ねる
  if (MapVirtualKeyW_) {
    UINT c = MapVirtualKeyW_((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7fffffffu;
    if (c >= 32 && c < 127) {
      if (c >= 'A' && c <= 'Z') c += 32;
      return (int)c;
    }
  }
  return -1;
}

void mouse_ev(LPARAM lp, int code, bool down) {
  ScreenEvent e;
  e.kind = SEV_Mouse;
  e.code = code;
  e.down = down;
  int x = (int)(short)LOWORD(lp) / g.scale;
  int y = (int)(short)HIWORD(lp) / g.scale;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= g.sw) x = g.sw - 1;
  if (y >= g.sh) y = g.sh - 1;
  e.x = x; e.y = y;
  push(e);
}

HCURSOR cur_ids[SCUR_Max] = {0};
int cur_now = SCUR_Arrow;
bool cur_hidden = false;

// OS がはじめから持っている形の番号（IDC_* は char 版の名前になってしまうので、
// UTF-16 の名前として作り直す）
LPCWSTR cursor_name(int kind) {
  unsigned id;
  switch (kind) {
    case SCUR_Hand: id = 32649; break;      // IDC_HAND
    case SCUR_Text: id = 32513; break;      // IDC_IBEAM
    case SCUR_Cross: id = 32515; break;     // IDC_CROSS
    case SCUR_Wait: id = 32514; break;      // IDC_WAIT
    case SCUR_ResizeX: id = 32644; break;   // IDC_SIZEWE
    case SCUR_ResizeY: id = 32645; break;   // IDC_SIZENS
    case SCUR_Move: id = 32646; break;      // IDC_SIZEALL
    default: id = 32512; break;             // IDC_ARROW
  }
  return MAKEINTRESOURCEW(id);
}

void apply_cursor() {
  if (!SetCursor_) return;
  if (cur_hidden) { SetCursor_(0); return; }
  if (!cur_ids[cur_now] && LoadCursorW_) cur_ids[cur_now] = LoadCursorW_(0, cursor_name(cur_now));
  SetCursor_(cur_ids[cur_now]);
}

// 面の大きさが変わった。写しの入れ物も作り直す
bool rebuild(int w, int h) {
  uint32_t* nb = (uint32_t*)malloc((size_t)w * (size_t)h * 4);
  if (!nb) return false;
  for (size_t i = 0; i < (size_t)w * (size_t)h; i++) nb[i] = 0;
  if (g.buf) free(g.buf);
  g.buf = nb;
  g.sw = w;
  g.sh = h;
  return true;
}

void size_changed(int cw, int ch) {
  int nw = cw / g.scale, nh = ch / g.scale;
  if (nw <= 0 || nh <= 0) return;
  if (nw == g.rw && nh == g.rh) return;
  g.rw = nw; g.rh = nh;
  ScreenEvent e; e.kind = SEV_Resize; e.x = nw; e.y = nh; push(e);
  // 縁を引いている間、OS はこちらを止めてしまう。頼まれていれば、
  // その場で描き直してもらう（PlatformScreen::set_redraw）
  if (g.sizing && g.redraw) g.redraw(nw, nh);
}

LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE: {
      ScreenEvent e; e.kind = SEV_Close; push(e);
      return 0;   // 閉じるかどうかを決めるのはプログラム。ここでは壊さない
    }
    case WM_PAINT:
      blit();
      if (ValidateRect_) ValidateRect_(hw, 0);   // 描き直しの印を消す
      return 0;
    case WM_PRINTCLIENT:
      // 「この紙に中身を写して」と頼まれたとき（画面の写しを取る道具が使う）
      blit_dc((HDC)wp);
      return 0;
    case WM_ERASEBKGND:
      return 1;   // 下地は塗らない（毎回ぜんぶ貼るので、ちらつくだけ）
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP: case WM_SYSKEYUP: {
      bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      // 変換の最中は、そのキーは変換のもの（enter は確定、esc は取り消し、
      // 空白と矢印は候補選び）。コアには渡さない
      if (in.composing) break;
      if (in.on && down && edit_key(wp)) return 0;   // 入力欄の手直しに使った
      int k = key_of(wp);
      if (k >= 0) key_ev(k, down);
      break;   // 文字は WM_CHAR で届くので、続けて OS にも渡す
    }
    case WM_CHAR: {
      wchar_t c = (wchar_t)wp;
      unsigned int cp;
      if (c >= 0xd800 && c <= 0xdbff) { g.hi = c; return 0; }   // 上位のかけら。相方を待つ
      if (c >= 0xdc00 && c <= 0xdfff) {
        if (!g.hi) return 0;
        cp = 0x10000u + (((unsigned int)(g.hi - 0xd800)) << 10) + (unsigned int)(c - 0xdc00);
        g.hi = 0;
      } else {
        g.hi = 0;
        cp = (unsigned int)c;
      }
      // 入力欄に入れている間は、こちらが受け取る（SEV_Text には出さない。
      // 二重に入ってしまうため）
      if (in.on) {
        if (cp >= 32 && cp != 127) insert_cp((int)cp);
        return 0;
      }
      push_text(cp);
      return 0;
    }

    // --- 変換つきの文字入力 ---
    case WM_IME_STARTCOMPOSITION:
      if (!have_imm || !in.on) break;
      in.composing = true;
      in.marked.clear();
      replace_sel(Vec<int>());   // 選んでいたところは、変換した字で置き換わる
      place_ime();
      return 0;   // 変換中の字はコアが描くので、OS の吹き出しは出させない
    case WM_IME_COMPOSITION: {
      if (!have_imm || !in.on) break;
      HIMC h = ImmGetContext_(hw);
      if (!h) break;
      if (lp & GCS_RESULTSTR) {   // 確定した
        Str got;
        read_composition(h, GCS_RESULTSTR, &got);
        Vec<int> cps;
        cps_of_utf8(got.c_str(), &cps);
        replace_sel(cps);
        in.marked.clear();
      }
      if (lp & GCS_COMPSTR) read_composition(h, GCS_COMPSTR, &in.marked);
      ImmReleaseContext_(hw, h);
      place_ime();
      return 0;   // ここで止める（渡すと、確定した字が WM_CHAR で二度入る）
    }
    case WM_IME_ENDCOMPOSITION:
      if (!have_imm || !in.on) break;
      in.composing = false;
      in.marked.clear();
      return 0;
    case WM_IME_SETCONTEXT:
      // 変換中の字はコアが入力欄に描く。OS の吹き出しは要らない（候補一覧は出す）
      if (have_imm && in.on) lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;
      break;
    case WM_LBUTTONDOWN: mouse_ev(lp, 0, true); return 0;
    case WM_LBUTTONUP:   mouse_ev(lp, 0, false); return 0;
    case WM_MBUTTONDOWN: mouse_ev(lp, 1, true); return 0;
    case WM_MBUTTONUP:   mouse_ev(lp, 1, false); return 0;
    case WM_RBUTTONDOWN: mouse_ev(lp, 2, true); return 0;
    case WM_RBUTTONUP:   mouse_ev(lp, 2, false); return 0;
    case WM_MOUSEMOVE:   mouse_ev(lp, -1, false); return 0;
    // 車輪（ホイール）。届くのは WHEEL_DELTA（120）の倍数なので、
    // 1行に満たないぶんはためておく
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      int d = (int)(short)HIWORD(wp);
      bool horiz = (msg == WM_MOUSEHWHEEL);
      int* acc = horiz ? &g.wheel_ax : &g.wheel_ay;
      *acc += d;
      int lines = *acc / 120;
      if (lines == 0) return 0;
      *acc -= lines * 120;
      ScreenEvent e;
      e.kind = SEV_Wheel;
      // 渡すのは 1/100 行。こちらは**行きっかりでしか送らない**ので 100 の倍数。
      // 前に回すと正で届く。縦は「先へ送る」を正にしたいので裏返す。横はそのまま
      e.x = horiz ? lines * 100 : 0;
      e.y = horiz ? 0 : -lines * 100;
      push(e);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) { apply_cursor(); return TRUE; }
      break;
    case WM_ENTERSIZEMOVE: g.sizing = true; return 0;
    case WM_EXITSIZEMOVE:  g.sizing = false; return 0;
    case WM_SIZE:
      size_changed((int)LOWORD(lp), (int)HIWORD(lp));
      return 0;
    default: break;
  }
  return DefWindowProcW_(hw, msg, wp, lp);
}

bool open(const char* title, int w, int h) {
  if (!load()) return false;
  if (g.open) return false;
  if (w <= 0 || h <= 0) return false;

  HINSTANCE inst = GetModuleHandleW(0);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = 0;   // マウスの形は WM_SETCURSOR でこちらが決める
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW_(&wc)) return false;
    registered = true;
  }

  g.scale = pick_scale(w, h);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT r;
  r.left = 0; r.top = 0;
  r.right = (LONG)(w * g.scale); r.bottom = (LONG)(h * g.scale);
  // 縁と題名の分だけ外側を広げて、**中身**が頼まれた大きさになるようにする
  UINT dpi = GetDpiForSystem_ ? GetDpiForSystem_() : 96;
  if (AdjustWindowRectExForDpi_) AdjustWindowRectExForDpi_(&r, style, FALSE, 0, dpi);
  else AdjustWindowRect_(&r, style, FALSE);

  Wide wt((title && title[0]) ? title : "Shark");
  g.win = CreateWindowExW_(0, kClassName, wt.get(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                           (int)(r.right - r.left), (int)(r.bottom - r.top), 0, 0, inst, 0);
  if (!g.win) return false;

  if (!rebuild(w, h)) { DestroyWindow_(g.win); g.win = 0; return false; }
  g.rw = w; g.rh = h;
  g.sizing = false;
  g.redraw = 0;
  g.hi = 0;
  cur_now = SCUR_Arrow;
  cur_hidden = false;
  qh = qn = 0;
  g.open = true;
  ShowWindow_(g.win, SW_SHOW);
  UpdateWindow_(g.win);
  return true;
}

void close() {
  if (!g.open) return;
  if (in.on) { cancel_composition(); enable_ime(false); in.on = false; }
  in.text.clear();
  in.caret = in.anchor = 0;
  in.marked.clear();
  g.open = false;
  if (g.win) { DestroyWindow_(g.win); g.win = 0; }
  if (g.buf) { free(g.buf); g.buf = 0; }
  for (int i = 0; i < SCUR_Max; i++) cur_ids[i] = 0;   // 形は OS のもの。返さなくてよい
}

void present(const uint32_t* px, int w, int h) {
  if (!g.open) return;
  if ((w != g.sw || h != g.sh) && !rebuild(w, h)) return;
  for (size_t i = 0; i < (size_t)w * (size_t)h; i++) g.buf[i] = px[i];
  blit();
}

void pump() {
  MSG m;
  for (int guard = 0; guard < 256 && PeekMessageW_(&m, 0, 0, 0, PM_REMOVE); guard++) {
    TranslateMessage_(&m);   // キーの押下を「打たれた文字」（WM_CHAR）に直してもらう
    DispatchMessageW_(&m);
  }
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

void set_redraw(bool (*fn)(int, int)) { g.redraw = fn; }

// --- 変換つきの文字入力（コアから呼ばれる口）------------------------------
//
// コアは入力欄を描くたびに text_input(true, ...) を呼び、そのあと text_state で
// 中身を読み直す。初めてのときと、呼んだ側が中身を変えたときだけ initial が来る
// （それ以外は 0 で、位置を伝えるだけ）。
void text_input(bool on, const char* initial, int x, int y, int h, bool multi) {
  if (!g.open) return;
  in.multi = multi;
  if (!on) {
    if (in.on) {
      cancel_composition();
      enable_ime(false);
    }
    in.on = false;
    in.text.clear();
    in.caret = in.anchor = 0;
    in.marked.clear();
    return;
  }
  if (!in.on) {
    in.on = true;
    in.composing = false;
    in.marked.clear();
    enable_ime(true);
  }
  if (initial) {   // 呼んだ側の中身に入れ直す。カーソルは末尾に置く
    in.text.clear();
    cps_of_utf8(initial, &in.text);
    in.caret = in.anchor = in.text.size();
  }
  in.x = x;
  in.y = y;
  in.h = h;
  place_ime();
}

bool text_state(Str* confirmed, Str* marked) {
  if (!in.on) return false;
  *confirmed = utf8_of_cps(in.text);
  *marked = in.marked;
  return true;
}

bool text_selection(int* start, int* len) {
  if (!in.on) return false;
  *start = sel_start();
  *len = sel_len();
  return true;
}

void text_select(int start, int len) {
  if (!in.on) return;
  int n = in.text.size();
  if (start < 0) start = 0;
  if (start > n) start = n;
  if (len < 0) len = 0;
  if (start + len > n) len = n - start;
  in.anchor = start;
  in.caret = start + len;
}

void text_replace(const char* s) {
  if (!in.on) return;
  Vec<int> cps;
  cps_of_utf8(s, &cps);
  replace_sel(cps);
}

void set_cursor(int kind) {
  if (kind < 0 || kind >= SCUR_Max) kind = SCUR_Arrow;
  cur_hidden = (kind == SCUR_None);
  cur_now = cur_hidden ? SCUR_Arrow : kind;
  if (g.open) apply_cursor();
}

// 縁を引いて大きさを変えられるかを切り替える
void set_resizable(bool on) {
  if (!g.open) return;
  LONG style = GetWindowLongW_(g.win, GWL_STYLE);
  if (on) style |= (LONG)(WS_THICKFRAME | WS_MAXIMIZEBOX);
  else style &= ~(LONG)(WS_THICKFRAME | WS_MAXIMIZEBOX);
  SetWindowLongW_(g.win, GWL_STYLE, style);
  SetWindowPos_(g.win, 0, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

// --- 切り貼りの置き場 -----------------------------------------------------
// 窓を開いていなくても使える（置き場は OS が持っている）
bool clipboard_get(Str* out) {
  if (!load() || !OpenClipboard_ || !GetClipboardData_ || !CloseClipboard_) return false;
  if (IsClipboardFormatAvailable_ && !IsClipboardFormatAvailable_(CF_UNICODETEXT)) return false;
  if (!OpenClipboard_(g.win)) return false;
  bool ok = false;
  HANDLE h = GetClipboardData_(CF_UNICODETEXT);
  if (h) {
    const wchar_t* p = (const wchar_t*)GlobalLock(h);
    if (p) { *out = from_wide(p); ok = true; GlobalUnlock(h); }
  }
  CloseClipboard_();
  return ok;
}

void clipboard_set(const char* s) {
  if (!load() || !OpenClipboard_ || !SetClipboardData_ || !EmptyClipboard_ || !CloseClipboard_)
    return;
  Wide w(s);
  size_t n = wcslen(w.get()) + 1;
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
  if (!h) return;
  wchar_t* dst = (wchar_t*)GlobalLock(h);
  if (!dst) { GlobalFree(h); return; }
  for (size_t i = 0; i < n; i++) dst[i] = w.get()[i];
  GlobalUnlock(h);
  if (!OpenClipboard_(g.win)) { GlobalFree(h); return; }
  EmptyClipboard_();
  if (!SetClipboardData_(CF_UNICODETEXT, h)) GlobalFree(h);   // 渡せたら OS のものになる
  CloseClipboard_();
}

}  // namespace win

// Windows は「離した」も出せる。
// 変換つきの文字入力（IME）も持つ。変換は OS に任せ、確定した字と変換中の字を
// 受け取って、描くのはコアの入力欄（spec/library/ui.md）
const PlatformScreen kWinScreen = {win::screen_scale,  win::open,  win::close,
                                   win::present,       win::poll,  true,
                                   win::text_input,    win::text_state,
                                   win::text_selection, win::text_select, win::text_replace,
                                   win::clipboard_get, win::clipboard_set,
                                   win::set_redraw,
                                   win::set_cursor,    win::set_resizable,
                                   false};   // 刻みはこちらで作る（眠って起きる）

const PlatformScreen* win_screen() { return win::load() ? &kWinScreen : 0; }

#endif

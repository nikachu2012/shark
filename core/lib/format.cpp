// format.cpp — 書式指定（spec/syntax.md「書式指定」）
//
// {式:[埋め文字][寄せ][0][幅][,][.桁数][種類]}
// 幅は表示幅で数える（全角は 2）。
#include "../registry.h"
#include "../value.h"

namespace shark {

struct FmtSpec {
  Str fill;
  char align;    // '<' '>' '^' または 0
  bool zero;
  int width;
  bool comma;
  int prec;      // -1 は指定なし
  char kind;     // 'd' 'f' 'e' 'b' 'o' 'x' 'X' '%' または 0
  FmtSpec() : align(0), zero(false), width(0), comma(false), prec(-1), kind(0) {}
};

static bool parse_spec(const Str& s, FmtSpec* f) {
  int i = 0;
  int n = s.size();
  if (n == 0) return true;
  // 埋め文字＋寄せ
  int first_len = 1;
  if ((unsigned char)s[0] >= 0x80) {
    unsigned char c = (unsigned char)s[0];
    first_len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
  }
  if (n > first_len && (s[first_len] == '<' || s[first_len] == '>' || s[first_len] == '^')) {
    f->fill = s.sub(0, first_len);
    f->align = s[first_len];
    i = first_len + 1;
  } else if (n > 0 && (s[0] == '<' || s[0] == '>' || s[0] == '^')) {
    f->align = s[0];
    i = 1;
  }
  if (i < n && s[i] == '0') { f->zero = true; i++; }
  while (i < n && s[i] >= '0' && s[i] <= '9') { f->width = f->width * 10 + (s[i] - '0'); i++; }
  if (i < n && s[i] == ',') { f->comma = true; i++; }
  if (i < n && s[i] == '.') {
    i++;
    f->prec = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') { f->prec = f->prec * 10 + (s[i] - '0'); i++; }
  }
  if (i < n) {
    char k = s[i];
    if (k == 'd' || k == 'f' || k == 'e' || k == 'b' || k == 'o' || k == 'x' || k == 'X' || k == '%') {
      f->kind = k;
      i++;
    }
  }
  return i == n;
}

static Str group3(const Str& digits) {
  Str out;
  int n = digits.size();
  for (int i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0) out += ",";
    out.push(digits[i]);
  }
  return out;
}

// 小数を固定桁で書く（環境差を出さないため自前で丸める）
static Str fixed(double v, int prec) {
  bool neg = v < 0;
  if (neg) v = -v;
  if (v != v) return Str("nan");
  if (v > 1.7976931348623157e307) return Str(neg ? "-inf" : "inf");
  double scale = 1.0;
  for (int i = 0; i < prec; i++) scale *= 10.0;
  double scaled = v * scale;
  // 最近接偶数に丸める
  double fl = (double)(int64_t)scaled;
  double frac = scaled - fl;
  int64_t ip = (int64_t)fl;
  if (frac > 0.5) ip += 1;
  else if (frac == 0.5) { if (ip % 2 != 0) ip += 1; }
  Str digits = str_from_int(ip);
  Str out;
  if (prec == 0) {
    out = digits;
  } else {
    while (digits.size() <= prec) digits = Str("0") + digits;
    out = digits.sub(0, digits.size() - prec) + "." + digits.sub(digits.size() - prec, prec);
  }
  if (neg) out = Str("-") + out;
  return out;
}

static Str expform(double v, int prec) {
  if (prec < 0) prec = 6;
  bool neg = v < 0;
  if (neg) v = -v;
  int e = 0;
  if (v != 0) {
    while (v >= 10.0) { v /= 10.0; e++; }
    while (v < 1.0) { v *= 10.0; e--; }
  }
  Str out = fixed(v, prec);
  out += "e";
  out += (e < 0 ? "-" : "+");
  int ae = e < 0 ? -e : e;
  if (ae < 10) out += "0";
  out += str_from_int(ae);
  if (neg) out = Str("-") + out;
  return out;
}

Str format_value(const Value& v, const Str& spec) {
  FmtSpec f;
  if (!parse_spec(spec, &f)) return val_to_display(v);

  Str body;
  bool numeric = false;
  bool negative = false;

  if (v.k == V_Int) {
    numeric = true;
    int64_t x = v.i;
    negative = x < 0;
    uint64_t u = negative ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;
    switch (f.kind) {
      case 'b': body = str_from_uint_base(u, 2, false); break;
      case 'o': body = str_from_uint_base(u, 8, false); break;
      case 'x': body = str_from_uint_base(u, 16, false); break;
      case 'X': body = str_from_uint_base(u, 16, true); break;
      case 'f': body = fixed((double)x, f.prec < 0 ? 6 : f.prec); negative = false; break;
      case 'e': body = expform((double)x, f.prec); negative = false; break;
      case '%': body = fixed((double)x * 100.0, f.prec < 0 ? 1 : f.prec) + "%"; negative = false; break;
      default: body = str_from_uint_base(u, 10, false); break;
    }
    if (f.comma) body = group3(body);
  } else if (v.k == V_Float) {
    numeric = true;
    double x = v.f;
    negative = x < 0;
    double ax = negative ? -x : x;
    switch (f.kind) {
      case 'f': body = fixed(ax, f.prec < 0 ? 6 : f.prec); break;
      case 'e': body = expform(ax, f.prec); break;
      case '%': body = fixed(ax * 100.0, f.prec < 0 ? 1 : f.prec) + "%"; break;
      case 'd': body = str_from_int((int64_t)ax); break;
      default:
        if (f.prec >= 0) body = fixed(ax, f.prec);
        else { body = str_from_float(ax); }
        break;
    }
    if (f.comma) {
      int dot = -1;
      for (int i = 0; i < body.size(); i++) if (body[i] == '.') { dot = i; break; }
      if (dot < 0) body = group3(body);
      else body = group3(body.sub(0, dot)) + body.sub(dot, body.size() - dot);
    }
  } else {
    body = val_to_display(v);
  }

  Str sign = negative ? Str("-") : Str();
  int w = utf8_display_width(body) + utf8_display_width(sign);
  if (w >= f.width) return sign + body;

  int pad = f.width - w;
  Str fill = f.fill.size() ? f.fill : Str(" ");
  if (f.zero && numeric && (f.align == 0 || f.align == '>')) {
    Str zeros;
    for (int i = 0; i < pad; i++) zeros += "0";
    return sign + zeros + body;
  }
  char align = f.align;
  if (align == 0) align = numeric ? '>' : '<';
  Str left, right;
  if (align == '<') {
    for (int i = 0; i < pad; i++) right += fill;
  } else if (align == '>') {
    for (int i = 0; i < pad; i++) left += fill;
  } else {
    int l = pad / 2, r = pad - l;
    for (int i = 0; i < l; i++) left += fill;
    for (int i = 0; i < r; i++) right += fill;
  }
  return left + sign + body + right;
}

}  // namespace shark

// file.cpp — std.file（spec/library/file.md）
//
// 移植層にファイル機能が無ければ、このモジュールは登録されない（import が E0501）。
#include "../platform/platform.h"
#include "../registry.h"
#include "../value.h"
#include "../vm.h"

namespace shark {

static Value* A(Value* args, int i) { return val_deref(&args[i]); }
static const Str& S(Value* args, int i) { return ((StrObj*)A(args, i)->o)->s; }

void file_obj_dispose(FileObj* f) {
  if (f->h && !f->closed && platform().file) platform().file->close(f->h);
  f->h = 0;
}

static bool read_all(const char* path, Str* out, Str* err) {
  const PlatformFile* pf = platform().file;
  void* h = pf->open(path, "r", err);
  if (!h) return false;
  char buf[4096];
  for (;;) {
    int n = pf->read(h, buf, sizeof buf);
    if (n <= 0) break;
    out->append(buf, n);
  }
  pf->close(h);
  return true;
}

static NativeStatus f_read(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str body, err;
  if (!read_all(S(a, 0).c_str(), &body, &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_str(body));
  return N_Ok;
}
static NativeStatus f_read_bytes(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str body, err;
  if (!read_all(S(a, 0).c_str(), &body, &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_bytes(body));
  return N_Ok;
}
static bool write_all(const char* path, const Str& data, const char* mode, Str* err) {
  const PlatformFile* pf = platform().file;
  void* h = pf->open(path, mode, err);
  if (!h) return false;
  bool ok = pf->write(h, data.data(), data.size());
  pf->close(h);
  if (!ok) *err = Str("書き込めません: ") + path;
  return ok;
}
static NativeStatus f_write(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str err;
  if (!write_all(S(a, 0).c_str(), S(a, 1), "w", &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus f_append(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str err;
  if (!write_all(S(a, 0).c_str(), S(a, 1), "a", &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus f_exists(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_bool(platform().file->exists(S(a, 0).c_str()));
  return N_Ok;
}
static NativeStatus f_is_dir(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  out = mk_bool(platform().file->is_dir(S(a, 0).c_str()));
  return N_Ok;
}
static NativeStatus f_size(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t sz = 0;
  if (!platform().file->size(S(a, 0).c_str(), &sz)) {
    out = mk_result_err(vm.make_error(Str("大きさを取れません: ") + S(a, 0), 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_int(sz));
  return N_Ok;
}
static NativeStatus f_modified(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  int64_t ns = 0;
  if (!platform().file->modified(S(a, 0).c_str(), &ns)) {
    out = mk_result_err(vm.make_error(Str("更新時刻を取れません: ") + S(a, 0), 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_time(ns));
  return N_Ok;
}
static NativeStatus f_remove(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str err;
  if (!platform().file->remove(S(a, 0).c_str(), &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus f_copy(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str body, err;
  if (!read_all(S(a, 0).c_str(), &body, &err) || !write_all(S(a, 1).c_str(), body, "w", &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus f_rename(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str err;
  if (!platform().file->rename(S(a, 0).c_str(), S(a, 1).c_str(), &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus f_list(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Vec<Str> names;
  Str err;
  if (!platform().file->list(S(a, 0).c_str(), &names, &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  Value l = mk_list();
  for (int i = 0; i < names.size(); i++) as_list(l)->v.push(mk_str(names[i]));
  out = mk_result_ok(l);
  val_release(l);
  return N_Ok;
}
static NativeStatus f_make_dir(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str err;
  if (!platform().file->make_dir(S(a, 0).c_str(), &err)) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus f_open(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  Str err;
  void* h = platform().file->open(S(a, 0).c_str(), S(a, 1).c_str(), &err);
  if (!h) {
    out = mk_result_err(vm.make_error(err, 0));
    return N_Ok;
  }
  FileObj* fo = new (sk_alloc(sizeof(FileObj))) FileObj();
  fo->h = h;
  Value v = mk_obj_value(fo);
  out = mk_result_ok(v);
  val_release(v);
  return N_Ok;
}
static NativeStatus fm_read_line(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  FileObj* f = (FileObj*)A(a, 0)->o;
  if (f->closed) { out = mk_none(); return N_Ok; }
  Str line;
  for (;;) {
    for (int i = 0; i < f->pending.size(); i++) {
      if (f->pending[i] != '\n') continue;
      line = f->pending.sub(0, i);
      f->pending = f->pending.sub(i + 1, f->pending.size() - i - 1);
      if (line.size() > 0 && line[line.size() - 1] == '\r') line = line.sub(0, line.size() - 1);
      out = mk_str(line);
      return N_Ok;
    }
    char buf[1024];
    int got = f->eof ? 0 : platform().file->read(f->h, buf, sizeof buf);
    if (got <= 0) {
      f->eof = true;
      if (f->pending.size() == 0) { out = mk_none(); return N_Ok; }
      out = mk_str(f->pending);
      f->pending.clear();
      return N_Ok;
    }
    f->pending.append(buf, got);
  }
}
static NativeStatus fm_read(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  FileObj* f = (FileObj*)A(a, 0)->o;
  if (f->closed) {
    out = mk_result_err(vm.make_error(Str("閉じたファイルは読めません"), 0));
    return N_Ok;
  }
  int64_t want = A(a, 1)->i;
  if (want <= 0) {
    out = mk_result_ok(mk_bytes(Str()));
    return N_Ok;
  }
  const int64_t kOnce = 1 << 30;   // 一度に読むのはここまで
  if (want > kOnce) want = kOnce;
  Str data;
  while (data.size() < want) {
    if (f->pending.size() > 0) {
      int take = (int)(want - data.size());
      if (take > f->pending.size()) take = f->pending.size();
      data.append(f->pending.data(), take);
      f->pending = f->pending.sub(take, f->pending.size() - take);
      continue;
    }
    char buf[4096];
    int chunk = (int)(want - data.size());
    if (chunk > (int)sizeof buf) chunk = sizeof buf;
    int got = platform().file->read(f->h, buf, chunk);
    if (got <= 0) break;
    data.append(buf, got);
  }
  out = mk_result_ok(mk_bytes(data));
  return N_Ok;
}
static NativeStatus fm_write(VM& vm, Value* a, int n, Value& out) {
  (void)n;
  FileObj* f = (FileObj*)A(a, 0)->o;
  if (f->closed || !platform().file->write(f->h, S(a, 1).data(), S(a, 1).size())) {
    out = mk_result_err(vm.make_error(Str("書き込めません"), 0));
    return N_Ok;
  }
  out = mk_result_ok(mk_void());
  return N_Ok;
}
static NativeStatus fm_close(VM& vm, Value* a, int n, Value& out) {
  (void)vm; (void)n;
  FileObj* f = (FileObj*)A(a, 0)->o;
  if (!f->closed && f->h) platform().file->close(f->h);
  f->closed = true;
  f->h = 0;
  out = mk_void();
  return N_Ok;
}

void register_file(Registry& r) {
  if (!platform().file) return;   // 持たない機種では入れない
  TypeTable& t = r.types();
  Type* ts = t.t_string();
  Type* ti = t.t_int();
  Type* tb = t.t_bool();
  Type* tv = t.t_void();
  Type* tby = t.t_bytes();
  r.add("file.read", f_read, t.result_of(ts), ts);
  r.add("file.read_bytes", f_read_bytes, t.result_of(tby), ts);
  r.add("file.write", f_write, t.result_of(tv), ts, ts);
  r.add("file.write_bytes", f_write, t.result_of(tv), ts, tby);
  r.add("file.append", f_append, t.result_of(tv), ts, ts);
  r.add("file.exists", f_exists, tb, ts);
  r.add("file.is_dir", f_is_dir, tb, ts);
  r.add("file.size", f_size, t.result_of(ti), ts);
  r.add("file.modified", f_modified, t.result_of(t.simple(T_Time)), ts);
  r.add("file.remove", f_remove, t.result_of(tv), ts);
  r.add("file.copy", f_copy, t.result_of(tv), ts, ts);
  r.add("file.rename", f_rename, t.result_of(tv), ts, ts);
  r.add("file.list", f_list, t.result_of(t.list_of(ts)), ts);
  r.add("file.make_dir", f_make_dir, t.result_of(tv), ts);
  r.add("file.open", f_open, t.result_of(t.simple(T_File)), ts, ts);
  r.add_untyped("file.File.read_line", fm_read_line);
  r.add_untyped("file.File.read", fm_read);
  r.add_untyped("file.File.write", fm_write);
  r.add_untyped("file.File.close", fm_close);
  r.enable_module("std.file");
}

}  // namespace shark

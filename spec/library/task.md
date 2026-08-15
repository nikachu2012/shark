# std.task

タスクとチャネルの操作。考え方と `task` / `parallel` の構文は
[../runtime/concurrency.md](../runtime/concurrency.md) にある。

```shark
import std.task;   // task, parallel, channel を使うだけなら省略できる
```

## Task&lt;T&gt;

`task f()` が返す値。

| メソッド | 説明 |
|---|---|
| `wait() -> T` | 終わるまで待って戻り値を受け取る |
| `wait_timeout(t: Duration) -> T?` | 時間を区切って待つ。間に合わなければ `none` |
| `done() -> bool` | 終わっているか（待たない） |
| `cancel() -> void` | 取り消しを頼む。すぐには止まらない |
| `id() -> int` | 見分けるための番号 |

```shark
var t = task heavy();

while !t.done() {
  print("計算中...");
  sleep(0.5);
}
print(t.wait());
```

## channel&lt;T&gt;

| 関数・メソッド | 説明 |
|---|---|
| `channel<T>() -> channel<T>` | 作る。受け渡しは1件ずつ |
| `channel<T>(capacity: int)` | 指定の件数までためられる |
| `ch.send(v: T) -> Result<void>` | 送る。ためられなければ空くまで待つ |
| `ch.recv() -> T?` | 受け取る。閉じていれば `none` |
| `ch.try_recv() -> T?` | 待たずに受け取る |
| `ch.close() -> void` | 閉じる |
| `ch.len() -> int` | たまっている件数 |

```shark
func producer(ch: channel<int>) {
  for var i in range(5) { _ = ch.send(i); }
  ch.close();
}

var ch = channel<int>(10);
task producer(ch);

while var v = ch.recv() {     // 閉じられるまで受け取り続ける
  print(v);
}
```

`channel` はハンドル型。コピーしても同じ待ち行列を指す
（[../runtime/memory.md](../runtime/memory.md)）。

### 送り手は何個でもよい

受け取るのは1つのタスクだけ。複数のタスクが同じチャネルに送れる。

```shark
var ch = channel<string>();
task worker(ch, "a");
task worker(ch, "b");

for var i in range(2) { print(ch.recv()!); }   // 先に終わった方から届く
```

**複数のチャネルを同時に待つ書き方（`select` にあたるもの）は無い。**
種類の違う通知は、チャネルと受け取るタスクを分ける
（[../runtime/concurrency.md](../runtime/concurrency.md)）。

### 取り消し

`cancel()` は要求を立てるだけで、待っている関数を外から断ち切らない。
待つ関数が `step()` ごとに要求を見て、その場でやめる。

## そのほか

| 関数 | 説明 |
|---|---|
| `task.yield() -> void` | 待ちが無くても他のタスクに順番を譲る |
| `task.count() -> int` | 走っているタスクの数 |

長い計算を回すタスクは、ときどき `task.yield()` を呼ぶと他のタスクが動ける。

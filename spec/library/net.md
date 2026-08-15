# std.net

TCP の通信。HTTP は [http.md](http.md)。

```shark
import std.net;
```

任意モジュール。通信を扱えない処理系では `import` の時点でエラーになる
（[overview.md](overview.md)）。

## つなぐ

| 関数・メソッド | 説明 |
|---|---|
| `net.connect(host: string, port: int) -> Result<Conn>` | つなぐ |
| `net.connect_timeout(host: string, port: int, t: Duration) -> Result<Conn>` | 時間を区切ってつなぐ |
| `c.read(n: int) -> Result<bytes>` | `n` バイトまで読む |
| `c.read_line() -> Result<string>` | 改行まで読む |
| `c.write(b: bytes) -> Result<int>` | 書く。書けたバイト数を返す |
| `c.write_text(s: string) -> Result<int>` | UTF-8 で書く |
| `c.close() -> void` | 閉じる |

```shark
var c = try net.connect("example.com", 80);
_ = c.write_text("GET / HTTP/1.0\r\n\r\n");
var line = try c.read_line();
c.close();
```

読み書きの待ちの間、**そのタスクだけが止まる**。他のタスクは動き続ける
（[../runtime/concurrency.md](../runtime/concurrency.md)）。

待っている間も取り消しの要求を見ている。`t.cancel()` を呼べば、
`step()` 1回分のうちに待つのをやめる。
移植層のソケットは待ちっぱなしにならない形で用意する
（[../runtime/platform.md](../runtime/platform.md)）。

## 待ち受ける

| 関数・メソッド | 説明 |
|---|---|
| `net.listen(port: int) -> Result<Listener>` | 待ち受ける |
| `l.accept() -> Result<Conn>` | 1件受け付ける |
| `l.close() -> void` | やめる |

```shark
var l = try net.listen(8080);

while true {
  var c = try l.accept();
  task handle(c);          // 1件ごとにタスクを立てる
}
```

## 調べる

| 関数 | 説明 |
|---|---|
| `net.resolve(host: string) -> Result<list<string>>` | 名前から住所を引く |

`Conn` と `Listener` はハンドル型。コピーしても同じ接続を指す
（[../runtime/memory.md](../runtime/memory.md)）。

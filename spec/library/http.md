# std.http

HTTP でのやり取り。

```shark
import std.http;
```

## 手軽に呼ぶ

| 関数 | 説明 |
|---|---|
| `http.get(url: string) -> Result<Response>` | 取得する |
| `http.post(url: string, body: string) -> Result<Response>` | 送る |
| `http.post_json(url: string, body: Json) -> Result<Response>` | JSON で送る |

```shark
var res = try http.get("https://example.com/fish.json");
if res.status() != 200 {
  return Error(f"取得に失敗: {res.status()}");
}
var data = try json.parse(res.body());
```

複数を同時に取りたいときは `parallel` を使う
（[../runtime/concurrency.md](../runtime/concurrency.md)）。

```shark
var pages = parallel {
  task http.get(url_a);
  task http.get(url_b);
};
```

## Response

| メソッド | 説明 |
|---|---|
| `status() -> int` | 状態コード |
| `body() -> string` | 本文（UTF-8 として解釈） |
| `body_bytes() -> bytes` | 本文（そのまま） |
| `header(name: string) -> string?` | 見出しを1つ引く |
| `headers() -> map<string, string>` | 見出しを全部 |

## 細かく指定する

| 関数・メソッド | 説明 |
|---|---|
| `http.request(method: string, url: string) -> Request` | 組み立て始める |
| `r.header(name: string, value: string) -> Request` | 見出しを足す |
| `r.body(text: string) -> Request` | 本文を入れる |
| `r.timeout(t: Duration) -> Request` | 制限時間 |
| `r.send() -> Result<Response>` | 送る |

`Request` のメソッドは自分を返すので、続けて書ける。

```shark
var res = try http.request("PUT", url)
  .header("Content-Type", "application/json")
  .timeout(time.seconds(5.0))
  .body(json.stringify(obj))
  .send();
```

## 注意

- 任意モジュール。取り込めない処理系では `import` の時点でエラーになる
- 暗号化（HTTPS）は移植層に任せる。使えない環境では `Result` の失敗を返す
- リダイレクトは既定で5回まで追う
- 待っている間も取り消しの要求を見る。`t.cancel()` で途中でやめられる
- 外部ライブラリには依存しない（[../runtime/platform.md](../runtime/platform.md)）

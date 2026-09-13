#!/usr/bin/env python3
"""bench/run.py — C / Python / Shark のベンチマーク実行・性能比較スクリプト

    python3 bench/run.py            # すべてのベンチマークを実行
    python3 bench/run.py loop fib   # 指定したケースのみ実行

・同一アルゴリズムを各言語で実装し、実行結果の一致を検証した上で実行時間を測定
・各ケースを 3 回実行し、最速タイム（プロセスの起動オーバーヘッドを含む実時間）を採用
"""
import os
import platform
import shutil
import subprocess
import sys
import time

# Windows 環境ではコンソールの既定コードページが UTF-8 ではない場合があるため、
# 標準入出力を UTF-8 に統一する（文字化けやエンコーディングエラーの防止）。
if sys.platform == 'win32':
    import ctypes
    ctypes.windll.kernel32.SetConsoleOutputCP(65001)
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

ROOT = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
BENCH = os.path.join(ROOT, "bench")
BUILD = os.path.join(BENCH, "build")
REPEAT = 3

# ベンチマークケース定義（名前 -> 概要説明）
CASES = [
    ("loop", "整数加算ループ 1,000万回（sum += i % 7）"),
    ("fib", "再帰フィボナッチ計算 fib(32)（呼び出し回数 436万回）"),
    ("list", "動的配列への 100万件要素追加と合計計算（5回試行）"),
    ("dict", "ハッシュマップへの 50万件挿入と 50万回ルックアップ"),
    ("format", "文字列補間による書式付き文字列 100万件生成と合計長計算"),
    ("startup", "プロセス起動オーバーヘッド（exit 0 までの所要時間）"),
]


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')


def need_cc():
    """C コンパイラ（cc）の存在を確認。未検出の場合はインストール案内を出力して終了"""
    if shutil.which("cc"):
        return
    print("エラー: C コンパイラ（cc）が見つかりません。ベンチマーク比較には C コンパイラが必要です。")
    if sys.platform == 'win32':
        print("  Windows 環境では MSYS2（pacman -S mingw-w64-x86_64-gcc）または")
        print("  LLVM（clang）をインストールし、cc として実行できるようにパスを設定してください。")
        print("  ※ Visual Studio の cl.exe はコマンドライン引数の互換性がないため使用できません。")
    sys.exit(1)


def build_c(name):
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, name + (".exe" if sys.platform == 'win32' else ""))
    src = os.path.join(BENCH, "c", name + ".c")
    r = sh(["cc", "-O2", "-o", out, src])
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(1)
    return out


def measure(cmd):
    """3 回実行し、最速の実行時間（秒）と標準出力を取得"""
    best = None
    out = None
    for _ in range(REPEAT):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        dt = time.perf_counter() - t0
        if r.returncode != 0:
            return None, "（失敗）" + r.stderr.strip()[:200]
        out = r.stdout.strip()
        best = dt if best is None else min(best, dt)
    return best, out


def main():
    want = sys.argv[1:]
    cases = [c for c in CASES if not want or c[0] in want]

    shark = os.path.join(ROOT, "shark")
    if not os.path.exists(shark) and os.path.exists(shark + ".exe"):
        shark += ".exe"   # Windows

    if not os.path.exists(shark):
        print("エラー: 先に make を実行して shark バイナリをビルドしてください")
        sys.exit(1)

    need_cc()

    print("実行環境")
    print("  OS       :", platform.platform())
    print("  CPU      :", platform.processor() or platform.machine())
    cc = sh(["cc", "--version"]).stdout.splitlines()
    print("  C        :", cc[0] if cc else "?", "(-O2)")
    print("  Python   :", sys.version.split()[0])
    print("  Shark    :", sh([shark, "version"]).stdout.strip())
    print()

    rows = []
    for name, desc in cases:
        exe = build_c(name)
        results = {}
        outs = {}
        for lang, cmd in (
            ("C", [exe]),
            ("Python", [sys.executable, os.path.join(BENCH, "py", name + ".py")]),
            ("Shark", [shark, "run", os.path.join(BENCH, "shark", name + ".shk")]),
        ):
            t, out = measure(cmd)
            results[lang] = t
            outs[lang] = out
        same = len(set(outs.values())) == 1
        rows.append((name, desc, results, outs["C"], same))
        mark = "" if same else "  ← 出力結果が一致しません"
        print("%-8s C %8.1f ms   Python %8.1f ms   Shark %8.1f ms   結果 %s%s" % (
            name,
            results["C"] * 1000, results["Python"] * 1000, results["Shark"] * 1000,
            outs["C"], mark))
        if not same:
            for k, v in outs.items():
                print("    %-7s %s" % (k, v))

    print()
    print("| ベンチマーク項目 | C (-O2) | Python | Shark | Shark ÷ C | Shark ÷ Python |")
    print("|---|---|---|---|---|---|")
    for name, desc, r, out, same in rows:
        print("| %s | %.0f ms | %.0f ms | %.0f ms | %.0f 倍 | %.2f 倍 |" % (
            desc, r["C"] * 1000, r["Python"] * 1000, r["Shark"] * 1000,
            r["Shark"] / r["C"], r["Shark"] / r["Python"]))


if __name__ == "__main__":
    main()

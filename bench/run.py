#!/usr/bin/env python3
"""bench/run.py — C・Python・Shark を同じ内容で走らせて時間を測る。

    python3 bench/run.py            # 全部
    python3 bench/run.py loop fib   # 選んで

・同じアルゴリズムを3つの言語で書き、出力が一致することを確かめてから測る
・各3回走らせて、いちばん速かった回を採る（プロセスの起動時間も含む）
"""
import os
import platform
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(os.path.dirname(__file__)))
BENCH = os.path.join(ROOT, "bench")
BUILD = os.path.join(BENCH, "build")
REPEAT = 3

# 名前 -> 何をするか（README にも出す説明）
CASES = [
    ("loop", "整数のループ 1000万回（sum += i % 7）"),
    ("fib", "再帰呼び出し fib(32)（436万回の呼び出し）"),
    ("list", "可変長配列に 100万件足して合計する（5回）"),
    ("dict", "key-value に 50万件入れて、50万回引く"),
    ("format", "書式付きの文字列を 100万個作り、長さを合計する"),
    ("startup", "起動して 0 を出すだけ（下敷きの時間）"),
]


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def build_c(name):
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, name)
    src = os.path.join(BENCH, "c", name + ".c")
    r = sh(["cc", "-O2", "-o", out, src])
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(1)
    return out


def measure(cmd):
    """3回走らせて、いちばん速い実時間（秒）と出力を返す"""
    best = None
    out = None
    for _ in range(REPEAT):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True, text=True)
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
    if not os.path.exists(shark):
        print("先に make してください")
        sys.exit(1)

    print("環境")
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
        mark = "" if same else "  ← 出力が食い違っています"
        print("%-8s C %8.1f ms   Python %8.1f ms   Shark %8.1f ms   結果 %s%s" % (
            name,
            results["C"] * 1000, results["Python"] * 1000, results["Shark"] * 1000,
            outs["C"], mark))
        if not same:
            for k, v in outs.items():
                print("    %-7s %s" % (k, v))

    print()
    print("| 内容 | C (-O2) | Python | Shark | Shark ÷ C | Shark ÷ Python |")
    print("|---|---|---|---|---|---|")
    for name, desc, r, out, same in rows:
        print("| %s | %.0f ms | %.0f ms | %.0f ms | %.0f 倍 | %.2f 倍 |" % (
            desc, r["C"] * 1000, r["Python"] * 1000, r["Shark"] * 1000,
            r["Shark"] / r["C"], r["Shark"] / r["Python"]))


if __name__ == "__main__":
    main()

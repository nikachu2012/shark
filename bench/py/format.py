# 書式付きの文字列を 100万個作り、長さを合計する
total = 0
for i in range(1000000):
    total += len(f"fish-{i}")
print(total)

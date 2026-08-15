# 可変長配列に 100万件足して合計する。これを5回くり返す
total = 0
for _ in range(5):
    xs = []
    for i in range(1000000):
        xs.append(i)
    for x in xs:
        total += x
print(total)

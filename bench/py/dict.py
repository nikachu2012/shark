# key-value に 50万件入れて、50万回引く
m = {}
for i in range(500000):
    m[i] = i * 2
total = 0
for i in range(500000):
    total += m[i]
print(total)

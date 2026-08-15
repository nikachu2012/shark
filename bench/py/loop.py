# 整数のループ 1000万回
sum_ = 0
for i in range(10000000):
    sum_ += i % 7
print(sum_)

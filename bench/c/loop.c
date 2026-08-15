/* 整数のループ 1000万回 */
#include <stdio.h>
int main(void) {
  long long sum = 0;
  for (long long i = 0; i < 10000000LL; i++) sum += i % 7;
  printf("%lld\n", sum);
  return 0;
}

/* 再帰呼び出し fib(32) */
#include <stdio.h>
static long long fib(long long n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
int main(void) {
  printf("%lld\n", fib(32));
  return 0;
}

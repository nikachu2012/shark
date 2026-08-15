/* 可変長配列に 100万件足して合計する。これを5回くり返す */
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  long long total = 0;
  for (int round = 0; round < 5; round++) {
    long long cap = 8, n = 0;
    long long* xs = (long long*)malloc(sizeof(long long) * cap);
    for (long long i = 0; i < 1000000LL; i++) {
      if (n == cap) { cap *= 2; xs = (long long*)realloc(xs, sizeof(long long) * cap); }
      xs[n++] = i;
    }
    for (long long i = 0; i < n; i++) total += xs[i];
    free(xs);
  }
  printf("%lld\n", total);
  return 0;
}

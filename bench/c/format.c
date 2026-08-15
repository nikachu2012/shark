/* 書式付きの文字列を 100万個作り、長さを合計する */
#include <stdio.h>
#include <string.h>
int main(void) {
  long long total = 0;
  char buf[64];
  for (long long i = 0; i < 1000000LL; i++) {
    snprintf(buf, sizeof buf, "fish-%lld", i);
    total += (long long)strlen(buf);
  }
  printf("%lld\n", total);
  return 0;
}

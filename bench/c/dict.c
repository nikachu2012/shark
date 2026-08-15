/* key-value に 50万件入れて、50万回引く（開番地法の表を自前で持つ） */
#include <stdio.h>
#include <stdlib.h>

#define N 500000

typedef struct { long long key, val; int used; } Slot;
static Slot* tab;
static long long cap;

static void put(long long k, long long v) {
  long long mask = cap - 1, b = (long long)((unsigned long long)k * 1099511628211ULL & (unsigned long long)mask);
  while (tab[b].used && tab[b].key != k) b = (b + 1) & mask;
  tab[b].key = k; tab[b].val = v; tab[b].used = 1;
}
static long long get(long long k) {
  long long mask = cap - 1, b = (long long)((unsigned long long)k * 1099511628211ULL & (unsigned long long)mask);
  while (tab[b].used) {
    if (tab[b].key == k) return tab[b].val;
    b = (b + 1) & mask;
  }
  return 0;
}
int main(void) {
  cap = 1;
  while (cap < (long long)N * 2) cap *= 2;
  tab = (Slot*)calloc((size_t)cap, sizeof(Slot));
  for (long long i = 0; i < N; i++) put(i, i * 2);
  long long sum = 0;
  for (long long i = 0; i < N; i++) sum += get(i);
  printf("%lld\n", sum);
  free(tab);
  return 0;
}

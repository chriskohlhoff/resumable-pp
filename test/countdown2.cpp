#include <stdio.h>

int main()
{
  auto f1 = [n = int(10)]() resumable
  {
    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };

  auto f2 = [n = int(5)]() resumable
  {
    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };

  auto f3 = [&]() resumable
  {
    yield from f1;
    return from f2;
  };

  while (!is_terminal(f3))
    printf("%d\n", f3());
}

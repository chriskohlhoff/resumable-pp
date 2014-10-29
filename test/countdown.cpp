#include <stdio.h>

int main()
{
  auto f = [n = int(10)]() resumable
  {
    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };

  while (!is_terminal(f))
    printf("%d\n", f());
}

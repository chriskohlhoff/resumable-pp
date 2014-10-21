#include <stdio.h>

int main()
{
  int n = 10;
  auto f = [n]() resumable
  {
    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };

  while (!f.is_terminal())
    printf("%d\n", f());
}

#include <stdio.h>
#include <utility>

int main()
{
  auto f = [n = int(10)]() resumable
  {
    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };

  while (!f.is_terminal())
    printf("%d\n", f());
}

#include <stdio.h>

int main()
{
  int n = 10;

  auto&& f = [&n] resumable
  {
    while (--n > 0)
      if (n == 1) return;
      else break_resumable;
  };

  while (!ready(f))
  {
    resume(f);
    printf("%d\n", n);
  }
}

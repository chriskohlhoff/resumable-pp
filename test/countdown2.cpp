#include <stdio.h>

int main()
{
  auto&& f = [] resumable
  {
    int n = 10;
    for (; n > 0; --n)
      co_yield n;
    return n;
  };

  while (!ready(f))
    printf("%d\n", f());
}

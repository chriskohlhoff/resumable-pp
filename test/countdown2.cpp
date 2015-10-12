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
  {
    int n = resume(f);
    printf("%d\n", ready(f) ? result(f) : n);
  }
}

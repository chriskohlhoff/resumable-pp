#include <stdio.h>

auto countdown(int n)
{
  return [=]() resumable
  {
    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };
}

typedef decltype(countdown(0)) countdown_t;

int main()
{
  auto f = [&]() resumable
  {
    countdown_t f1(countdown(10));
    yield from f1;
    countdown_t f2(countdown(5));
    return from f2;
  };

  while (!f.is_terminal())
    printf("%d\n", f());
}

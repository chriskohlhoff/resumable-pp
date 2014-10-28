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

int main()
{
  auto f = [&]() resumable
  {
    yield from countdown(10);
    return from countdown(5);
  };

  while (!is_terminal(f))
    printf("%d\n", f());
}

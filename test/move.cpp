#include <stdio.h>
#include <utility>

template <int N>
struct copyable
{
  copyable() {}
  copyable(const copyable&) { printf("copying %d\n", N); }
  copyable(copyable&&) { printf("moving %d\n", N); }
};

int main()
{
  auto g1 = []() resumable -> int
  {
    {
      copyable<1> c1;
      yield 1;
      yield 2;
    }
    {
      copyable<2> c2;
      yield 3;
      yield 4;
    }
  };

  printf("g1 returned %d\n", g1());
  printf("g1 returned %d\n", g1());
  auto g2(std::move(g1));
  printf("g1 is_terminal() returned %d\n", is_terminal(g1) ? 1 : 0);
  printf("g2 returned %d\n", g2());
}

#include <stdio.h>

template <int N>
struct noncopyable
{
  noncopyable() { printf("noncopyable()\n"); }
  ~noncopyable() { printf("~noncopyable()\n"); }
  noncopyable(const noncopyable&) = delete;
};

int main()
{
  auto&& f = [n = int(10)]() resumable
  {
    noncopyable<1> c1;

    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };

  while (!is_terminal(f))
    printf("%d\n", f());
}

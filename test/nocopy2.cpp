#include <stdio.h>
#include <utility>

template <int N>
struct noncopyable
{
  noncopyable() { printf("noncopyable()\n"); }
  ~noncopyable() { printf("~noncopyable()\n"); }
  noncopyable(const noncopyable&) = delete;
};

auto countdown(int n)
{
  auto&& f = [=]() resumable
  {
    noncopyable<1> c1;

    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  };

  return *std::move(f);
}

int main()
{
  auto i = countdown(10);
  decltype(i)::lambda f(std::move(i));

  while (!f.is_terminal())
    printf("%d\n", f());
}

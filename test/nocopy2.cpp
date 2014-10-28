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
  return initializer([=]() resumable
  {
    noncopyable<1> c1;

    while (--n > 0)
      if (n == 1) return n;
      else yield n;
  });
}

int main()
{
  auto i = countdown(10);
  lambda_t<decltype(i)> f(std::move(i));

  while (!is_terminal(f))
    printf("%d\n", f());
}

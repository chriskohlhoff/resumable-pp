#include <iostream>

template <class F> void run(F f)
{
  f();
}

class foo
{
public:
  void bar()
  {
    run([=]() resumable { baz(this); });
  }

  void baz(foo*)
  {
  }
};

int main()
{
  auto f = [](int* i) resumable { return *i + 42; };
  int i = 42;
  int j = 123;
  run([=, &j]() resumable { std::cout << i << j << "\n"; });
}

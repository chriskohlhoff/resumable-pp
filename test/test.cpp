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
    run([=]() mutable { baz(); });
  }

  void baz()
  {
  }
};

int main()
{
  auto f = [](int* i) mutable { return *i + 42; };
  int i = 42;
  int j = 123;
  run([=, &j]() mutable { std::cout << i << j << "\n"; });
}

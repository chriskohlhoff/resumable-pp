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
    run([=]{ baz(); });
  }

  void baz()
  {
  }
};

int main()
{
  auto f = []{ return 42; };
  int i = 42;
  int j = 123;
  run([=, &j]{ std::cout << i << j << "\n"; });
}

#include <iostream>
#include <utility>

auto print_1_to(int n)
{
  return lambda_initializer(
    [n] resumable {
      for (int i = 1;;)
      {
        std::cout << i << std::endl;
        if (++i > n) break;
        break_resumable;
      }
    }
  );
}

int main()
{
  auto i = print_1_to(10);
  initializer_lambda<decltype(i)> r(std::move(i));
  while (!ready(r))
  {
    std::cout << "resuming ... ";
    resume(r);
  }
}

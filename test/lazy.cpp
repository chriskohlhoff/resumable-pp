#include <stdio.h>
#include <utility>

class heavy
{
public:
  heavy() { printf("heavy()\n"); }
  ~heavy() { printf("~heavy()\n"); }
  void print() { printf("here we are\n"); }
};

int main()
{
  auto lazy_h = []() resumable -> heavy* {
    for (heavy h;;)
      yield &h;
  };

  printf("before use\n");
  lazy_h()->print();
  printf("after use\n");
}

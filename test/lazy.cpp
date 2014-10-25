#include <stdio.h>
#include <utility>

class heavy
{
public:
  heavy(const char* s) { printf("heavy(%s)\n", s); }
  ~heavy() { printf("~heavy()\n"); }
  void print() { printf("here we are\n"); }
};

int main()
{
  const char* text = "hello";

  auto lazy_h = [=]() resumable -> heavy* {
    for (heavy h(text);;)
      yield &h;
  };

  printf("before use\n");
  lazy_h()->print();
  printf("after use\n");
}

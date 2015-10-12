#include <stdio.h>
#include <cassert>

class heavy
{
public:
  heavy(const char* s) { printf("heavy(%s)\n", s); }
  heavy(const heavy&) { printf("heavy(const heavy&)\n"); }
  ~heavy() { printf("~heavy()\n"); }
  void print() { printf("here we are\n"); }
};

int main()
{
  auto&& h = [=] resumable {
    return heavy("foo");
  };

  printf("before call\n");
  resume(h);
  printf("after call\n");
  assert(ready(h));
  result(h).print();
}

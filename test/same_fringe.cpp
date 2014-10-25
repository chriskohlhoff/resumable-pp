#include <stdio.h>

template <class T>
class generator
{
public:
  generator()
    : impl_(nullptr)
  {
  }

  generator(decltype(nullptr))
    : impl_(nullptr)
  {
  }

  template <class G>
  generator(G g)
    : impl_(new impl<G>(g))
  {
  }

  generator(const generator& other)
    : impl_(other.impl_ ? other.impl_->clone() : nullptr)
  {
  }

  generator(generator&& other)
    : impl_(other.impl_)
  {
    other.impl_ = nullptr;
  }

  generator& operator=(const generator& other)
  {
    return operator=(generator(other));
  }

  generator& operator=(generator&& other)
  {
    delete impl_;
    impl_ = other.impl_;
    other.impl_ = nullptr;
    return *this;
  }

  template <class G>
  generator& operator=(G g)
  {
    return operator=(generator(g));
  }

  ~generator()
  {
    delete impl_;
  }

  T operator()()
  {
    return impl_->invoke();
  }

  bool is_terminal() const
  {
    return impl_ ? impl_->is_terminal() : true;
  }
  
private:
  struct impl_base
  {
    virtual ~impl_base() {}
    virtual impl_base* clone() const = 0;
    virtual T invoke() = 0;
    virtual bool is_terminal() const = 0;
  };

  template <class G>
  struct impl : impl_base
  {
    explicit impl(G g) : g_(g) {}
    virtual impl_base* clone() const { return new impl(g_); }
    virtual T invoke() { return g_(); }
    virtual bool is_terminal() const { return g_.is_terminal(); }
    G g_;
  };

  impl_base* impl_;
};

struct node
{
  node* left = nullptr;
  node* right = nullptr;
  int value = 0;
};

#if 0

generator<int> flatten(node& n)
{
  return [&] {
    if (n.left) yield from flatten(*n.left);
    if (!n.right) return n.value;
    yield n.value;
    return from flatten(*n.right);
  };
}

#endif

generator<int> flatten(node& n)
{
  return [&]() -> int {
    if (n.left)
    {
      generator<int> g1 = flatten(*n.left);
      yield from g1;
    }
    if (!n.right) return n.value;
    yield n.value;
    generator<int> g2 = flatten(*n.right);
    return from g2;
  };
}

int main()
{
  node root;
  root.value = 1;
  root.left = new node;
  root.left->value = 2;
  root.right = new node;
  root.right->value = 3;
  root.right->left = new node;
  root.right->left->value = 4;

  generator<int> g = flatten(root);
  while (!g.is_terminal())
    printf("%d\n", g());
}

#include <stdio.h>
#include <typeinfo>

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

  bool is_terminal() const noexcept
  {
    return impl_ ? impl_->is_terminal() : true;
  }

  const std::type_info& wanted_type() const noexcept
  {
    return impl_ ? impl_->wanted_type() : typeid(void);
  }

  void* wanted() noexcept
  {
    return impl_ ? impl_->wanted() : nullptr;
  }

  const void* wanted() const noexcept
  {
    return impl_ ? impl_->wanted() : nullptr;
  }
  
private:
  struct impl_base
  {
    virtual ~impl_base() {}
    virtual impl_base* clone() const = 0;
    virtual T invoke() = 0;
    virtual bool is_terminal() const noexcept = 0;
    virtual const std::type_info& wanted_type() const noexcept = 0;
    virtual void* wanted() noexcept = 0;
    virtual const void* wanted() const noexcept = 0;
  };

  template <class G>
  struct impl : impl_base
  {
    explicit impl(G g) : g_(g) {}
    virtual impl_base* clone() const { return new impl(g_); }
    virtual T invoke() { return g_(); }
    virtual bool is_terminal() const noexcept { return ::is_terminal(g_); }
    virtual const std::type_info& wanted_type() const noexcept { return ::wanted_type(g_); }
    virtual void* wanted() noexcept { return ::wanted(g_); }
    virtual const void* wanted() const noexcept { return ::wanted(g_); }
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
  return [&]() resumable -> int {
    if (n.left) yield from flatten(*n.left);
    if (!n.right) return n.value;
    yield n.value;
    return from flatten(*n.right);
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
  while (!is_terminal(g))
    printf("%d\n", g());
}

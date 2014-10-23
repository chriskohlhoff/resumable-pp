#include <stdio.h>

class function_base
{
public:
  virtual ~function_base() {}
  virtual void invoke() = 0;

private:
  friend class scheduler;
  function_base* next_ = nullptr;
};

template <class F>
class function : public function_base
{
public:
  explicit function(F f)
    : f_(f)
  {
  }

  virtual ~function()
  {
  }

  virtual void invoke()
  {
    f_();
  }

private:
  F f_;
};

class scheduler
{
public:
  template <class F>
  int post(F f)
  {
    if (last_)
    {
      last_->next_ = new function<F>(f);
      last_ = last_->next_;
    }
    else
    {
      first_ = last_ = new function<F>(f);
    }

    return 0;
  }

  void run()
  {
    while (first_)
    {
      function_base* f = first_;
      first_ = first_->next_;
      if (!first_)
        last_ = nullptr;
      f->invoke();
      delete f;
    }
  }

private:
  function_base* first_ = nullptr;
  function_base* last_ = nullptr;
};

int main()
{
  scheduler sched;

  auto f = [&sched, i = int(0)]() resumable
  {
    for (i = 0; i < 10; ++i)
    {
      printf("f: %d\n", i);
      yield sched.post(*lambda_this);
    }
  };

  auto g = [&sched, i = int(0)]() resumable
  {
    for (i = 0; i < 5; ++i)
    {
      printf("g: %d\n", i);
      yield sched.post(*lambda_this);
    }
  };

  sched.post(f);
  sched.post(g);
  sched.run();
}

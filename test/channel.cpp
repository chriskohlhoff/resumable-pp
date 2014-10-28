#include <stdio.h>
#include <typeinfo>

template <class T>
class channel
{
public:
  class waiter
  {
  public:
    waiter(const waiter&) = delete;
    waiter& operator=(const waiter&) = delete;
    waiter& operator=(waiter&&) = delete;

    waiter(T* value, channel* c)
      : value_(value), channel_(c)
    {
      insert();
    }

    waiter(waiter&& w)
      : value_(w.value_), channel_(w.channel_)
    {
      w.remove();
      insert();
    }

    ~waiter()
    {
      remove();
    }

    void operator()()
    {
      if (!channel_)
        value_ = nullptr; // indicates that value has been retrieved
    }

    bool is_terminal() const noexcept
    {
      return value_ == nullptr;
    }

    const std::type_info& wanted_type() const noexcept
    {
      return typeid(void);
    }

    void* wanted() noexcept
    {
      return nullptr;
    }

    const void* wanted() const noexcept
    {
      return nullptr;
    }

  private:
    void insert()
    {
      if (channel_)
      {
        if (channel_->last_)
        {
          prev_ = channel_->last_;
          channel_->last_->next_ = this;
          channel_->last_ = this;
        }
        else
        {
          channel_->first_ = channel_->last_ = this;
        }
      }
    }

    void remove()
    {
      if (channel_)
      {
        if (channel_->first_ == this)
          channel_->first_ = next_;
        if (channel_->last_ == this)
          channel_->last_ = prev_;
        if (next_)
          next_->prev_ = prev_;
        if (prev_)
          prev_->next_ = next_;
        next_ = prev_ = nullptr;
        channel_ = nullptr;
      }
    }

    friend class channel;
    T* value_; // null means terminated
    channel* channel_; // non-null means still waiting for value
    waiter* next_ = nullptr;
    waiter* prev_ = nullptr;
  };

  waiter push(T* value)
  {
    if (first_ && mode_ == have_pullers)
    {
      *first_->value_ = *value;
      first_->remove();
      return { value, nullptr };
    }
    else
    {
      mode_ = have_pushers;
      return { value, this };
    }
  }

  waiter pull(T* value)
  {
    if (first_ && mode_ == have_pushers)
    {
      *value = *first_->value_;
      first_->remove();
      return { value, nullptr };
    }
    else
    {
      mode_ = have_pullers;
      return { value, this };
    }
  }

private:
  waiter* first_ = nullptr;
  waiter* last_ = nullptr;
  enum { have_pushers, have_pullers } mode_ = have_pushers;
};

int main()
{
  channel<int> c;

  auto&& pusher = [&]() resumable
  {
    int i;
    for (i = 0; i < 10; ++i)
      yield from c.push(&i);
    i = -1;
    yield from c.push(&i);
  };

  auto&& puller = [&]() resumable
  {
    int i;
    do
    {
      yield from c.pull(&i);
      printf("%d\n", i);
    } while (i != -1);
  };

  while (!is_terminal(puller))
  {
    puller();
    pusher();
  }
}

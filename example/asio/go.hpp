#ifndef GO_HPP
#define GO_HPP

#include <asio/async_result.hpp>
#include <asio/handler_type.hpp>
#include <exception>
#include <memory>
#include <type_traits>

class coro
{
public:
  template <class F>
  explicit coro(F&& f) :
    impl_(new impl<typename std::decay<F>::type>(std::forward<F>(f)))
  {
  }

private:
  struct impl_base
  {
    virtual ~impl_base() {}
    virtual void* wanted() = 0;
    void (*resume_)(coro);
  };

  template <class F>
  struct impl : impl_base
  {
    explicit impl(F&& f) :
      f_(std::move(f))
    {
      resume_ = &impl::resume;
    }

    virtual void* wanted()
    {
      return ::wanted(f_);
    }

    static void resume(coro c)
    {
      static_cast<impl*>(c.impl_.get())->f_(std::move(c));
    }

    typename F::lambda f_;
  };

  template <class F> friend void go(F&&);
  template <class...> friend class coro_handler;
  std::unique_ptr<impl_base> impl_;
};

template <class F>
void go(F&& f)
{
  coro c(initializer(std::move(f)));
  c.impl_->resume_(std::move(c));
}

struct async_generator_init
{
  typedef class async_generator generator_type;
};

class async_generator
{
public:
  async_generator() {}
  async_generator(const async_generator&) = delete;
  async_generator(async_generator&&) = delete;
  async_generator& operator=(const async_generator&) = delete;
  async_generator& operator=(async_generator&&) = delete;

  void construct(async_generator_init&&) {}
  void destroy() {}

  void operator()()
  {
    if (state_ == ready)
    {
      state_ = terminal;
      if (ex_) std::rethrow_exception(ex_);
    }
  }

  bool is_terminal() const noexcept
  {
    return state_ == terminal;
  }

  const std::type_info& wanted_type() const noexcept
  {
    return typeid(std::exception_ptr);
  }

  void* wanted() noexcept
  {
    return this;
  }

  const void* wanted() const noexcept
  {
    return this;
  }

private:
  template <class...> friend class coro_handler;
  enum { initial, ready, terminal } state_ = initial;
  std::exception_ptr ex_;
};

template <class... Args>
class coro_handler
{
public:
  explicit coro_handler(coro& c)
    : coro_(std::move(c))
  {
  }

  void operator()(Args...)
  {
    static_cast<async_generator*>(coro_.impl_->wanted())->state_ = async_generator::ready;
    coro_.impl_->resume_(std::move(coro_));
  }

private:
  coro coro_;
};

template <class... Args>
class coro_handler<std::exception_ptr, Args...>
{
public:
  explicit coro_handler(coro& c)
    : coro_(std::move(c))
  {
  }

  void operator()(std::exception_ptr ex, Args...)
  {
    static_cast<async_generator*>(coro_.impl_->wanted())->ex_ = ex;
    static_cast<async_generator*>(coro_.impl_->wanted())->state_ = async_generator::ready;
    coro_.impl_->resume_(std::move(coro_));
  }

private:
  coro coro_;
};

template <class... Args>
class coro_handler<std::error_code, Args...>
{
public:
  explicit coro_handler(coro& c)
    : coro_(std::move(c))
  {
  }

  void operator()(std::error_code ec, Args...)
  {
    if (ec)
    {
      std::system_error ex(ec);
      static_cast<async_generator*>(coro_.impl_->wanted())->ex_ = std::make_exception_ptr(ex);
    }
    static_cast<async_generator*>(coro_.impl_->wanted())->state_ = async_generator::ready;
    coro_.impl_->resume_(std::move(coro_));
  }

private:
  coro coro_;
};

namespace asio {

template <class R, class... Args>
struct handler_type<coro, R(Args...)>
{
  typedef coro_handler<Args...> type;
};

template <class... Args>
struct async_result<coro_handler<Args...>>
{
  typedef async_generator_init type;
  explicit async_result(coro_handler<Args...>&) {}
  type get() { return {}; }
};

} // namespace asio

#endif // GO_HPP

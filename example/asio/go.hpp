#ifndef GO_HPP
#define GO_HPP

#include <asio/async_result.hpp>
#include <asio/handler_type.hpp>
#include <exception>
#include <memory>
#include <tuple>
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
    void (*resume_)(coro&);
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

    static void resume(coro& c)
    {
      do static_cast<impl*>(c.impl_.get())->f_(c);
      while (c.impl_ && !is_terminal(static_cast<impl*>(c.impl_.get())->f_));
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
  c.impl_->resume_(c);
}

template <class... Args>
class async_generator;

template <class... Args>
struct async_generator_init
{
  typedef async_generator<Args...> generator_type;
};

template <class... Args>
inline std::tuple<Args...> get_result(const std::tuple<Args...>& result)
{
  return result;
}

template <class Arg>
inline std::tuple<Arg> get_result(const std::tuple<Arg>& result)
{
  return std::get<0>(result);
}

inline void get_result(const std::tuple<std::exception_ptr>& result)
{
  if (std::get<0>(result))
    std::rethrow_exception(std::get<0>(result));
}

inline void get_result(const std::tuple<std::error_code>& result)
{
  if (std::get<0>(result))
    throw std::system_error(std::get<0>(result));
}

template <class Arg>
inline Arg get_result(const std::tuple<std::exception_ptr, Arg>& result)
{
  if (std::get<0>(result))
    std::rethrow_exception(std::get<0>(result));
  return std::get<1>(result);
}

template <class Arg>
inline Arg get_result(const std::tuple<std::error_code, Arg>& result)
{
  if (std::get<0>(result))
    throw std::system_error(std::get<0>(result));
  return std::get<1>(result);
}

template <class... Args>
class async_generator
{
public:
  async_generator() {}
  async_generator(const async_generator&) = delete;
  async_generator(async_generator&&) = delete;
  async_generator& operator=(const async_generator&) = delete;
  async_generator& operator=(async_generator&&) = delete;

  void construct(async_generator_init<Args...>&&) {}
  void destroy() {}

  auto operator()()
  {
    if (state_ == ready)
      state_ = terminal;
    return get_result(result_);
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
  std::tuple<typename std::decay<Args>::type...> result_;
};

template <class... Args>
class coro_handler
{
public:
  explicit coro_handler(coro& c)
    : coro_(std::move(c))
  {
  }

  void operator()(Args... args)
  {
    static_cast<async_generator<Args...>*>(coro_.impl_->wanted())->result_ = std::make_tuple(args...);
    static_cast<async_generator<Args...>*>(coro_.impl_->wanted())->state_ = async_generator<Args...>::ready;
    coro_.impl_->resume_(coro_);
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
  typedef async_generator_init<Args...> type;
  explicit async_result(coro_handler<Args...>&) {}
  type get() { return {}; }
};

} // namespace asio

#endif // GO_HPP

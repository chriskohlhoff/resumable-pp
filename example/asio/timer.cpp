#include <asio/io_service.hpp>
#include <asio/steady_timer.hpp>
#include <cstdio>
#include "go.hpp"

using asio::steady_timer;

int main()
{
  asio::io_service io_service;

  go([&](coro&& c) {
      asio::steady_timer timer(io_service);
      for (int i = 0; i < 10; ++i)
      {
        timer.expires_after(std::chrono::seconds(1));
        yield from timer.async_wait(c);
        std::printf("after wait %d\n", i);
      }
    });

  io_service.run();
}

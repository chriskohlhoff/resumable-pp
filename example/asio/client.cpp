#include <asio/io_service.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <cstdio>
#include "go.hpp"

using asio::async_read;
using asio::buffer;
using asio::ip::address_v4;
using asio::ip::tcp;

int main()
{
  asio::io_service io_service;

  std::size_t total = 64;
  go([&, total](coro& c) {
      char buf[64 * 1024];
      tcp::socket conn(io_service);
      yield from conn.async_connect({address_v4::loopback(), 13375}, c);
      yield from async_read(conn, buffer(buf, total), c);
    });

  io_service.run();
}

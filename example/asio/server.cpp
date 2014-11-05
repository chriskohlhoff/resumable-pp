#include <asio/io_service.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
#include <cstdio>
#include "go.hpp"

using asio::async_write;
using asio::buffer;
using asio::ip::address_v4;
using asio::ip::tcp;

void echo(tcp::socket sock)
{
  go([sock=std::move(sock)](coro& c) resumable {
      std::size_t length;
      char data[1024];
      for (;;)
      {
        length = yield from sock.async_read_some(buffer(data), c);
        yield from async_write(sock, buffer(data, length), c);
      }
    });
}

int main()
{
  asio::io_service io_service;
  tcp::acceptor acceptor(io_service, {tcp::v4(), 55555});

  go([&](coro& c) resumable {
      for (;;)
      {
        tcp::socket sock(io_service);
        yield from acceptor.async_accept(sock, c);
        echo(std::move(sock));
      }
    });

  for (;;)
  {
    try
    {
      io_service.run();
      break; // Run exited normally.
    }
    catch (std::exception&)
    {
      // Ignore and resume running the io_service.
    }
  }
}

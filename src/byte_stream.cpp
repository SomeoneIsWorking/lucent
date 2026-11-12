#include "lucent/byte_stream.h"

#include "http_socket.h"

#include <array>

namespace lucent::net {

namespace detail = http::detail;

std::unique_ptr<TcpStream> TcpStream::connect(const Endpoint &server, std::string &error) {
  if (!detail::initialize_socket_runtime()) {
    error = "socket runtime unavailable";
    return nullptr;
  }
  const detail::Socket socket = detail::create_tcp_socket();
  if (detail::is_invalid_socket(socket)) {
    error = "cannot create a socket";
    return nullptr;
  }
  std::unique_ptr<TcpStream> stream{new TcpStream{socket, true}};
  detail::set_close_on_exec(socket);
  detail::set_socket_timeouts(socket);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(server.port);
  address.sin_addr.s_addr = htonl(server.address);
  if (::connect(detail::native_socket(socket), reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
    error = "cannot connect to " + format_ipv4(server.address) + ":" + std::to_string(server.port);
    return nullptr;
  }
  return stream;
}

std::unique_ptr<TcpStream> TcpStream::borrow(std::uintptr_t socket) {
  return std::unique_ptr<TcpStream>{new TcpStream{socket, false}};
}

TcpStream::~TcpStream() {
  if (owned_) {
    detail::close_socket(socket_);
  }
}

std::string describe(ReadOutcome outcome, std::string_view closed) {
  switch (outcome) {
  case ReadOutcome::Closed:
    return std::string{closed};
  case ReadOutcome::TimedOut:
    return "timed out";
  case ReadOutcome::Data:
  case ReadOutcome::Failed:
    break;
  }
  return "receive failed";
}

std::uintptr_t TcpStream::native() const noexcept {
  return socket_;
}

bool TcpStream::send_all(std::string_view bytes, std::string &error) {
  while (!bytes.empty()) {
    const std::ptrdiff_t sent = detail::send_bytes(socket_, bytes.data(), bytes.size());
    if (sent < 0 && detail::socket_error_interrupted(detail::last_socket_error())) {
      continue;
    }
    if (sent <= 0) {
      error = "send failed";
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

ReadOutcome TcpStream::read_some(std::string &buffer) {
  std::array<char, 4096> chunk{};
  for (;;) {
    const std::ptrdiff_t received = detail::receive_bytes(socket_, chunk.data(), chunk.size());
    if (received > 0) {
      buffer.append(chunk.data(), static_cast<std::size_t>(received));
      return ReadOutcome::Data;
    }
    if (received == 0) {
      return ReadOutcome::Closed;
    }
    const int code = detail::last_socket_error();
    if (detail::socket_error_interrupted(code)) {
      continue;
    }
    return detail::socket_error_would_block(code) ? ReadOutcome::TimedOut : ReadOutcome::Failed;
  }
}

} // namespace lucent::net

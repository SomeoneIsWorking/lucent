// lucent/byte_stream.h — a connected stream of bytes for the HTTP and WebSocket clients: plain TCP
// here, TLS over it in `lucent::online` (`lucent/p2p/tls_stream.h`).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lucent/ipv4.h"

namespace lucent::net {

enum class ReadOutcome : std::uint8_t { Data, Closed, TimedOut, Failed };

class ByteStream {
public:
  ByteStream() = default;
  ByteStream(const ByteStream &) = delete;
  ByteStream &operator=(const ByteStream &) = delete;
  ByteStream(ByteStream &&) = delete;
  ByteStream &operator=(ByteStream &&) = delete;
  virtual ~ByteStream() = default;

  // False, with `error`, once the stream cannot take them.
  virtual bool send_all(std::string_view bytes, std::string &error) = 0;
  // Appends what one read returns to `buffer`, waiting at most the stream's timeout.
  virtual ReadOutcome read_some(std::string &buffer) = 0;
};

// One TCP connection; each send and read waits at most 5 s.
class TcpStream final : public ByteStream {
public:
  static std::unique_ptr<TcpStream> connect(const Endpoint &server, std::string &error);
  ~TcpStream() override;
  TcpStream(const TcpStream &) = delete;
  TcpStream &operator=(const TcpStream &) = delete;
  TcpStream(TcpStream &&) = delete;
  TcpStream &operator=(TcpStream &&) = delete;

  bool send_all(std::string_view bytes, std::string &error) override;
  ReadOutcome read_some(std::string &buffer) override;
  // The OS socket, for a layer such as TLS that runs over it.
  std::uintptr_t native() const noexcept;

private:
  explicit TcpStream(std::uintptr_t socket) noexcept : socket_{socket} {
  }

  std::uintptr_t socket_;
};

} // namespace lucent::net

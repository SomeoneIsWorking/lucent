#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lucent/byte_stream.h"
#include "lucent/ipv4.h"

// TLS 1.2 or later over a TCP stream, for the HTTP and WebSocket clients: the server's certificate
// is checked against the system's roots and the name dialled.
namespace lucent::p2p {

// The certificates a TLS connection trusts.
struct TlsTrust {
  // The system's roots: OpenSSL's default paths on Linux, the CA files on Android, the
  // Keychain's system roots on macOS, the ROOT store on Windows.
  bool system = true;
  // More roots as concatenated PEM, such as a lab's own.
  std::string extra_pem;
};

class TlsStream final : public net::ByteStream {
public:
  // A connection to `name`:`port`, resolved to its first IPv4 address.
  static std::unique_ptr<TlsStream> connect(std::string_view name, std::uint16_t port,
                                            const TlsTrust &trust, std::string &error);
  // A connection to `at`, whose certificate must name `name`.
  static std::unique_ptr<TlsStream> connect(const net::Endpoint &at, std::string_view name,
                                            const TlsTrust &trust, std::string &error);
  ~TlsStream() override;
  TlsStream(const TlsStream &) = delete;
  TlsStream &operator=(const TlsStream &) = delete;
  TlsStream(TlsStream &&) = delete;
  TlsStream &operator=(TlsStream &&) = delete;

  bool send_all(std::string_view bytes, std::string &error) override;
  net::ReadOutcome read_some(std::string &buffer) override;

private:
  struct Session;
  explicit TlsStream(std::unique_ptr<Session> session) noexcept;

  std::unique_ptr<Session> session_;
};

// The system's roots as concatenated PEM where OpenSSL has no path to them (Android, macOS,
// Windows), for TLS here and for another client such as libcurl; empty on Linux, where OpenSSL's
// defaults find them.
std::string system_roots_pem();

} // namespace lucent::p2p

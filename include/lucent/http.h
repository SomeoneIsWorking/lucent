// lucent/http.h — a bounded loopback HTTP control channel.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lucent::http {

namespace detail {
struct ServerState;
}

struct Request {
  std::string method;
  std::string target;
  std::string body;

  std::string_view path() const noexcept;
  std::string_view query() const noexcept;
};

struct Response {
  int status = 200;
  std::string reason = "OK";
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;

  static Response text(int status, std::string reason, std::string body);
  static Response json(int status, std::string reason, std::string body);
  static Response binary(int status, std::string reason, std::string content_type,
                         std::string body);
};

struct FormField {
  std::string name;
  std::string value;
};

// Decodes application/x-www-form-urlencoded data. Malformed percent escapes are refused and named
// in `error`; an empty field between separators is ignored.
bool parse_form_urlencoded(std::string_view encoded, std::vector<FormField> &fields,
                           std::string &error);

struct ServerOptions {
  std::uint16_t port = 0;
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_body_bytes = 1024 * 1024;
  std::size_t max_connections = 8;
  int backlog = 8;
};

using Handler = std::function<Response(const Request &)>;

// Owns a loopback-only listener. Connections are handled concurrently up to max_connections so a
// long-running probe does not block input or status requests. Each connection carries one request
// and one response; keep-alive and chunked transfer encoding are deliberately outside this control
// channel's scope.
class Server {
public:
  Server(ServerOptions options, Handler handler);
  ~Server();

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  bool start();
  void stop();
  bool running() const noexcept;
  std::uint16_t port() const noexcept;

private:
  std::shared_ptr<detail::ServerState> state_;
};

} // namespace lucent::http

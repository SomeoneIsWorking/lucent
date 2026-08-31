#include "lucent/http.h"

#include "lucent/log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Consumers may build with -fno-exceptions (PCSX2 does). Handler dispatch and
// thread creation degrade to direct calls; without exceptions a failure there
// terminates the process either way.
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define LUCENT_EXCEPTIONS 1
#else
#define LUCENT_EXCEPTIONS 0
#endif

namespace lucent::http {
namespace {

struct ReadResult {
  int status = 400;
  std::string reason = "Bad Request";
  std::string error;
};

void close_socket(int socket) {
  if (socket >= 0)
    close(socket);
}

void set_close_on_exec(int socket) {
  const int flags = fcntl(socket, F_GETFD);
  if (flags >= 0)
    fcntl(socket, F_SETFD, flags | FD_CLOEXEC);
}

void set_socket_timeouts(int socket) {
  constexpr timeval timeout{5, 0};
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

bool send_all(int socket, const void *bytes, std::size_t size) {
  const char *cursor = static_cast<const char *>(bytes);
  while (size != 0) {
    const ssize_t sent = send(socket, cursor, size, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      return false;
    cursor += sent;
    size -= static_cast<std::size_t>(sent);
  }
  return true;
}

bool send_response(int socket, const Response &response) {
  std::error_code error;
  const std::uintmax_t file_size =
      response.file_path.empty() ? 0 : std::filesystem::file_size(response.file_path, error);
  if (!response.file_path.empty() && error)
    return false;
  const std::size_t content_length =
      response.file_path.empty() ? response.body.size() : static_cast<std::size_t>(file_size);
  std::string header = "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason +
                       "\r\nContent-Type: " + response.content_type +
                       "\r\nContent-Length: " + std::to_string(content_length) +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (!send_all(socket, header.data(), header.size()))
    return false;
  if (response.file_path.empty())
    return send_all(socket, response.body.data(), response.body.size());

  std::ifstream file(response.file_path, std::ios::binary);
  if (!file)
    return false;
  char block[64 * 1024];
  while (file.read(block, sizeof(block)) || file.gcount() != 0) {
    if (!send_all(socket, block, static_cast<std::size_t>(file.gcount())))
      return false;
  }
  return !file.bad();
}

Response error_response(int status, std::string reason, std::string message) {
  return Response::text(status, std::move(reason), std::move(message) + "\n");
}

bool ascii_iequals(std::string_view left, std::string_view right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    char a = left[i];
    char b = right[i];
    if (a >= 'A' && a <= 'Z')
      a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = static_cast<char>(b - 'A' + 'a');
    if (a != b)
      return false;
  }
  return true;
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool read_more(int socket, std::string &wire, ReadResult &result) {
  char block[2048];
  ssize_t count = -1;
  do {
    count = recv(socket, block, sizeof(block), 0);
  } while (count < 0 && errno == EINTR);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    result.status = 408;
    result.reason = "Request Timeout";
    result.error = "request timed out";
    return false;
  }
  if (count <= 0) {
    result.error = "request ended before it was complete";
    return false;
  }
  wire.append(block, static_cast<std::size_t>(count));
  return true;
}

bool read_request(int socket, const ServerOptions &options, Request &request, ReadResult &result) {
  std::string wire;
  wire.reserve(std::min<std::size_t>(options.max_header_bytes, 2048));
  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    if (!read_more(socket, wire, result))
      return false;
    header_end = wire.find("\r\n\r\n");
    if (header_end == std::string::npos && wire.size() > options.max_header_bytes) {
      result.status = 431;
      result.reason = "Request Header Fields Too Large";
      result.error = "request headers are too large";
      return false;
    }
  }
  if (header_end + 4 > options.max_header_bytes) {
    result.status = 431;
    result.reason = "Request Header Fields Too Large";
    result.error = "request headers are too large";
    return false;
  }

  const std::size_t first_line_end = wire.find("\r\n");
  const std::size_t method_end = wire.find(' ');
  const std::size_t target_end =
      method_end == std::string::npos ? std::string::npos : wire.find(' ', method_end + 1);
  if (first_line_end == std::string::npos || method_end == std::string::npos ||
      target_end == std::string::npos || target_end >= first_line_end || method_end == 0 ||
      target_end == method_end + 1) {
    result.error = "malformed request line";
    return false;
  }
  const std::string_view version(wire.data() + target_end + 1, first_line_end - target_end - 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    result.status = 505;
    result.reason = "HTTP Version Not Supported";
    result.error = "only HTTP/1.0 and HTTP/1.1 are supported";
    return false;
  }
  request.method.assign(wire.data(), method_end);
  request.target.assign(wire.data() + method_end + 1, target_end - method_end - 1);

  std::size_t content_length = 0;
  bool saw_content_length = false;
  std::size_t line = first_line_end + 2;
  while (line < header_end) {
    const std::size_t end = wire.find("\r\n", line);
    if (end == std::string::npos || end > header_end) {
      result.error = "malformed request headers";
      return false;
    }
    const std::string_view header(wire.data() + line, end - line);
    const std::size_t colon = header.find(':');
    if (colon == std::string_view::npos) {
      result.error = "malformed request header";
      return false;
    }
    const std::string_view name = trim(header.substr(0, colon));
    const std::string_view value = trim(header.substr(colon + 1));
    if (ascii_iequals(name, "Content-Length")) {
      std::size_t parsed = 0;
      const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (value.empty() || converted.ec != std::errc{} ||
          converted.ptr != value.data() + value.size() ||
          (saw_content_length && parsed != content_length)) {
        result.error = "invalid Content-Length";
        return false;
      }
      content_length = parsed;
      saw_content_length = true;
    } else if (ascii_iequals(name, "Transfer-Encoding") && !value.empty()) {
      result.status = 501;
      result.reason = "Not Implemented";
      result.error = "Transfer-Encoding is not supported";
      return false;
    }
    line = end + 2;
  }
  if (content_length > options.max_body_bytes) {
    result.status = 413;
    result.reason = "Content Too Large";
    result.error = "request body is too large";
    return false;
  }

  const std::size_t body_start = header_end + 4;
  while (wire.size() - body_start < content_length) {
    if (!read_more(socket, wire, result))
      return false;
  }
  request.body.assign(wire.data() + body_start, content_length);
  return true;
}

int hex_digit(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

bool decode_form_component(std::string_view encoded, std::string &decoded) {
  decoded.clear();
  decoded.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    if (encoded[index] == '+') {
      decoded.push_back(' ');
    } else if (encoded[index] == '%') {
      if (index + 2 >= encoded.size())
        return false;
      const int high = hex_digit(encoded[index + 1]);
      const int low = hex_digit(encoded[index + 2]);
      if (high < 0 || low < 0)
        return false;
      decoded.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else {
      decoded.push_back(encoded[index]);
    }
  }
  return true;
}

} // namespace

struct detail::ServerState {
  ServerState(ServerOptions server_options, Handler request_handler)
      : options(server_options), handler(std::move(request_handler)) {}

  ServerOptions options;
  Handler handler;
  std::atomic<bool> running{false};
  std::atomic<int> listener{-1};
  std::atomic<std::uint16_t> bound_port{0};
  std::thread accept_thread;
  std::mutex clients_mutex;
  std::condition_variable clients_stopped;
  std::unordered_set<int> clients;
};

namespace {

void finish_client(const std::shared_ptr<detail::ServerState> &state, int client) {
  {
    std::lock_guard lock(state->clients_mutex);
    state->clients.erase(client);
  }
  close_socket(client);
  state->clients_stopped.notify_all();
}

void serve_client(const std::shared_ptr<detail::ServerState> &state, int client) {
  Request request;
  ReadResult result;
  if (!read_request(client, state->options, request, result)) {
    send_response(client, error_response(result.status, result.reason, result.error));
    finish_client(state, client);
    return;
  }

#if LUCENT_EXCEPTIONS
  try {
    if (!send_response(client, state->handler(request)))
      lucent::log(Level::Warn, "http", "response could not be sent");
  } catch (const std::exception &exception) {
    lucent::log(Level::Error, "http", std::string{"request handler threw: "} + exception.what());
    send_response(client, error_response(500, "Internal Server Error", "request handler failed"));
  } catch (...) {
    lucent::log(Level::Error, "http", "request handler threw an unknown exception");
    send_response(client, error_response(500, "Internal Server Error", "request handler failed"));
  }
#else
  send_response(client, state->handler(request));
#endif
  finish_client(state, client);
}

void accept_connections(const std::shared_ptr<detail::ServerState> &state) {
  while (state->running.load(std::memory_order_acquire)) {
    const int listener = state->listener.load(std::memory_order_acquire);
    if (listener < 0)
      break;
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0 && errno == EINTR)
      continue;
    if (client < 0) {
      if (!state->running.load(std::memory_order_acquire))
        break;
      lucent::log(Level::Warn, "http", "accept failed (errno " + std::to_string(errno) + ")");
      continue;
    }
    set_close_on_exec(client);
    set_socket_timeouts(client);

    bool accepted = false;
    {
      std::lock_guard lock(state->clients_mutex);
      if (state->running.load(std::memory_order_acquire) &&
          state->clients.size() < state->options.max_connections) {
        state->clients.insert(client);
        accepted = true;
      }
    }
    if (!accepted) {
      send_response(client, error_response(503, "Service Unavailable", "server is busy"));
      close_socket(client);
      continue;
    }
#if LUCENT_EXCEPTIONS
    try {
      std::thread(serve_client, state, client).detach();
    } catch (const std::system_error &error) {
      lucent::log(Level::Error, "http",
                  std::string{"could not start request worker: "} + error.what());
      send_response(client,
                    error_response(503, "Service Unavailable", "could not start request worker"));
      finish_client(state, client);
    }
#else
    std::thread(serve_client, state, client).detach();
#endif
  }
}

} // namespace

std::string_view Request::path() const noexcept {
  const std::size_t separator = target.find('?');
  return std::string_view(target).substr(0, separator);
}

std::string_view Request::query() const noexcept {
  const std::size_t separator = target.find('?');
  return separator == std::string::npos ? std::string_view{}
                                        : std::string_view(target).substr(separator + 1);
}

Response Response::text(int status, std::string reason, std::string body) {
  return {status, std::move(reason), "text/plain; charset=utf-8", std::move(body), {}};
}

Response Response::json(int status, std::string reason, std::string body) {
  return {status, std::move(reason), "application/json", std::move(body), {}};
}

Response Response::binary(int status, std::string reason, std::string content_type,
                          std::string body) {
  return {status, std::move(reason), std::move(content_type), std::move(body), {}};
}

Response Response::file(int status, std::string reason, std::string content_type,
                        std::string path) {
  Response response;
  response.status = status;
  response.reason = std::move(reason);
  response.content_type = std::move(content_type);
  response.file_path = std::move(path);
  return response;
}

bool parse_form_urlencoded(std::string_view encoded, std::vector<FormField> &fields,
                           std::string &error) {
  while (!encoded.empty()) {
    const std::size_t separator = encoded.find('&');
    const std::string_view item = encoded.substr(0, separator);
    encoded =
        separator == std::string_view::npos ? std::string_view{} : encoded.substr(separator + 1);
    if (item.empty())
      continue;
    const std::size_t equals = item.find('=');
    FormField field;
    if (!decode_form_component(item.substr(0, equals), field.name) ||
        !decode_form_component(equals == std::string_view::npos ? std::string_view{}
                                                                : item.substr(equals + 1),
                               field.value)) {
      error = "invalid percent-encoding in form data";
      return false;
    }
    fields.push_back(std::move(field));
  }
  return true;
}

Server::Server(ServerOptions options, Handler handler)
    : state_(std::make_shared<detail::ServerState>(options, std::move(handler))) {
}

Server::~Server() {
  stop();
}

bool Server::start() {
  if (state_->running.load(std::memory_order_acquire))
    return true;
  if (!state_->handler || state_->options.max_header_bytes < 16 ||
      state_->options.max_connections == 0 || state_->options.backlog <= 0) {
    lucent::log(Level::Error, "http", "refusing invalid server options");
    return false;
  }

  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    lucent::log(Level::Error, "http",
                "could not create listener (errno " + std::to_string(errno) + ")");
    return false;
  }
  set_close_on_exec(listener);
  const int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(state_->options.port);
  const bool local_network = state_->options.listen_scope == ListenScope::LocalNetwork;
  address.sin_addr.s_addr = htonl(local_network ? INADDR_ANY : INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
      listen(listener, state_->options.backlog) != 0) {
    lucent::log(Level::Error, "http",
                "could not bind " + std::string(local_network ? "local-network" : "loopback") +
                    " listener on port " + std::to_string(state_->options.port) + " (errno " +
                    std::to_string(errno) + ")");
    close_socket(listener);
    return false;
  }

  socklen_t address_size = sizeof(address);
  if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_size) != 0) {
    lucent::log(Level::Error, "http",
                "could not read bound listener address (errno " + std::to_string(errno) + ")");
    close_socket(listener);
    return false;
  }
  state_->listener.store(listener, std::memory_order_release);
  state_->bound_port.store(ntohs(address.sin_port), std::memory_order_release);
  state_->running.store(true, std::memory_order_release);
#if LUCENT_EXCEPTIONS
  try {
    state_->accept_thread = std::thread(accept_connections, state_);
  } catch (const std::system_error &error) {
    state_->running.store(false, std::memory_order_release);
    state_->listener.store(-1, std::memory_order_release);
    state_->bound_port.store(0, std::memory_order_release);
    close_socket(listener);
    lucent::log(Level::Error, "http",
                std::string{"could not start listener thread: "} + error.what());
    return false;
  }
#else
  state_->accept_thread = std::thread(accept_connections, state_);
#endif
  lucent::log(Level::Info, "http",
              std::string(local_network ? "local-network" : "loopback") + " server listening on " +
                  (local_network ? "all IPv4 interfaces:" : "http://127.0.0.1:") +
                  std::to_string(port()));
  return true;
}

void Server::stop() {
  if (!state_->running.exchange(false, std::memory_order_acq_rel))
    return;
  const int listener = state_->listener.exchange(-1, std::memory_order_acq_rel);
  if (listener >= 0) {
    shutdown(listener, SHUT_RDWR);
    close_socket(listener);
  }
  if (state_->accept_thread.joinable())
    state_->accept_thread.join();

  std::unique_lock lock(state_->clients_mutex);
  for (const int client : state_->clients)
    shutdown(client, SHUT_RDWR);
  state_->clients_stopped.wait(lock, [this] { return state_->clients.empty(); });
  state_->bound_port.store(0, std::memory_order_release);
}

bool Server::running() const noexcept {
  return state_->running.load(std::memory_order_acquire);
}

std::uint16_t Server::port() const noexcept {
  return state_->bound_port.load(std::memory_order_acquire);
}

} // namespace lucent::http

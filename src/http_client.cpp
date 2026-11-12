#include "lucent/http_client.h"

#include "lucent/byte_stream.h"

#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <utility>

namespace lucent::http {
namespace {

using net::ReadOutcome;

constexpr std::size_t kMaxHeaderBytes = std::size_t{16} * 1024;

std::string lowercase(std::string_view text) {
  std::string out;
  for (const char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

struct Head {
  int status = 0;
  // Header names lower-cased, one "name: value" line each, for lookups.
  std::string headers;
  std::string rest;
};

std::optional<std::string> header_value(const Head &head, std::string_view name) {
  const std::string key = "\n" + lowercase(name) + ":";
  const std::string lowered = lowercase(head.headers);
  const std::size_t at = lowered.find(key);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  std::size_t begin = at + key.size();
  std::size_t end = head.headers.find("\r\n", begin);
  while (begin < end && head.headers[begin] == ' ') {
    ++begin;
  }
  while (end > begin && head.headers[end - 1] == ' ') {
    --end;
  }
  return head.headers.substr(begin, end - begin);
}

// Reads the status line and headers; bytes past them are left in `head.rest`.
bool read_head(net::ByteStream &stream, Head &head, std::string &error) {
  std::string buffer;
  std::size_t end = std::string::npos;
  while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
    if (buffer.size() > kMaxHeaderBytes) {
      error = "response headers too large";
      return false;
    }
    if (const ReadOutcome outcome = stream.read_some(buffer); outcome != ReadOutcome::Data) {
      error = net::describe(outcome, "connection closed before the response headers");
      return false;
    }
  }
  const std::size_t line_end = buffer.find("\r\n");
  const std::string_view status_line{buffer.data(), line_end};
  const std::size_t space = status_line.find(' ');
  if (!status_line.starts_with("HTTP/1.") || space == std::string_view::npos ||
      std::from_chars(status_line.data() + space + 1, status_line.data() + status_line.size(),
                      head.status)
              .ec != std::errc{}) {
    error = "malformed status line";
    return false;
  }
  head.headers = buffer.substr(line_end, end + 2 - line_end);
  head.rest = buffer.substr(end + 4);
  return true;
}

enum class Chunked : std::uint8_t { Complete, Partial, Malformed };

// RFC 9112 7.1: the body of a chunked message read so far, trailers skipped.
Chunked dechunk(std::string_view raw, std::size_t max_body, std::string &body) {
  body.clear();
  std::size_t at = 0;
  while (true) {
    const std::size_t line_end = raw.find("\r\n", at);
    if (line_end == std::string_view::npos) {
      return raw.size() - at > 32 ? Chunked::Malformed : Chunked::Partial;
    }
    std::string_view size_line = raw.substr(at, line_end - at);
    size_line = size_line.substr(0, size_line.find(';'));
    while (!size_line.empty() && (size_line.back() == ' ' || size_line.back() == '\t')) {
      size_line.remove_suffix(1);
    }
    std::size_t size = 0;
    const auto parsed =
        std::from_chars(size_line.data(), size_line.data() + size_line.size(), size, 16);
    if (size_line.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != size_line.data() + size_line.size() || size > max_body - body.size()) {
      return Chunked::Malformed;
    }
    at = line_end + 2;
    if (size == 0) {
      // Trailer lines, then the empty line that ends the message.
      while (true) {
        const std::size_t trailer_end = raw.find("\r\n", at);
        if (trailer_end == std::string_view::npos) {
          return Chunked::Partial;
        }
        if (trailer_end == at) {
          return Chunked::Complete;
        }
        at = trailer_end + 2;
      }
    }
    if (raw.size() < at + size + 2) {
      return Chunked::Partial;
    }
    if (raw.substr(at + size, 2) != "\r\n") {
      return Chunked::Malformed;
    }
    body.append(raw.substr(at, size));
    at += size + 2;
  }
}

// Reads a chunked body to its last chunk.
std::optional<std::string> read_chunked(net::ByteStream &connection, std::string raw,
                                        std::size_t max_body, std::string &error) {
  std::string body;
  while (true) {
    switch (dechunk(raw, max_body, body)) {
    case Chunked::Complete:
      return body;
    case Chunked::Malformed:
      error = "malformed or oversized chunked body";
      return std::nullopt;
    case Chunked::Partial:
      break;
    }
    if (const ReadOutcome outcome = connection.read_some(raw); outcome != ReadOutcome::Data) {
      error = net::describe(outcome, "connection closed before the last chunk");
      return std::nullopt;
    }
  }
}

} // namespace

namespace {

constexpr net::Endpoint loopback(std::uint16_t port) {
  return {0x7F000001u, port};
}

} // namespace

bool fetch(const net::Endpoint &server, const FetchRequest &sent, FetchResult &result,
           std::string &error) {
  const std::unique_ptr<net::TcpStream> connection = net::TcpStream::connect(server, error);
  return connection &&
         fetch(*connection, net::format_ipv4(server.address) + ":" + std::to_string(server.port),
               sent, result, error);
}

bool fetch(net::ByteStream &connection, std::string_view host, const FetchRequest &sent,
           FetchResult &result, std::string &error) {
  std::string request = std::string{sent.method} + " " + std::string{sent.target} +
                        " HTTP/1.1\r\nHost: " + std::string{host} + "\r\nConnection: close\r\n" +
                        std::string{sent.headers};
  if (sent.body) {
    request += "Content-Length: " + std::to_string(sent.body->size()) + "\r\n\r\n";
    request += *sent.body;
  } else {
    request += "\r\n";
  }
  Head head;
  if (!connection.send_all(request, error) || !read_head(connection, head, error)) {
    return false;
  }
  if (const auto encoding = header_value(head, "Transfer-Encoding")) {
    if (lowercase(*encoding) != "chunked") {
      error = "unsupported transfer encoding " + *encoding;
      return false;
    }
    std::optional<std::string> body =
        read_chunked(connection, std::move(head.rest), sent.max_body, error);
    if (!body) {
      return false;
    }
    result.status = head.status;
    result.body = std::move(*body);
    return true;
  }
  std::optional<std::size_t> length;
  if (const auto value = header_value(head, "Content-Length")) {
    std::size_t parsed = 0;
    if (std::from_chars(value->data(), value->data() + value->size(), parsed).ec != std::errc{} ||
        parsed > sent.max_body) {
      error = "bad Content-Length " + *value;
      return false;
    }
    length = parsed;
  }
  std::string response = std::move(head.rest);
  while (!length || response.size() < *length) {
    if (response.size() > sent.max_body) {
      error = "response body too large";
      return false;
    }
    const ReadOutcome outcome = connection.read_some(response);
    if (outcome == ReadOutcome::Closed && !length) {
      break;
    }
    if (outcome != ReadOutcome::Data) {
      error = net::describe(outcome, "connection closed before the response body");
      return false;
    }
  }
  if (length) {
    response.resize(*length);
  }
  result.status = head.status;
  result.body = std::move(response);
  return true;
}

bool get_loopback(std::uint16_t port, std::string_view target, FetchResult &result,
                  std::string &error) {
  return fetch(loopback(port), FetchRequest{"GET", target, {}, std::nullopt}, result, error);
}

bool post_loopback(std::uint16_t port, std::string_view target, std::string_view body,
                   FetchResult &result, std::string &error) {
  return fetch(loopback(port), FetchRequest{"POST", target, {}, body}, result, error);
}

WebSocketClient::~WebSocketClient() {
  close();
}

bool WebSocketClient::connected() const noexcept {
  return socket_ && socket_->open();
}

void WebSocketClient::close() noexcept {
  socket_.reset();
  stream_.reset();
}

bool WebSocketClient::connect_loopback(std::uint16_t port, std::string_view path,
                                       std::string &error) {
  std::unique_ptr<net::TcpStream> stream = net::TcpStream::connect(loopback(port), error);
  if (!stream) {
    close();
    return false;
  }
  return connect(std::move(stream), "127.0.0.1:" + std::to_string(port), path, error);
}

bool WebSocketClient::connect(std::unique_ptr<net::ByteStream> stream, std::string_view host,
                              std::string_view path, std::string &error) {
  close();
  const std::string key = websocket_key();
  const std::string request = "GET " + std::string{path} +
                              " HTTP/1.1\r\nHost: " + std::string{host} +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: " +
                              key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  Head head;
  if (!stream->send_all(request, error) || !read_head(*stream, head, error)) {
    return false;
  }
  if (head.status != 101) {
    error = "upgrade refused with status " + std::to_string(head.status);
    return false;
  }
  const auto upgrade = header_value(head, "Upgrade");
  if (!upgrade || lowercase(*upgrade) != "websocket") {
    error = "response does not upgrade to websocket";
    return false;
  }
  if (header_value(head, "Sec-WebSocket-Accept") != websocket_accept(key)) {
    error = "Sec-WebSocket-Accept does not match the key";
    return false;
  }
  stream_ = std::move(stream);
  socket_.emplace(*stream_, std::move(head.rest), WebSocketRole::Client);
  return true;
}

bool WebSocketClient::send_text(std::string_view text, std::string &error) {
  if (!socket_ || !socket_->open()) {
    error = "not connected";
    return false;
  }
  return socket_->send_text(text, error);
}

std::optional<std::string> WebSocketClient::receive_text(std::string &error) {
  if (!socket_ || !socket_->open()) {
    error = "not connected";
    return std::nullopt;
  }
  return socket_->receive_text(error);
}

} // namespace lucent::http

#include "lucent/http_client.h"

#include "http_socket.h"

#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <optional>
#include <random>
#include <utility>

namespace lucent::http {
namespace {

using detail::Socket;

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxHeaderBytes = std::size_t{16} * 1024;
constexpr std::size_t kMaxBodyBytes = std::size_t{16} * 1024 * 1024;

constexpr std::uint8_t kFin = 0x80;
constexpr std::uint8_t kMask = 0x80;
constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

std::array<std::uint8_t, 20> sha1(std::string_view text) {
  std::array<std::uint32_t, 5> h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::string padded{text};
  padded.push_back(static_cast<char>(0x80));
  while (padded.size() % 64 != 56) {
    padded.push_back('\0');
  }
  const std::uint64_t bits = static_cast<std::uint64_t>(text.size()) * 8;
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<char>((bits >> shift) & 0xffu));
  }
  for (std::size_t block = 0; block < padded.size(); block += 64) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      for (std::size_t b = 0; b < 4; ++b) {
        w[i] = (w[i] << 8) | static_cast<std::uint8_t>(padded[block + i * 4 + b]);
      }
    }
    for (std::size_t i = 16; i < 80; ++i) {
      w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    auto [a, b, c, d, e] = h;
    for (std::size_t i = 0; i < 80; ++i) {
      std::uint32_t f = 0;
      std::uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const std::uint32_t next = std::rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = std::rotl(b, 30);
      b = a;
      a = next;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  std::array<std::uint8_t, 20> digest{};
  for (std::size_t i = 0; i < 20; ++i) {
    digest[i] = static_cast<std::uint8_t>((h[i / 4] >> (24 - 8 * (i % 4))) & 0xffu);
  }
  return digest;
}

std::string base64(const std::uint8_t *bytes, std::size_t size) {
  constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < size; i += 3) {
    const std::uint32_t n = (std::uint32_t{bytes[i]} << 16) |
                            (i + 1 < size ? std::uint32_t{bytes[i + 1]} << 8 : 0u) |
                            (i + 2 < size ? std::uint32_t{bytes[i + 2]} : 0u);
    out.push_back(alphabet[(n >> 18) & 63u]);
    out.push_back(alphabet[(n >> 12) & 63u]);
    out.push_back(i + 1 < size ? alphabet[(n >> 6) & 63u] : '=');
    out.push_back(i + 2 < size ? alphabet[n & 63u] : '=');
  }
  return out;
}

std::string lowercase(std::string_view text) {
  std::string out;
  for (const char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

enum class ReadOutcome : std::uint8_t { Data, Closed, TimedOut, Failed };

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

class Connection {
public:
  ~Connection() {
    detail::close_socket(socket_);
  }
  Connection() = default;
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  Connection(Connection &&) = delete;
  Connection &operator=(Connection &&) = delete;

  bool open(std::uint16_t port, std::string &error) {
    if (!detail::initialize_socket_runtime()) {
      error = "socket runtime unavailable";
      return false;
    }
    socket_ = detail::create_tcp_socket();
    if (detail::is_invalid_socket(socket_)) {
      error = "cannot create a socket";
      return false;
    }
    detail::set_close_on_exec(socket_);
    detail::set_socket_timeouts(socket_);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(detail::native_socket(socket_), reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) != 0) {
      error = "cannot connect to 127.0.0.1:" + std::to_string(port);
      return false;
    }
    return true;
  }

  bool send_all(std::string_view bytes, std::string &error) const {
    if (detail::is_invalid_socket(socket_)) {
      error = "not connected";
      return false;
    }
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

  // Reads the status line and headers; bytes past them are left in `head.rest`.
  bool read_head(Head &head, std::string &error) const {
    std::string buffer;
    std::size_t end = std::string::npos;
    while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
      if (buffer.size() > kMaxHeaderBytes) {
        error = "response headers too large";
        return false;
      }
      if (const ReadOutcome outcome = read_some(buffer); outcome != ReadOutcome::Data) {
        error = describe(outcome, "connection closed before the response headers");
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

  // Appends what one receive returns.
  ReadOutcome read_some(std::string &buffer) const {
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

  Socket release() noexcept {
    return std::exchange(socket_, detail::kInvalidSocket);
  }

private:
  Socket socket_ = detail::kInvalidSocket;
};

} // namespace

bool get_loopback(std::uint16_t port, std::string_view target, FetchResult &result,
                  std::string &error) {
  Connection connection;
  if (!connection.open(port, error)) {
    return false;
  }
  const std::string request = "GET " + std::string{target} +
                              " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                              "\r\nConnection: close\r\n\r\n";
  Head head;
  if (!connection.send_all(request, error) || !connection.read_head(head, error)) {
    return false;
  }
  if (const auto encoding = header_value(head, "Transfer-Encoding");
      encoding && lowercase(*encoding) != "identity") {
    error = "unsupported transfer encoding " + *encoding;
    return false;
  }
  std::optional<std::size_t> length;
  if (const auto value = header_value(head, "Content-Length")) {
    std::size_t parsed = 0;
    if (std::from_chars(value->data(), value->data() + value->size(), parsed).ec != std::errc{} ||
        parsed > kMaxBodyBytes) {
      error = "bad Content-Length " + *value;
      return false;
    }
    length = parsed;
  }
  std::string body = std::move(head.rest);
  while (!length || body.size() < *length) {
    if (body.size() > kMaxBodyBytes) {
      error = "response body too large";
      return false;
    }
    const ReadOutcome outcome = connection.read_some(body);
    if (outcome == ReadOutcome::Closed && !length) {
      break;
    }
    if (outcome != ReadOutcome::Data) {
      error = describe(outcome, "connection closed before the response body");
      return false;
    }
  }
  if (length) {
    body.resize(*length);
  }
  result.status = head.status;
  result.body = std::move(body);
  return true;
}

std::string websocket_accept(std::string_view key) {
  const std::array<std::uint8_t, 20> digest = sha1(std::string{key} + std::string{kWebSocketGuid});
  return base64(digest.data(), digest.size());
}

WebSocketClient::~WebSocketClient() {
  close();
}

bool WebSocketClient::connected() const noexcept {
  return !detail::is_invalid_socket(socket_);
}

void WebSocketClient::close() noexcept {
  detail::close_socket(socket_);
  socket_ = detail::kInvalidSocket;
  pending_.clear();
}

bool WebSocketClient::fail(std::string &error, std::string reason) {
  close();
  error = std::move(reason);
  return false;
}

bool WebSocketClient::connect_loopback(std::uint16_t port, std::string_view path,
                                       std::string &error) {
  close();
  Connection connection;
  if (!connection.open(port, error)) {
    return false;
  }
  std::random_device random;
  std::array<std::uint8_t, 16> nonce{};
  for (std::uint8_t &byte : nonce) {
    byte = static_cast<std::uint8_t>(random() & 0xffu);
  }
  const std::string key = base64(nonce.data(), nonce.size());
  const std::string request = "GET " + std::string{path} +
                              " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: " +
                              key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  Head head;
  if (!connection.send_all(request, error) || !connection.read_head(head, error)) {
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
  socket_ = connection.release();
  pending_ = std::move(head.rest);
  return true;
}

bool WebSocketClient::send_frame(std::uint8_t opcode, std::string_view payload,
                                 std::string &error) {
  if (!connected()) {
    error = "not connected";
    return false;
  }
  std::string frame;
  frame.push_back(static_cast<char>(kFin | opcode));
  const std::size_t size = payload.size();
  if (size < 126) {
    frame.push_back(static_cast<char>(kMask | size));
  } else if (size <= 0xffff) {
    frame.push_back(static_cast<char>(kMask | 126));
    frame.push_back(static_cast<char>((size >> 8) & 0xffu));
    frame.push_back(static_cast<char>(size & 0xffu));
  } else {
    frame.push_back(static_cast<char>(kMask | 127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> shift) & 0xffu));
    }
  }
  std::random_device random;
  std::array<char, 4> mask{};
  for (char &byte : mask) {
    byte = static_cast<char>(random() & 0xffu);
  }
  frame.append(mask.data(), mask.size());
  for (std::size_t i = 0; i < size; ++i) {
    frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
  }
  std::string_view bytes = frame;
  while (!bytes.empty()) {
    const std::ptrdiff_t sent = detail::send_bytes(socket_, bytes.data(), bytes.size());
    if (sent < 0 && detail::socket_error_interrupted(detail::last_socket_error())) {
      continue;
    }
    if (sent <= 0) {
      return fail(error, "send failed");
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

bool WebSocketClient::send_text(std::string_view text, std::string &error) {
  return send_frame(kOpText, text, error);
}

bool WebSocketClient::read_exact(char *bytes, std::size_t size, std::string &error) {
  while (pending_.size() < size) {
    std::array<char, 4096> chunk{};
    const std::ptrdiff_t received = detail::receive_bytes(socket_, chunk.data(), chunk.size());
    if (received > 0) {
      pending_.append(chunk.data(), static_cast<std::size_t>(received));
      continue;
    }
    if (received == 0) {
      return fail(error, "connection closed");
    }
    const int code = detail::last_socket_error();
    if (detail::socket_error_interrupted(code)) {
      continue;
    }
    return fail(error, detail::socket_error_would_block(code) ? "timed out" : "receive failed");
  }
  std::memcpy(bytes, pending_.data(), size);
  pending_.erase(0, size);
  return true;
}

std::optional<std::string> WebSocketClient::receive_text(std::string &error) {
  if (!connected()) {
    error = "not connected";
    return std::nullopt;
  }
  std::string message;
  bool in_message = false;
  for (;;) {
    std::array<char, 2> header{};
    if (!read_exact(header.data(), header.size(), error)) {
      return std::nullopt;
    }
    const auto first = static_cast<std::uint8_t>(header[0]);
    const auto second = static_cast<std::uint8_t>(header[1]);
    const std::uint8_t opcode = first & 0x0fu;
    std::uint64_t size = second & 0x7fu;
    if (size >= 126) {
      std::array<char, 8> extended{};
      const std::size_t width = size == 126 ? 2 : 8;
      if (!read_exact(extended.data(), width, error)) {
        return std::nullopt;
      }
      size = 0;
      for (std::size_t i = 0; i < width; ++i) {
        size = (size << 8) | static_cast<std::uint8_t>(extended[i]);
      }
    }
    if (message.size() + size > kMaxMessageBytes) {
      fail(error, "message too large");
      return std::nullopt;
    }
    std::array<char, 4> mask{};
    const bool masked = (second & kMask) != 0;
    if (masked && !read_exact(mask.data(), mask.size(), error)) {
      return std::nullopt;
    }
    std::string payload(static_cast<std::size_t>(size), '\0');
    if (!read_exact(payload.data(), payload.size(), error)) {
      return std::nullopt;
    }
    if (masked) {
      for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
      }
    }
    const bool final_frame = (first & kFin) != 0;
    switch (opcode) {
    case kOpPing:
      if (!send_frame(kOpPong, payload, error)) {
        return std::nullopt;
      }
      continue;
    case kOpPong:
      continue;
    case kOpClose: {
      std::string ignored;
      send_frame(kOpClose, {}, ignored);
      fail(error, "closed by the server");
      return std::nullopt;
    }
    case kOpText:
      if (in_message) {
        fail(error, "text frame inside a fragmented message");
        return std::nullopt;
      }
      in_message = true;
      break;
    case kOpContinuation:
      if (!in_message) {
        fail(error, "continuation without a message");
        return std::nullopt;
      }
      break;
    default:
      fail(error, "unsupported opcode " + std::to_string(opcode));
      return std::nullopt;
    }
    message += payload;
    if (final_frame) {
      return message;
    }
  }
}

} // namespace lucent::http

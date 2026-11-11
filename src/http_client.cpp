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

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaxHeaderBytes = std::size_t{16} * 1024;

constexpr std::uint8_t kFin = 0x80;
constexpr std::uint8_t kMask = 0x80;
constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText = 0x1;
constexpr std::uint8_t kOpClose = 0x8;
constexpr std::uint8_t kOpPing = 0x9;
constexpr std::uint8_t kOpPong = 0xA;

// RFC 6455 5.3: payload byte i is XORed with mask byte i % 4, both ways.
void apply_mask(std::span<char> payload, const std::array<char, 4> &mask) {
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^
                                   static_cast<std::uint8_t>(mask[i % 4]));
  }
}

std::array<std::uint8_t, 20> sha1(std::string_view text) {
  std::array<std::uint32_t, 5> h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::string padded{text};
  padded.push_back(static_cast<char>(0x80));
  while (padded.size() % 64 != 56) {
    padded.push_back('\0');
  }
  const std::uint64_t bits = static_cast<std::uint64_t>(text.size()) * 8;
  for (std::size_t byte = 8; byte-- > 0;) {
    padded.push_back(static_cast<char>((bits >> (byte * 8U)) & 0xffU));
  }
  for (std::size_t block = 0; block < padded.size(); block += 64) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      for (std::size_t b = 0; b < 4; ++b) {
        w[i] = (w[i] << 8U) | static_cast<std::uint8_t>(padded[block + i * 4 + b]);
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
    const std::uint32_t n = (std::uint32_t{bytes[i]} << 16U) |
                            (i + 1 < size ? std::uint32_t{bytes[i + 1]} << 8U : 0U) |
                            (i + 2 < size ? std::uint32_t{bytes[i + 2]} : 0u);
    out.push_back(alphabet[(n >> 18U) & 63U]);
    out.push_back(alphabet[(n >> 12U) & 63U]);
    out.push_back(i + 1 < size ? alphabet[(n >> 6U) & 63U] : '=');
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
      error = describe(outcome, "connection closed before the last chunk");
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
      error = describe(outcome, "connection closed before the response body");
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

std::string websocket_accept(std::string_view key) {
  const std::array<std::uint8_t, 20> digest = sha1(std::string{key} + std::string{kWebSocketGuid});
  return base64(digest.data(), digest.size());
}

WebSocketClient::~WebSocketClient() {
  close();
}

bool WebSocketClient::connected() const noexcept {
  return stream_ != nullptr;
}

void WebSocketClient::close() noexcept {
  stream_.reset();
  pending_.clear();
}

bool WebSocketClient::fail(std::string &error, std::string reason) {
  close();
  error = std::move(reason);
  return false;
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
  std::random_device random;
  std::array<std::uint8_t, 16> nonce{};
  for (std::uint8_t &byte : nonce) {
    byte = static_cast<std::uint8_t>(random() & 0xffu);
  }
  const std::string key = base64(nonce.data(), nonce.size());
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
    frame.push_back(static_cast<char>(kMask | 126U));
    frame.push_back(static_cast<char>((size >> 8U) & 0xffU));
    frame.push_back(static_cast<char>(size & 0xffu));
  } else {
    frame.push_back(static_cast<char>(kMask | 127U));
    for (std::size_t byte = 8; byte-- > 0;) {
      frame.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> (byte * 8U)) & 0xffU));
    }
  }
  std::random_device random;
  std::array<char, 4> mask{};
  for (char &byte : mask) {
    byte = static_cast<char>(random() & 0xffu);
  }
  frame.append(mask.data(), mask.size());
  frame.append(payload);
  apply_mask(std::span(frame).subspan(frame.size() - size), mask);
  std::string reason;
  if (!stream_->send_all(frame, reason)) {
    return fail(error, reason);
  }
  return true;
}

bool WebSocketClient::send_text(std::string_view text, std::string &error) {
  return send_frame(kOpText, text, error);
}

bool WebSocketClient::read_exact(char *bytes, std::size_t size, std::string &error) {
  while (pending_.size() < size) {
    if (const ReadOutcome outcome = stream_->read_some(pending_); outcome != ReadOutcome::Data) {
      return fail(error, describe(outcome, "connection closed"));
    }
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
        size = (size << 8U) | static_cast<std::uint8_t>(extended[i]);
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
      apply_mask(payload, mask);
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

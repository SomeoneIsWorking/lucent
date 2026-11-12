#include "lucent/websocket.h"

#include <array>
#include <bit>
#include <cstring>
#include <random>
#include <span>
#include <utility>

namespace lucent::http {
namespace {

using net::ReadOutcome;

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
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

std::string header_lower(std::string_view text) {
  std::string out;
  for (const char c : text) {
    out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

} // namespace

std::string websocket_accept(std::string_view key) {
  const std::array<std::uint8_t, 20> digest = sha1(std::string{key} + std::string{kWebSocketGuid});
  return base64(digest.data(), digest.size());
}

std::string websocket_key() {
  std::random_device random;
  std::array<std::uint8_t, 16> nonce{};
  for (std::uint8_t &byte : nonce) {
    byte = static_cast<std::uint8_t>(random() & 0xffu);
  }
  return base64(nonce.data(), nonce.size());
}

WebSocket::WebSocket(net::ByteStream &stream, std::string pending, WebSocketRole role)
    : stream_{stream}, pending_{std::move(pending)}, role_{role} {
}

bool WebSocket::fail(std::string &error, std::string reason) {
  open_ = false;
  pending_.clear();
  error = std::move(reason);
  return false;
}

bool WebSocket::send_frame(std::uint8_t opcode, std::string_view payload, std::string &error) {
  if (!open_) {
    error = "not connected";
    return false;
  }
  const std::uint8_t masked = role_ == WebSocketRole::Client ? kMask : 0;
  std::string frame;
  frame.push_back(static_cast<char>(kFin | opcode));
  const std::size_t size = payload.size();
  if (size < 126) {
    frame.push_back(static_cast<char>(masked | size));
  } else if (size <= 0xffff) {
    frame.push_back(static_cast<char>(masked | 126U));
    frame.push_back(static_cast<char>((size >> 8U) & 0xffU));
    frame.push_back(static_cast<char>(size & 0xffu));
  } else {
    frame.push_back(static_cast<char>(masked | 127U));
    for (std::size_t byte = 8; byte-- > 0;) {
      frame.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> (byte * 8U)) & 0xffU));
    }
  }
  if (masked != 0) {
    std::random_device random;
    std::array<char, 4> mask{};
    for (char &byte : mask) {
      byte = static_cast<char>(random() & 0xffu);
    }
    frame.append(mask.data(), mask.size());
    frame.append(payload);
    apply_mask(std::span(frame).subspan(frame.size() - size), mask);
  } else {
    frame.append(payload);
  }
  std::string reason;
  if (!stream_.send_all(frame, reason)) {
    return fail(error, reason);
  }
  return true;
}

bool WebSocket::send_text(std::string_view text, std::string &error) {
  return send_frame(kOpText, text, error);
}

bool WebSocket::read_exact(char *bytes, std::size_t size, std::string &error) {
  while (pending_.size() < size) {
    if (const ReadOutcome outcome = stream_.read_some(pending_); outcome != ReadOutcome::Data) {
      return fail(error, net::describe(outcome, "connection closed"));
    }
  }
  std::memcpy(bytes, pending_.data(), size);
  pending_.erase(0, size);
  return true;
}

std::optional<std::string> WebSocket::receive_text(std::string &error) {
  if (!open_) {
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
    const bool masked = (second & kMask) != 0;
    // RFC 6455 5.1: a server takes only masked frames, a client only unmasked ones.
    if (masked != (role_ == WebSocketRole::Server)) {
      fail(error, masked ? "a masked frame from the server" : "an unmasked frame from the client");
      return std::nullopt;
    }
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
      fail(error, "closed by the other end");
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

Response websocket_upgrade(const Request &request, std::function<void(WebSocket &)> serve) {
  const std::optional<std::string_view> upgrade = request.header("Upgrade");
  const std::optional<std::string_view> key = request.header("Sec-WebSocket-Key");
  const std::optional<std::string_view> version = request.header("Sec-WebSocket-Version");
  if (request.method != "GET" || !upgrade || header_lower(*upgrade) != "websocket" || !key ||
      key->empty() || version != "13") {
    return Response::text(400, "Bad Request", "a WebSocket upgrade is needed\n");
  }
  Response response;
  response.status = 101;
  response.reason = "Switching Protocols";
  response.headers = {{"Upgrade", "websocket"},
                      {"Connection", "Upgrade"},
                      {"Sec-WebSocket-Accept", websocket_accept(*key)}};
  response.takeover = [serve = std::move(serve)](net::ByteStream &stream) {
    WebSocket socket{stream, {}, WebSocketRole::Server};
    serve(socket);
  };
  return response;
}

} // namespace lucent::http

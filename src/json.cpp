#include "lucent/json.h"

#include "lucent/text.h"

#include <charconv>

namespace lucent::json {
namespace {

class Reader {
public:
  explicit Reader(std::string_view text) : text_{text} {
  }

  std::optional<Value> document(std::string &error) {
    std::optional<Value> value = read_value(0);
    skip_space();
    if (value && at_ != text_.size()) {
      fail("text after the value");
      value.reset();
    }
    if (!value) {
      error = "JSON: " + reason_ + " at byte " + std::to_string(failed_at_);
    }
    return value;
  }

private:
  std::optional<Value> read_value(std::size_t depth) {
    skip_space();
    if (at_ == text_.size()) {
      return fail("end of text where a value goes");
    }
    switch (text_[at_]) {
    case '{':
      return read_object(depth);
    case '[':
      return read_array(depth);
    case '"': {
      std::optional<std::string> text = read_string();
      return text ? std::optional<Value>{Value{std::move(*text)}} : std::nullopt;
    }
    case 't':
      return read_word("true", Value{true});
    case 'f':
      return read_word("false", Value{false});
    case 'n':
      return read_word("null", Value{});
    default:
      return read_number();
    }
  }

  std::optional<Value> read_object(std::size_t depth) {
    if (depth == kMaxDepth) {
      return fail("nesting deeper than " + std::to_string(kMaxDepth));
    }
    ++at_;
    Object members;
    skip_space();
    if (take('}')) {
      return Value{std::move(members)};
    }
    for (;;) {
      skip_space();
      if (at_ == text_.size() || text_[at_] != '"') {
        return fail("a member name expected");
      }
      std::optional<std::string> key = read_string();
      if (!key) {
        return std::nullopt;
      }
      skip_space();
      if (!take(':')) {
        return fail("':' expected");
      }
      std::optional<Value> value = read_value(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      members.emplace_back(std::move(*key), std::move(*value));
      skip_space();
      if (take('}')) {
        return Value{std::move(members)};
      }
      if (!take(',')) {
        return fail("',' or '}' expected");
      }
    }
  }

  std::optional<Value> read_array(std::size_t depth) {
    if (depth == kMaxDepth) {
      return fail("nesting deeper than " + std::to_string(kMaxDepth));
    }
    ++at_;
    Array items;
    skip_space();
    if (take(']')) {
      return Value{std::move(items)};
    }
    for (;;) {
      std::optional<Value> value = read_value(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      items.push_back(std::move(*value));
      skip_space();
      if (take(']')) {
        return Value{std::move(items)};
      }
      if (!take(',')) {
        return fail("',' or ']' expected");
      }
    }
  }

  std::optional<Value> read_word(std::string_view word, Value value) {
    if (text_.substr(at_, word.size()) != word) {
      return fail("unknown word");
    }
    at_ += word.size();
    return value;
  }

  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  std::optional<Value> read_number() {
    const std::size_t start = at_;
    take('-');
    if (!take('0') && !digits()) {
      return fail("a value expected");
    }
    if (take('.') && !digits()) {
      return fail("a digit expected after '.'");
    }
    if (take('e') || take('E')) {
      if (!take('+')) {
        take('-');
      }
      if (!digits()) {
        return fail("a digit expected in the exponent");
      }
    }
    return Value{Value::Number{std::string{text_.substr(start, at_ - start)}}};
  }

  std::optional<std::string> read_string() {
    ++at_;
    std::string out;
    for (;;) {
      if (at_ == text_.size()) {
        return fail("unterminated string");
      }
      const auto byte = static_cast<unsigned char>(text_[at_]);
      if (byte == '"') {
        ++at_;
        return out;
      }
      if (byte < 0x20) {
        return fail("a control byte in a string");
      }
      if (byte == '\\') {
        if (!read_escape(out)) {
          return std::nullopt;
        }
        continue;
      }
      if (!copy_utf8(out)) {
        return fail("a string that is not UTF-8");
      }
    }
  }

  bool read_escape(std::string &out) {
    ++at_;
    if (at_ == text_.size()) {
      fail("unterminated escape");
      return false;
    }
    const char kind = text_[at_++];
    switch (kind) {
    case '"':
    case '\\':
    case '/':
      out.push_back(kind);
      return true;
    case 'b':
      out.push_back('\b');
      return true;
    case 'f':
      out.push_back('\f');
      return true;
    case 'n':
      out.push_back('\n');
      return true;
    case 'r':
      out.push_back('\r');
      return true;
    case 't':
      out.push_back('\t');
      return true;
    case 'u':
      return read_code_point(out);
    default:
      fail("an unknown escape");
      return false;
    }
  }

  // \uXXXX, with a surrogate pair as one code point.
  bool read_code_point(std::string &out) {
    std::optional<std::uint32_t> unit = hex4();
    if (!unit) {
      return false;
    }
    std::uint32_t code = *unit;
    if (code >= 0xD800 && code <= 0xDBFF) {
      if (text_.substr(at_, 2) != "\\u") {
        fail("a lone high surrogate");
        return false;
      }
      at_ += 2;
      const std::optional<std::uint32_t> low = hex4();
      if (!low) {
        return false;
      }
      if (*low < 0xDC00 || *low > 0xDFFF) {
        fail("a high surrogate without its low half");
        return false;
      }
      code = 0x10000 + ((code - 0xD800) << 10U) + (*low - 0xDC00);
    } else if (code >= 0xDC00 && code <= 0xDFFF) {
      fail("a lone low surrogate");
      return false;
    }
    append_utf8(out, code);
    return true;
  }

  std::optional<std::uint32_t> hex4() {
    std::uint32_t value = 0;
    const std::string_view digits = text_.substr(at_, 4);
    const auto [end, problem] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (digits.size() != 4 || problem != std::errc{} || end != digits.data() + 4 ||
        digits.front() == '+' || digits.front() == '-') {
      fail("four hex digits expected");
      return std::nullopt;
    }
    at_ += 4;
    return value;
  }

  static void append_utf8(std::string &out, std::uint32_t code) {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
      out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
  }

  // One UTF-8 sequence, shortest form, no surrogates, at most U+10FFFF.
  bool copy_utf8(std::string &out) {
    const auto lead = static_cast<unsigned char>(text_[at_]);
    std::size_t length = 1;
    std::uint32_t code = lead;
    std::uint32_t least = 0;
    if (lead >= 0xF0 && lead <= 0xF4) {
      length = 4;
      code = lead & 0x07U;
      least = 0x10000;
    } else if (lead >= 0xE0) {
      length = 3;
      code = lead & 0x0FU;
      least = 0x800;
    } else if (lead >= 0xC2 && lead < 0xE0) {
      length = 2;
      code = lead & 0x1FU;
      least = 0x80;
    } else if (lead >= 0x80) {
      return false;
    }
    if (at_ + length > text_.size()) {
      return false;
    }
    for (std::size_t index = 1; index < length; ++index) {
      const auto next = static_cast<unsigned char>(text_[at_ + index]);
      if ((next & 0xC0U) != 0x80U) {
        return false;
      }
      code = (code << 6U) | (next & 0x3FU);
    }
    if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      return false;
    }
    out.append(text_.substr(at_, length));
    at_ += length;
    return true;
  }

  bool digits() {
    const std::size_t start = at_;
    while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
      ++at_;
    }
    return at_ != start;
  }

  bool take(char expected) {
    if (at_ < text_.size() && text_[at_] == expected) {
      ++at_;
      return true;
    }
    return false;
  }

  void skip_space() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
      ++at_;
    }
  }

  std::nullopt_t fail(std::string reason) {
    if (reason_.empty()) {
      reason_ = std::move(reason);
      failed_at_ = at_;
    }
    return std::nullopt;
  }

  std::string_view text_;
  std::size_t at_ = 0;
  std::string reason_;
  std::size_t failed_at_ = 0;
};

} // namespace

std::optional<std::int64_t> Value::integer() const noexcept {
  const auto *number = std::get_if<Number>(&data_);
  if (number == nullptr) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  const char *end = number->text.data() + number->text.size();
  const auto [stop, problem] = std::from_chars(number->text.data(), end, value);
  if (problem != std::errc{} || stop != end) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> Value::real() const noexcept {
  const auto *number = std::get_if<Number>(&data_);
  if (number == nullptr) {
    return std::nullopt;
  }
  double value = 0;
  const char *end = number->text.data() + number->text.size();
  const auto [stop, problem] = text::from_chars_real(number->text.data(), end, value);
  if (problem != std::errc{} || stop != end) {
    return std::nullopt;
  }
  return value;
}

const Value *Value::find(std::string_view key) const noexcept {
  const Object *members = object();
  if (members == nullptr) {
    return nullptr;
  }
  for (const auto &[name, value] : *members) {
    if (name == key) {
      return &value;
    }
  }
  return nullptr;
}

std::optional<Value> parse(std::string_view text, std::string &error) {
  return Reader{text}.document(error);
}

} // namespace lucent::json

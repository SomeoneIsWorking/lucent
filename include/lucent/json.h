// lucent/json.h — reads JSON text (RFC 8259) into a tree; `text::json_quote` writes strings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lucent::json {

// Arrays and objects nest at most this deep.
inline constexpr std::size_t kMaxDepth = 64;

class Value;
using Array = std::vector<Value>;
// Members in document order; a repeated key keeps every occurrence and `find` sees the first.
using Object = std::vector<std::pair<std::string, Value>>;

class Value {
public:
  // A number keeps its source text, so an integer reads back exactly.
  struct Number {
    std::string text;
  };

  Value() = default;
  explicit Value(bool flag) : data_{flag} {
  }
  explicit Value(Number number) : data_{std::move(number)} {
  }
  explicit Value(std::string text) : data_{std::move(text)} {
  }
  explicit Value(Array items) : data_{std::move(items)} {
  }
  explicit Value(Object members) : data_{std::move(members)} {
  }

  bool is_null() const noexcept {
    return std::holds_alternative<std::monostate>(data_);
  }
  const bool *boolean() const noexcept {
    return std::get_if<bool>(&data_);
  }
  const std::string *string() const noexcept {
    return std::get_if<std::string>(&data_);
  }
  const Array *array() const noexcept {
    return std::get_if<Array>(&data_);
  }
  const Object *object() const noexcept {
    return std::get_if<Object>(&data_);
  }
  // A number written as a plain integer that fits; nothing for 1.5, 1e3 or anything else.
  std::optional<std::int64_t> integer() const noexcept;
  std::optional<double> real() const noexcept;
  // The first member named `key` of an object; null for a missing key or a non-object.
  const Value *find(std::string_view key) const noexcept;

private:
  std::variant<std::monostate, bool, Number, std::string, Array, Object> data_;
};

// The one value `text` holds, with only whitespace around it. Nothing, with `error` naming the
// byte offset, for anything else, a string that is not UTF-8, or nesting past kMaxDepth.
std::optional<Value> parse(std::string_view text, std::string &error);

} // namespace lucent::json

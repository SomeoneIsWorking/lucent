// The JSON reader takes RFC 8259 documents and refuses what is not one; json_quote writes what
// the reader gives back.
#include "lucent/json.h"
#include "lucent/text.h"

#include <cassert>
#include <cstdio>
#include <string>

namespace {

using lucent::json::parse;
using lucent::json::Value;

Value read(std::string_view text) {
  std::string error;
  std::optional<Value> value = parse(text, error);
  assert(value && error.empty());
  return std::move(*value);
}

void refused(std::string_view text) {
  std::string error;
  const std::optional<Value> value = parse(text, error);
  assert(!value && error.find("at byte") != std::string::npos);
}

} // namespace

int main() {
  const Value event = read(R"( ["EVENT", "sub", {"kind": 30078, "created_at": 1731340800,
      "tags": [["d", "lobby"], ["expiration", "1731341100"]], "content": "a\"b\\c\n",
      "ratio": -1.5e2, "on": true, "off": false, "none": null}] )");
  const lucent::json::Array &message = *event.array();
  assert(message.size() == 3 && *message[0].string() == "EVENT");
  const Value &body = message[2];
  assert(body.find("kind")->integer() == 30078);
  assert(body.find("created_at")->integer() == 1731340800);
  assert(*body.find("content")->string() == "a\"b\\c\n");
  assert(body.find("ratio")->real() == -150.0 && !body.find("ratio")->integer());
  assert(*body.find("on")->boolean() && !*body.find("off")->boolean());
  assert(body.find("none")->is_null() && body.find("missing") == nullptr);
  assert(*(*body.find("tags")->array())[1].array()->at(1).string() == "1731341100");

  assert(*read(R"("\u00e9\ud83d\ude00\/")").string() == "\xC3\xA9\xF0\x9F\x98\x80/");
  assert(*read("\"\xE2\x82\xAC\"").string() == "\xE2\x82\xAC");
  assert(!read("99999999999999999999").integer());
  assert(read("[]").array()->empty() && read("{}").object()->empty());

  for (const std::string_view bad :
       {"", "[1,]", "{\"a\" 1}", "[1] 2", "01", "1.", "-", "tru", "\"\\x\"", "\"\x01\"",
        "\"\\ud83d\"", "\"\\ude00\"", "\"\xC0\xAF\"", "\"\xED\xA0\x80\"", "\"open", "{\"a\":}"}) {
    refused(bad);
  }
  refused(std::string(65, '[') + std::string(65, ']'));
  read(std::string(64, '[') + std::string(64, ']'));

  const std::string control = "q\"\\\n\t\r\b\f\x01/\xC3\xA9";
  const std::string quoted = lucent::text::json_quote(control);
  assert(quoted == "\"q\\\"\\\\\\n\\t\\r\\b\\f\\u0001/\xC3\xA9\"");
  assert(*read(quoted).string() == control);
  std::puts(
      "JSON reads RFC 8259 documents, refuses the rest, and reads back what json_quote writes");
  return 0;
}

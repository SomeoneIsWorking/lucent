#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace lucent::p2p::detail {

// One DER certificate as PEM; empty if it does not read.
std::string pem_of_der(std::span<const std::uint8_t> der);

} // namespace lucent::p2p::detail

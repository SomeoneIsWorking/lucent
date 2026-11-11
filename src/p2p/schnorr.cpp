// BIP-340 on OpenSSL's secp256k1 group, step for step as the BIP writes Sign and Verify.
#include "lucent/p2p/crypto.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

#include <algorithm>

namespace lucent::p2p::crypto {
namespace {

constexpr std::size_t kScalarBytes = 32;

struct FreeGroup {
  void operator()(EC_GROUP *group) const noexcept {
    EC_GROUP_free(group);
  }
};
struct FreePoint {
  void operator()(EC_POINT *point) const noexcept {
    EC_POINT_free(point);
  }
};
struct FreeNumber {
  void operator()(BIGNUM *number) const noexcept {
    BN_clear_free(number);
  }
};
struct FreeContext {
  void operator()(BN_CTX *context) const noexcept {
    BN_CTX_free(context);
  }
};
using Point = std::unique_ptr<EC_POINT, FreePoint>;
using Number = std::unique_ptr<BIGNUM, FreeNumber>;

// The curve, its order and field, and a scratch context for one call.
class Curve {
public:
  Curve()
      : group_{EC_GROUP_new_by_curve_name(NID_secp256k1)}, context_{BN_CTX_new()}, order_{BN_new()},
        field_{BN_new()} {
    if (!group_ || !context_ || !order_ || !field_ ||
        EC_GROUP_get_order(group_.get(), order_.get(), context_.get()) != 1 ||
        EC_GROUP_get_curve(group_.get(), field_.get(), nullptr, nullptr, context_.get()) != 1) {
      group_.reset();
    }
  }

  bool usable() const noexcept {
    return group_ != nullptr;
  }
  const BIGNUM *order() const noexcept {
    return order_.get();
  }
  const BIGNUM *field() const noexcept {
    return field_.get();
  }
  BN_CTX *context() const noexcept {
    return context_.get();
  }

  Point point() const {
    return Point{EC_POINT_new(group_.get())};
  }
  // `scalar`·G, plus `other_scalar`·`other` when given.
  Point multiply(const BIGNUM *scalar, const EC_POINT *other = nullptr,
                 const BIGNUM *other_scalar = nullptr) const {
    Point made = point();
    if (!made ||
        EC_POINT_mul(group_.get(), made.get(), scalar, other, other_scalar, context_.get()) != 1) {
      return nullptr;
    }
    return made;
  }
  bool infinite(const EC_POINT *point) const noexcept {
    return EC_POINT_is_at_infinity(group_.get(), point) == 1;
  }
  // The affine x and whether y is even; false at infinity.
  bool coordinates(const EC_POINT *point, BIGNUM *x, bool &even_y) const {
    const Number y{BN_new()};
    if (!y ||
        EC_POINT_get_affine_coordinates(group_.get(), point, x, y.get(), context_.get()) != 1) {
      return false;
    }
    even_y = BN_is_odd(y.get()) == 0;
    return true;
  }
  // The point with x `x` and an even y; nothing when there is none.
  Point lift_x(const BIGNUM *x) const {
    Point lifted = point();
    if (!lifted || BN_cmp(x, field_.get()) >= 0 ||
        EC_POINT_set_compressed_coordinates(group_.get(), lifted.get(), x, 0, context_.get()) !=
            1) {
      return nullptr;
    }
    return lifted;
  }

private:
  std::unique_ptr<EC_GROUP, FreeGroup> group_;
  std::unique_ptr<BN_CTX, FreeContext> context_;
  Number order_;
  Number field_;
};

Number number_of(std::span<const std::uint8_t> bytes) {
  return Number{BN_bin2bn(bytes.data(), static_cast<int>(bytes.size()), nullptr)};
}

Bytes bytes_of(const BIGNUM *number) {
  Bytes out(kScalarBytes);
  BN_bn2binpad(number, out.data(), static_cast<int>(out.size()));
  return out;
}

// SHA256(SHA256(tag) || SHA256(tag) || parts...), the BIP's hash_tag(x).
Bytes tagged_hash(std::string_view tag,
                  std::initializer_list<std::span<const std::uint8_t>> parts) {
  const Bytes tag_hash = sha256(crypto::bytes_of(tag));
  Bytes input;
  input.insert(input.end(), tag_hash.begin(), tag_hash.end());
  input.insert(input.end(), tag_hash.begin(), tag_hash.end());
  for (const std::span<const std::uint8_t> part : parts) {
    input.insert(input.end(), part.begin(), part.end());
  }
  return sha256(input);
}

// int(hash) mod n.
Number reduced(const Curve &curve, std::span<const std::uint8_t> hash) {
  Number value = number_of(hash);
  if (!value || BN_nnmod(value.get(), value.get(), curve.order(), curve.context()) != 1) {
    return nullptr;
  }
  return value;
}

// d' from the secret, or nothing when it is zero or not below n.
Number secret_scalar(const Curve &curve, std::span<const std::uint8_t> secret) {
  if (secret.size() != kScalarBytes) {
    return nullptr;
  }
  Number scalar = number_of(secret);
  if (!scalar || BN_is_zero(scalar.get()) || BN_cmp(scalar.get(), curve.order()) >= 0) {
    return nullptr;
  }
  return scalar;
}

// `value` replaced by n - `value` unless `keep`.
bool negate_unless(const Curve &curve, BIGNUM *value, bool keep) {
  return keep || BN_sub(value, curve.order(), value) == 1;
}

} // namespace

std::optional<Bytes> schnorr_public_key(std::span<const std::uint8_t> secret) {
  const Curve curve;
  const Number scalar = curve.usable() ? secret_scalar(curve, secret) : nullptr;
  const Point public_point = scalar ? curve.multiply(scalar.get()) : nullptr;
  const Number x{BN_new()};
  bool even = false;
  if (!public_point || !x || !curve.coordinates(public_point.get(), x.get(), even)) {
    return std::nullopt;
  }
  return bytes_of(x.get());
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<Bytes> schnorr_sign(std::span<const std::uint8_t> secret,
                                  std::span<const std::uint8_t> message,
                                  std::span<const std::uint8_t> aux) {
  const Curve curve;
  if (!curve.usable() || aux.size() != kScalarBytes) {
    return std::nullopt;
  }
  const Number d = secret_scalar(curve, secret);
  const Point public_point = d ? curve.multiply(d.get()) : nullptr;
  const Number public_x{BN_new()};
  bool public_even = false;
  if (!public_point || !public_x ||
      !curve.coordinates(public_point.get(), public_x.get(), public_even) ||
      !negate_unless(curve, d.get(), public_even)) {
    return std::nullopt;
  }
  const Bytes p = bytes_of(public_x.get());
  Bytes t = bytes_of(d.get());
  const Bytes aux_hash = tagged_hash("BIP0340/aux", {aux});
  std::transform(t.begin(), t.end(), aux_hash.begin(), t.begin(),
                 [](std::uint8_t first, std::uint8_t second) {
                   return static_cast<std::uint8_t>(first ^ second);
                 });
  const Number k = reduced(curve, tagged_hash("BIP0340/nonce", {t, p, message}));
  wipe(t);
  if (!k || BN_is_zero(k.get())) {
    return std::nullopt;
  }
  const Point nonce_point = curve.multiply(k.get());
  const Number nonce_x{BN_new()};
  bool nonce_even = false;
  if (!nonce_point || !nonce_x ||
      !curve.coordinates(nonce_point.get(), nonce_x.get(), nonce_even) ||
      !negate_unless(curve, k.get(), nonce_even)) {
    return std::nullopt;
  }
  const Bytes r = bytes_of(nonce_x.get());
  const Number e = reduced(curve, tagged_hash("BIP0340/challenge", {r, p, message}));
  const Number s{BN_new()};
  if (!e || !s || BN_mod_mul(s.get(), e.get(), d.get(), curve.order(), curve.context()) != 1 ||
      BN_mod_add(s.get(), s.get(), k.get(), curve.order(), curve.context()) != 1) {
    return std::nullopt;
  }
  Bytes signature = r;
  const Bytes s_bytes = bytes_of(s.get());
  signature.insert(signature.end(), s_bytes.begin(), s_bytes.end());
  if (!schnorr_verify(p, message, signature)) {
    return std::nullopt;
  }
  return signature;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool schnorr_verify(std::span<const std::uint8_t> public_key, std::span<const std::uint8_t> message,
                    std::span<const std::uint8_t> signature) {
  const Curve curve;
  if (!curve.usable() || public_key.size() != kScalarBytes ||
      signature.size() != kSchnorrSignatureBytes) {
    return false;
  }
  const Number public_x = number_of(public_key);
  const Point public_point = public_x ? curve.lift_x(public_x.get()) : nullptr;
  const std::span<const std::uint8_t> r_bytes = signature.first(kScalarBytes);
  const Number r = number_of(r_bytes);
  const Number s = number_of(signature.last(kScalarBytes));
  if (!public_point || !r || !s || BN_cmp(r.get(), curve.field()) >= 0 ||
      BN_cmp(s.get(), curve.order()) >= 0) {
    return false;
  }
  const Number e = reduced(curve, tagged_hash("BIP0340/challenge", {r_bytes, public_key, message}));
  // R = s·G + (n - e)·P.
  if (!e || !negate_unless(curve, e.get(), BN_is_zero(e.get()) != 0)) {
    return false;
  }
  const Point nonce_point = curve.multiply(s.get(), public_point.get(), e.get());
  const Number nonce_x{BN_new()};
  bool even = false;
  return nonce_point && nonce_x && !curve.infinite(nonce_point.get()) &&
         curve.coordinates(nonce_point.get(), nonce_x.get(), even) && even &&
         BN_cmp(nonce_x.get(), r.get()) == 0;
}

} // namespace lucent::p2p::crypto

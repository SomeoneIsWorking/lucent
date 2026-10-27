#include <lucent/content.h>

#include <algorithm>
#include <array>
#include <vector>
#include <bit>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace lucent::content {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

class Sha256State {
public:
  void update(std::span<const std::byte> bytes) {
    total_bytes_ += bytes.size();
    while (!bytes.empty()) {
      std::size_t count = std::min(bytes.size(), block_.size() - block_size_);
      std::copy_n(bytes.begin(), count, block_.data() + block_size_);
      block_size_ += count;
      bytes = bytes.subspan(count);
      if (block_size_ == block_.size()) {
        transform(block_);
        block_size_ = 0;
      }
    }
  }

  Sha256 finish() {
    std::uint64_t bit_count = total_bytes_ * 8U;
    block_[block_size_++] = std::byte{0x80};
    if (block_size_ > 56) {
      std::fill(block_.data() + block_size_, block_.data() + block_.size(), std::byte{0});
      transform(block_);
      block_size_ = 0;
    }
    std::fill(block_.data() + block_size_, block_.data() + 56, std::byte{0});
    for (std::size_t index = 0; index < 8; ++index) {
      block_[63 - index] = std::byte((bit_count >> (index * 8U)) & 0xffU);
    }
    transform(block_);

    Sha256 digest{};
    for (std::size_t word = 0; word < state_.size(); ++word) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        digest[word * 4 + byte] = static_cast<std::uint8_t>(state_[word] >> ((3U - byte) * 8U));
      }
    }
    return digest;
  }

private:
  void transform(const std::array<std::byte, 64> &block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      std::size_t offset = index * 4;
      words[index] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
                     (std::to_integer<std::uint32_t>(block[offset + 1]) << 16U) |
                     (std::to_integer<std::uint32_t>(block[offset + 2]) << 8U) |
                     std::to_integer<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      std::uint32_t s0 = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^
                         (words[index - 15] >> 3U);
      std::uint32_t s1 = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^
                         (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto working = state_;
    for (std::size_t index = 0; index < words.size(); ++index) {
      std::uint32_t sum1 =
          std::rotr(working[4], 6) ^ std::rotr(working[4], 11) ^ std::rotr(working[4], 25);
      std::uint32_t choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
      std::uint32_t temporary1 = working[7] + sum1 + choice + round_constants[index] + words[index];
      std::uint32_t sum0 =
          std::rotr(working[0], 2) ^ std::rotr(working[0], 13) ^ std::rotr(working[0], 22);
      std::uint32_t majority =
          (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
      std::uint32_t temporary2 = sum0 + majority;
      working = {temporary1 + temporary2, working[0], working[1], working[2],
                 working[3] + temporary1, working[4], working[5], working[6]};
    }
    for (std::size_t index = 0; index < state_.size(); ++index) {
      state_[index] += working[index];
    }
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<std::byte, 64> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t total_bytes_ = 0;
};

constexpr std::array<std::uint64_t, 80> round_constants_512 = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

class Sha512State {
public:
  void update(std::span<const std::byte> bytes) {
    total_bytes_ += bytes.size();
    while (!bytes.empty()) {
      std::size_t count = std::min(bytes.size(), block_.size() - block_size_);
      std::copy_n(bytes.begin(), count, block_.data() + block_size_);
      block_size_ += count;
      bytes = bytes.subspan(count);
      if (block_size_ == block_.size()) {
        transform(block_);
        block_size_ = 0;
      }
    }
  }

  Sha512 finish() {
    // Inputs stay below 2^61 bytes, so the high half of the 128-bit length is zero.
    std::uint64_t bit_count = total_bytes_ * 8U;
    block_[block_size_++] = std::byte{0x80};
    if (block_size_ > 112) {
      std::fill(block_.data() + block_size_, block_.data() + block_.size(), std::byte{0});
      transform(block_);
      block_size_ = 0;
    }
    std::fill(block_.data() + block_size_, block_.data() + 120, std::byte{0});
    for (std::size_t index = 0; index < 8; ++index) {
      block_[127 - index] = std::byte((bit_count >> (index * 8U)) & 0xffU);
    }
    transform(block_);

    Sha512 digest{};
    for (std::size_t word = 0; word < state_.size(); ++word) {
      for (std::size_t byte = 0; byte < 8; ++byte) {
        digest[word * 8 + byte] = static_cast<std::uint8_t>(state_[word] >> ((7U - byte) * 8U));
      }
    }
    return digest;
  }

private:
  void transform(const std::array<std::byte, 128> &block) {
    std::array<std::uint64_t, 80> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      std::uint64_t word = 0;
      for (std::size_t byte = 0; byte < 8; ++byte) {
        word = (word << 8U) | std::to_integer<std::uint64_t>(block[index * 8 + byte]);
      }
      words[index] = word;
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      std::uint64_t s0 = std::rotr(words[index - 15], 1) ^ std::rotr(words[index - 15], 8) ^
                         (words[index - 15] >> 7U);
      std::uint64_t s1 = std::rotr(words[index - 2], 19) ^ std::rotr(words[index - 2], 61) ^
                         (words[index - 2] >> 6U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto working = state_;
    for (std::size_t index = 0; index < words.size(); ++index) {
      std::uint64_t sum1 =
          std::rotr(working[4], 14) ^ std::rotr(working[4], 18) ^ std::rotr(working[4], 41);
      std::uint64_t choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
      std::uint64_t temporary1 =
          working[7] + sum1 + choice + round_constants_512[index] + words[index];
      std::uint64_t sum0 =
          std::rotr(working[0], 28) ^ std::rotr(working[0], 34) ^ std::rotr(working[0], 39);
      std::uint64_t majority =
          (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
      std::uint64_t temporary2 = sum0 + majority;
      working = {temporary1 + temporary2, working[0], working[1], working[2],
                 working[3] + temporary1, working[4], working[5], working[6]};
    }
    for (std::size_t index = 0; index < state_.size(); ++index) {
      state_[index] += working[index];
    }
  }

  std::array<std::uint64_t, 8> state_ = {
      0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
      0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
  };
  std::array<std::byte, 128> block_{};
  std::size_t block_size_ = 0;
  std::uint64_t total_bytes_ = 0;
};

template <typename State>
auto hash_file(const std::filesystem::path &path, std::string &error, const char *name)
    -> std::optional<decltype(State{}.finish())> {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = std::string{"cannot open file for "} + name + ": " + path.string();
    return std::nullopt;
  }
  State state;
  // The read buffer belongs on the heap: a hashing helper is called from
  // whatever stack the caller happens to be on, and a browser build gives a
  // thread only 64 KB of it, which this buffer alone would exhaust.
  std::vector<std::byte> buffer(std::size_t{64} * 1024);
  while (input) {
    input.read(reinterpret_cast<char *>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));
    auto count = input.gcount();
    if (count > 0) {
      state.update(std::span(buffer.data(), static_cast<std::size_t>(count)));
    }
  }
  if (!input.eof()) {
    error = std::string{"cannot read file for "} + name + ": " + path.string();
    return std::nullopt;
  }
  error.clear();
  return state.finish();
}

std::string hex_of(std::span<const std::uint8_t> digest) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::uint8_t byte : digest) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

} // namespace

Sha256 sha256(std::span<const std::byte> bytes) {
  Sha256State state;
  state.update(bytes);
  return state.finish();
}

std::optional<Sha256> sha256_file(const std::filesystem::path &path, std::string &error) {
  return hash_file<Sha256State>(path, error, "SHA-256");
}

std::string sha256_hex(const Sha256 &digest) {
  return hex_of(digest);
}

Sha512 sha512(std::span<const std::byte> bytes) {
  Sha512State state;
  state.update(bytes);
  return state.finish();
}

std::optional<Sha512> sha512_file(const std::filesystem::path &path, std::string &error) {
  return hash_file<Sha512State>(path, error, "SHA-512");
}

std::string sha512_hex(const Sha512 &digest) {
  return hex_of(digest);
}

} // namespace lucent::content

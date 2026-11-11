#include "lucent/p2p/tls_stream.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "lucent/log.h"

namespace lucent::p2p {
namespace {

// The updatable store since Android 14, then the one in the system image. Their file names use
// OpenSSL's old subject hash, which a CA directory lookup no longer matches, so each is read.
constexpr std::array<const char *, 2> kCaFolders{"/apex/com.android.conscrypt/cacerts",
                                                 "/system/etc/security/cacerts"};

} // namespace

std::string system_roots_pem() {
  std::string pem;
  for (const char *folder : kCaFolders) {
    std::error_code error;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator{folder, error}) {
      std::ifstream file{entry.path(), std::ios::binary};
      pem.append(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
      pem.push_back('\n');
    }
    if (!pem.empty()) {
      return pem;
    }
  }
  lucent::log(Level::Warn, "tls", "no system CA certificates found; TLS connections will fail");
  return pem;
}

} // namespace lucent::p2p

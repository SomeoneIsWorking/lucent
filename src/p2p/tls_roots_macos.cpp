#include "lucent/p2p/tls_stream.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include "lucent/log.h"
#include "tls_roots.h"

namespace lucent::p2p {

std::string system_roots_pem() {
  std::string pem;
  CFArrayRef anchors = nullptr;
  if (SecTrustCopyAnchorCertificates(&anchors) != errSecSuccess || anchors == nullptr) {
    lucent::log(Level::Warn, "tls", "the Keychain gives no system roots");
    return pem;
  }
  for (CFIndex index = 0; index < CFArrayGetCount(anchors); ++index) {
    const auto certificate =
        static_cast<SecCertificateRef>(const_cast<void *>(CFArrayGetValueAtIndex(anchors, index)));
    CFDataRef der = SecCertificateCopyData(certificate);
    if (der == nullptr) {
      continue;
    }
    pem +=
        detail::pem_of_der({CFDataGetBytePtr(der), static_cast<std::size_t>(CFDataGetLength(der))});
    CFRelease(der);
  }
  CFRelease(anchors);
  return pem;
}

} // namespace lucent::p2p

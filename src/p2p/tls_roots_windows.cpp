#include "lucent/p2p/tls_stream.h"

#include <windows.h>

#include <wincrypt.h>

#include "lucent/log.h"
#include "tls_roots.h"

namespace lucent::p2p {

std::string system_roots_pem() {
  std::string pem;
  HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
  if (store == nullptr) {
    lucent::log(Level::Warn, "tls", "the ROOT certificate store does not open");
    return pem;
  }
  for (PCCERT_CONTEXT certificate = CertEnumCertificatesInStore(store, nullptr);
       certificate != nullptr; certificate = CertEnumCertificatesInStore(store, certificate)) {
    pem += detail::pem_of_der({certificate->pbCertEncoded, certificate->cbCertEncoded});
  }
  CertCloseStore(store, 0);
  return pem;
}

} // namespace lucent::p2p

// A TLS stream to a loopback server whose certificate a test CA signs for one name: the right
// name and root connect and carry bytes both ways; another name, or no such root, is refused.
#include "lucent/p2p/tls_stream.h"

#include "http_socket.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace lucent;
namespace detail = lucent::http::detail;

constexpr const char *kName = "relay.lucent.test";

struct KeyFree {
  void operator()(EVP_PKEY *key) const noexcept {
    EVP_PKEY_free(key);
  }
};
struct CertificateFree {
  void operator()(X509 *certificate) const noexcept {
    X509_free(certificate);
  }
};
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;
using Certificate = std::unique_ptr<X509, CertificateFree>;

Certificate certificate_for(const char *common_name, EVP_PKEY *key, X509 *issuer,
                            EVP_PKEY *issuer_key, bool authority) {
  Certificate made{X509_new()};
  X509_set_version(made.get(), 2);
  ASN1_INTEGER_set(X509_get_serialNumber(made.get()), authority ? 1 : 2);
  X509_gmtime_adj(X509_getm_notBefore(made.get()), -60);
  X509_gmtime_adj(X509_getm_notAfter(made.get()), 3600);
  X509_set_pubkey(made.get(), key);
  X509_NAME *name = X509_get_subject_name(made.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char *>(common_name), -1, -1, 0);
  X509_set_issuer_name(made.get(), issuer != nullptr ? X509_get_subject_name(issuer) : name);
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer != nullptr ? issuer : made.get(), made.get(), nullptr, nullptr,
                 0);
  const std::string alt = std::string{"DNS:"} + common_name;
  for (const auto &[nid, value] :
       {std::pair{NID_basic_constraints, std::string{authority ? "critical,CA:TRUE" : "CA:FALSE"}},
        std::pair{NID_subject_alt_name, alt}}) {
    X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
    X509_add_ext(made.get(), extension, -1);
    X509_EXTENSION_free(extension);
  }
  X509_sign(made.get(), issuer_key, EVP_sha256());
  return made;
}

std::string pem_of(X509 *certificate) {
  BIO *bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, certificate);
  char *text = nullptr;
  const long length = BIO_get_mem_data(bio, &text);
  std::string pem{text, static_cast<std::size_t>(length)};
  BIO_free(bio);
  return pem;
}

// A loopback listener that serves `rounds` TLS connections, answering "ping" with "pong".
class Server {
public:
  Server(X509 *certificate, EVP_PKEY *key, int rounds)
      : context_{SSL_CTX_new(TLS_server_method())}, listener_{detail::create_tcp_socket()} {
    SSL_CTX_use_certificate(context_, certificate);
    SSL_CTX_use_PrivateKey(context_, key);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x7F000001u);
    const int bound = ::bind(detail::native_socket(listener_),
                             reinterpret_cast<const sockaddr *>(&address), sizeof address);
    assert(bound == 0);
    ::listen(detail::native_socket(listener_), 4);
    detail::SocketLength size = sizeof address;
    ::getsockname(detail::native_socket(listener_), reinterpret_cast<sockaddr *>(&address), &size);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread{[this, rounds] {
      serve(rounds);
    }};
  }
  ~Server() {
    thread_.join();
    detail::close_socket(listener_);
    SSL_CTX_free(context_);
  }
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  std::uint16_t port() const noexcept {
    return port_;
  }

private:
  void serve(int rounds) {
    for (int round = 0; round < rounds; ++round) {
      const detail::Socket client = detail::accept_socket(listener_);
      SSL *ssl = SSL_new(context_);
      SSL_set_fd(ssl, static_cast<int>(detail::native_socket(client)));
      if (SSL_accept(ssl) == 1) {
        std::string asked(4, '\0');
        if (SSL_read(ssl, asked.data(), 4) == 4 && asked == "ping") {
          SSL_write(ssl, "pong", 4);
        }
        SSL_shutdown(ssl);
      }
      SSL_free(ssl);
      detail::close_socket(client);
    }
  }

  SSL_CTX *context_;
  detail::Socket listener_;
  std::uint16_t port_ = 0;
  std::thread thread_;
};

} // namespace

int main() {
  const bool runtime = detail::initialize_socket_runtime();
  assert(runtime);
  const Key authority_key{EVP_EC_gen("P-256")};
  const Key server_key{EVP_EC_gen("P-256")};
  const Certificate authority =
      certificate_for("Lucent test CA", authority_key.get(), nullptr, authority_key.get(), true);
  const Certificate server =
      certificate_for(kName, server_key.get(), authority.get(), authority_key.get(), false);
  const net::Endpoint loopback_at{0x7F000001u, 0};
  Server listener{server.get(), server_key.get(), 3};
  const net::Endpoint at{loopback_at.address, listener.port()};
  const p2p::TlsTrust trusted{false, pem_of(authority.get())};

  std::string error;
  const auto stream = p2p::TlsStream::connect(at, kName, trusted, error);
  assert(stream && error.empty());
  const bool sent = stream->send_all("ping", error);
  assert(sent);
  std::string answer;
  while (answer.size() < 4 && stream->read_some(answer) == net::ReadOutcome::Data) {
  }
  assert(answer == "pong");

  error.clear();
  const auto misnamed = p2p::TlsStream::connect(at, "other.lucent.test", trusted, error);
  assert(!misnamed && error.find("refused") != std::string::npos);

  error.clear();
  const auto untrusted = p2p::TlsStream::connect(at, kName, p2p::TlsTrust{}, error);
  assert(!untrusted && error.find("refused") != std::string::npos);
  std::puts("a TLS stream carries bytes to the name its root vouches for, and refuses the rest");
  return 0;
}

#include "lucent/p2p/tls_stream.h"

#include "http_socket.h"
#include "tls_roots.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <climits>
#include <optional>
#include <span>
#include <utility>

namespace lucent::p2p {
namespace {

struct ContextFree {
  void operator()(SSL_CTX *context) const noexcept {
    SSL_CTX_free(context);
  }
};
struct SslFree {
  void operator()(SSL *ssl) const noexcept {
    SSL_free(ssl);
  }
};
struct BioFree {
  void operator()(BIO *bio) const noexcept {
    BIO_free(bio);
  }
};
struct X509Free {
  void operator()(X509 *certificate) const noexcept {
    X509_free(certificate);
  }
};

std::string openssl_error(std::string_view what) {
  std::array<char, 256> text{};
  const unsigned long code = ERR_get_error();
  ERR_clear_error();
  if (code == 0) {
    return std::string{what};
  }
  ERR_error_string_n(code, text.data(), text.size());
  return std::string{what} + ": " + text.data();
}

// Adds each certificate in `pem` to `store`; false if one does not read.
bool add_pem(X509_STORE *store, std::string_view pem) {
  const std::unique_ptr<BIO, BioFree> bio{
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
  if (!bio) {
    return false;
  }
  bool any = false;
  while (true) {
    std::unique_ptr<X509, X509Free> certificate{
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
    if (!certificate) {
      break;
    }
    // A duplicate of a root already there is not an error.
    X509_STORE_add_cert(store, certificate.get());
    any = true;
  }
  ERR_clear_error();
  return any;
}

std::unique_ptr<SSL_CTX, ContextFree> client_context(const TlsTrust &trust, std::string &error) {
  std::unique_ptr<SSL_CTX, ContextFree> context{SSL_CTX_new(TLS_client_method())};
  if (!context) {
    error = openssl_error("no TLS context");
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
  SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
  X509_STORE *store = SSL_CTX_get_cert_store(context.get());
  if (trust.system) {
    const std::string pem = system_roots_pem();
    if (pem.empty()) {
      SSL_CTX_set_default_verify_paths(context.get());
    } else {
      add_pem(store, pem);
    }
  }
  if (!trust.extra_pem.empty() && !add_pem(store, trust.extra_pem)) {
    error = "the extra roots hold no certificate";
    return nullptr;
  }
  return context;
}

} // namespace

namespace detail {

std::string pem_of_der(std::span<const std::uint8_t> der) {
  const unsigned char *bytes = der.data();
  const std::unique_ptr<X509, X509Free> certificate{
      d2i_X509(nullptr, &bytes, static_cast<long>(der.size()))};
  const std::unique_ptr<BIO, BioFree> bio{BIO_new(BIO_s_mem())};
  if (!certificate || !bio || PEM_write_bio_X509(bio.get(), certificate.get()) != 1) {
    ERR_clear_error();
    return {};
  }
  char *text = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &text);
  return {text, static_cast<std::size_t>(length)};
}

} // namespace detail

struct TlsStream::Session {
  std::unique_ptr<net::TcpStream> tcp;
  std::unique_ptr<SSL_CTX, ContextFree> context;
  std::unique_ptr<SSL, SslFree> ssl;
};

TlsStream::TlsStream(std::unique_ptr<Session> session) noexcept : session_{std::move(session)} {
}

TlsStream::~TlsStream() {
  if (session_ && session_->ssl) {
    SSL_shutdown(session_->ssl.get());
  }
}

std::unique_ptr<TlsStream> TlsStream::connect(std::string_view name, std::uint16_t port,
                                              const TlsTrust &trust, std::string &error) {
  const std::optional<net::Endpoint> at =
      net::resolve_endpoint(std::string{name} + ":" + std::to_string(port));
  if (!at) {
    error = "cannot resolve " + std::string{name};
    return nullptr;
  }
  return connect(*at, name, trust, error);
}

std::unique_ptr<TlsStream> TlsStream::connect(const net::Endpoint &at, std::string_view name,
                                              const TlsTrust &trust, std::string &error) {
  auto session = std::make_unique<Session>();
  session->context = client_context(trust, error);
  if (!session->context) {
    return nullptr;
  }
  session->tcp = net::TcpStream::connect(at, error);
  if (!session->tcp) {
    return nullptr;
  }
  session->ssl.reset(SSL_new(session->context.get()));
  const std::string host{name};
  if (!session->ssl ||
      SSL_set_fd(session->ssl.get(), static_cast<int>(session->tcp->native())) != 1 ||
      SSL_set_tlsext_host_name(session->ssl.get(), host.c_str()) != 1 ||
      SSL_set1_host(session->ssl.get(), host.c_str()) != 1) {
    error = openssl_error("TLS setup failed");
    return nullptr;
  }
  if (SSL_connect(session->ssl.get()) != 1) {
    const long verified = SSL_get_verify_result(session->ssl.get());
    error = verified != X509_V_OK ? std::string{"the certificate of "} + host +
                                        " is refused: " + X509_verify_cert_error_string(verified)
                                  : openssl_error("TLS handshake with " + host + " failed");
    return nullptr;
  }
  return std::unique_ptr<TlsStream>{new TlsStream{std::move(session)}};
}

bool TlsStream::send_all(std::string_view bytes, std::string &error) {
  while (!bytes.empty()) {
    const int chunk = static_cast<int>(std::min<std::size_t>(bytes.size(), INT_MAX));
    const int sent = SSL_write(session_->ssl.get(), bytes.data(), chunk);
    if (sent <= 0) {
      error = openssl_error("TLS send failed");
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

net::ReadOutcome TlsStream::read_some(std::string &buffer) {
  std::array<char, 16384> chunk{};
  const int received = SSL_read(session_->ssl.get(), chunk.data(), static_cast<int>(chunk.size()));
  if (received > 0) {
    buffer.append(chunk.data(), static_cast<std::size_t>(received));
    return net::ReadOutcome::Data;
  }
  const int reason = SSL_get_error(session_->ssl.get(), received);
  ERR_clear_error();
  switch (reason) {
  case SSL_ERROR_ZERO_RETURN:
    return net::ReadOutcome::Closed;
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_WRITE:
    return net::ReadOutcome::TimedOut;
  case SSL_ERROR_SYSCALL:
    // The socket's receive timeout ends a blocking read as a system error.
    return http::detail::socket_error_would_block(http::detail::last_socket_error())
               ? net::ReadOutcome::TimedOut
               : net::ReadOutcome::Failed;
  default:
    return net::ReadOutcome::Failed;
  }
}

std::unique_ptr<net::ByteStream> connect_stream(const net::Url &url, const TlsTrust &trust,
                                                std::string &error) {
  if (url.secure()) {
    return TlsStream::connect(url.host, url.port, trust, error);
  }
  const std::optional<net::Endpoint> at =
      net::resolve_endpoint(url.host + ":" + std::to_string(url.port));
  if (!at) {
    error = "cannot resolve " + url.host;
    return nullptr;
  }
  return net::TcpStream::connect(*at, error);
}

} // namespace lucent::p2p

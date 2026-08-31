#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace lucent::http::detail {

// SOCKET is pointer-width on Windows. Keeping it in an unsigned pointer-width
// value preserves its identity in the concurrent listener/client registries.
using Socket = std::uintptr_t;

#ifdef _WIN32
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr Socket kInvalidSocket = static_cast<Socket>(INVALID_SOCKET);
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr Socket kInvalidSocket = static_cast<Socket>(-1);
#endif

inline NativeSocket native_socket(Socket socket) {
  return static_cast<NativeSocket>(socket);
}

inline Socket stored_socket(NativeSocket socket) {
  return static_cast<Socket>(socket);
}

inline bool is_invalid_socket(Socket socket) {
  return socket == kInvalidSocket;
}

inline bool initialize_socket_runtime() {
#ifdef _WIN32
  // WSAStartup is process-scoped. This intentionally stays alive through
  // process shutdown so static destructors cannot close a socket after cleanup.
  static const bool initialized = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
#else
  return true;
#endif
}

inline int last_socket_error() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

inline bool socket_error_interrupted(int error) {
#ifdef _WIN32
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

inline bool socket_error_would_block(int error) {
#ifdef _WIN32
  return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
#else
  return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

inline Socket create_tcp_socket() {
  return stored_socket(::socket(AF_INET, SOCK_STREAM, 0));
}

inline Socket accept_socket(Socket listener) {
  return stored_socket(::accept(native_socket(listener), nullptr, nullptr));
}

inline void close_socket(Socket socket) {
  if (is_invalid_socket(socket))
    return;
#ifdef _WIN32
  closesocket(native_socket(socket));
#else
  close(native_socket(socket));
#endif
}

inline void set_close_on_exec(Socket socket) {
#ifndef _WIN32
  const int flags = fcntl(native_socket(socket), F_GETFD);
  if (flags >= 0)
    fcntl(native_socket(socket), F_SETFD, flags | FD_CLOEXEC);
#else
  (void)socket;
#endif
}

inline void set_socket_timeouts(Socket socket) {
#ifdef _WIN32
  constexpr DWORD timeout_ms = 5000;
  setsockopt(native_socket(socket), SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
  setsockopt(native_socket(socket), SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
  constexpr timeval timeout{5, 0};
  setsockopt(native_socket(socket), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(native_socket(socket), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

inline void set_reuse_address(Socket socket) {
  const int reuse = 1;
#ifdef _WIN32
  setsockopt(native_socket(socket), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
  setsockopt(native_socket(socket), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
}

inline std::ptrdiff_t send_bytes(Socket socket, const char *bytes, std::size_t size) {
#ifdef _WIN32
  const auto bounded =
      static_cast<int>(std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int sent = ::send(native_socket(socket), bytes, bounded, 0);
  return sent == SOCKET_ERROR ? -1 : sent;
#else
  return ::send(native_socket(socket), bytes, size, MSG_NOSIGNAL);
#endif
}

inline std::ptrdiff_t receive_bytes(Socket socket, char *bytes, std::size_t size) {
#ifdef _WIN32
  const auto bounded =
      static_cast<int>(std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int received = ::recv(native_socket(socket), bytes, bounded, 0);
  return received == SOCKET_ERROR ? -1 : received;
#else
  return ::recv(native_socket(socket), bytes, size, 0);
#endif
}

inline int shutdown_socket(Socket socket) {
#ifdef _WIN32
  return ::shutdown(native_socket(socket), SD_BOTH);
#else
  return ::shutdown(native_socket(socket), SHUT_RDWR);
#endif
}

} // namespace lucent::http::detail

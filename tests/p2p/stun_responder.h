#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "lucent/udp_socket.h"

namespace lucent::p2p::test {

// A STUN server on loopback that answers each Binding request with XOR-MAPPED-ADDRESS.
class StunResponder {
public:
  StunResponder() : socket_{bound()} {
    thread_ = std::thread{[this] {
      std::vector<char> buffer(2048);
      while (!stop_) {
        net::Endpoint from;
        const auto length = socket_.receive(buffer, from, std::chrono::milliseconds{20});
        if (!length || *length < 20) {
          continue;
        }
        std::string answer(buffer.data(), 20);
        answer[0] = 0x01;
        answer[1] = 0x01;
        answer[2] = 0;
        answer[3] = 12;
        const std::uint16_t port = from.port ^ 0x2112u;
        const std::uint32_t address = from.address ^ 0x2112A442u;
        answer += std::string{"\x00\x20\x00\x08\x00\x01", 6};
        answer.push_back(static_cast<char>(port >> 8u));
        answer.push_back(static_cast<char>(port & 0xFFu));
        for (int shift = 24; shift >= 0; shift -= 8) {
          answer.push_back(static_cast<char>((address >> shift) & 0xFFu));
        }
        socket_.send(from, answer);
        ++answered_;
      }
    }};
  }
  ~StunResponder() {
    stop_ = true;
    thread_.join();
  }
  StunResponder(const StunResponder &) = delete;
  StunResponder &operator=(const StunResponder &) = delete;
  StunResponder(StunResponder &&) = delete;
  StunResponder &operator=(StunResponder &&) = delete;

  std::string address() const {
    return "127.0.0.1:" + std::to_string(socket_.port());
  }
  int answered() const {
    return answered_;
  }

private:
  static net::UdpSocket bound() {
    std::string error;
    std::optional<net::UdpSocket> socket = net::UdpSocket::bind(0, error);
    assert(socket);
    return std::move(*socket);
  }

  net::UdpSocket socket_;
  std::atomic<bool> stop_{false};
  std::atomic<int> answered_{0};
  std::thread thread_;
};

} // namespace lucent::p2p::test

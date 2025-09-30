#include <boost/program_options.hpp>

import std;

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;
namespace po = boost::program_options;

using socket_t = int;

namespace {
constexpr string_view EXIT_MESSAGE = "[[EXIT]]";
constexpr size_t MAX_UDP_PAYLOAD = 2048;

volatile sig_atomic_t STOP_REQUESTED = 0;

void handleSignal(int signum) {
  if (signum == SIGINT) {
    STOP_REQUESTED = 1;
  }
}
} // namespace

int main(int argc, char *argv[]) {
  string host;
  uint16_t port;
  uint8_t channels;
  bool help;

  po::options_description desc("UDP Subscriber options");
  desc.add_options()("help,h", po::bool_switch(&help), "Show help message")(
      "host", po::value<string>(&host)->default_value("127.0.0.1"),
      "Broker host address")(
      "port,p", po::value<uint16_t>(&port)->default_value(5000), "Broker port")(
      "channels,c", po::value<uint8_t>(&channels)->default_value(0),
      "Channels to subscribe to (comma-separated, or 'ALL' for all channels)");

  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    if (help) {
      cout << desc << '\n';
      return 0;
    }
  } catch (const po::error &e) {
    println(stderr, "\033[31mError parsing arguments: {}\033[0m", e.what());
    cout << desc << '\n';
    return 1;
  }

  print(R"( ▄▄▄ █  ▐▌▗▖       █  ▐▌ ▄▄▄ ▄ ▄▄▄▄    █  ▐▌▗▖   ▄▄▄
▀▄▄  ▀▄▄▞▘▐▌       ▀▄▄▞▘█    ▄ █   █   ▀▄▄▞▘▐▌  █   █
▄▄▄▀      ▐▛▀▚▖         █    █ █   █        ▐▛▀▚▖█   █
          ▐▙▄▞▘              █     ▗▄▖      ▐▙▄▞▘█▄▄▄▀
                                  ▐▌ ▐▌                █
                                   ▝▀▜▌                ▀
                                  ▐▙▄▞▘                   )");

  println("\n\n--    Press ctrl+c to exit...    --");
  println("Target broker: {}:{}", host, port);
  println("Subscribing to channels: {}", channels);
  println("Protocol: UDP (datagram-based)\n");

  signal(SIGINT, handleSignal);

  socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    println(stderr, "\033[31mSocket creation failed: {}\033[0m",
            strerror(errno));
    return EXIT_FAILURE;
  }

  // Set receive timeout to allow periodic checking of STOP_REQUESTED
  timeval timeout{};
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  if (::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
      0) {
    println(stderr, "\033[31mFailed to set socket timeout: {}\033[0m",
            strerror(errno));
    ::close(sock);
    return EXIT_FAILURE;
  }

  sockaddr_in brokerAddr{};
  brokerAddr.sin_family = AF_INET;
  brokerAddr.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &brokerAddr.sin_addr) <= 0) {
    println(stderr, "\033[31mInvalid address: {}\033[0m", strerror(errno));
    ::close(sock);
    return 1;
  }

  // Send handshake datagram to register as subscriber
  const auto handshake =
      channels == 0 ? string("[[SUB:ALL]]") : format("[[SUB:{}]]", channels);
  const auto handshakeSent =
      ::sendto(sock, handshake.data(), handshake.size(), 0,
               (sockaddr *)&brokerAddr, sizeof(brokerAddr));
  if (handshakeSent < 0) {
    println(stderr, "\033[31mFailed to send handshake: {}\033[0m",
            strerror(errno));
    ::close(sock);
    return 1;
  }
  println("\033[32mHandshake sent: {}\033[0m", handshake);
  println("Listening for messages...\n");

  array<char, MAX_UDP_PAYLOAD> buffer;
  sockaddr_in senderAddr{};
  socklen_t senderAddrLen = sizeof(senderAddr);

  while (!STOP_REQUESTED) {
    senderAddrLen = sizeof(senderAddr);
    auto received =
        ::recvfrom(sock, buffer.data(), buffer.size(), 0,
                   (sockaddr *)&senderAddr, &senderAddrLen);

    if (received < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        if (STOP_REQUESTED) {
          break;
        }
        continue;
      }
      println(stderr, "\033[31mReceive failed: {}\033[0m", strerror(errno));
      break;
    } else if (received == 0) {
      // UDP doesn't close connections, but we got an empty datagram
      continue;
    }

    string_view message(buffer.data(), received);

    // Check for EXIT message
    if (message.starts_with(EXIT_MESSAGE)) {
      println("\033[32mReceived EXIT message from broker\033[0m");
      STOP_REQUESTED = 1;
      break;
    }

    // Display received message
    char senderIP[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &senderAddr.sin_addr, senderIP, INET_ADDRSTRLEN);
    println("\033[36mReceived from {}:{} [{} bytes]: {}\033[0m", senderIP,
            ntohs(senderAddr.sin_port), received, message);
  }

  println("\n\033[33mSending EXIT message...\033[0m");
  ssize_t exitSent = ::sendto(sock, EXIT_MESSAGE.data(), EXIT_MESSAGE.size(), 0,
                               (sockaddr *)&brokerAddr, sizeof(brokerAddr));
  if (exitSent < 0) {
    println(stderr, "\033[31mFailed to send EXIT message: {}\033[0m",
            strerror(errno));
  } else {
    println("\033[32mEXIT message sent\033[0m");
  }

  ::close(sock);
  println("\nExiting subscriber...");
  return 0;
}

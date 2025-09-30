#include <boost/program_options.hpp>

import std;

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;
namespace po = boost::program_options;

namespace {
constexpr string_view EXIT_MESSAGE = "[[EXIT]]\n";

volatile sig_atomic_t stopRequested = 0;

void handleSignal(int signum) {
  if (signum == SIGINT) {
    stopRequested = 1;
  }
}
} // namespace

int main(int argc, char *argv[]) {
  string host;
  uint16_t port;
  bool help;

  po::options_description desc("Subscriber options");
  desc.add_options()("help,h", po::bool_switch(&help), "Show help message")(
      "host", po::value<string>(&host)->default_value("127.0.0.1"),
      "Broker host address")(
      "port,p", po::value<uint16_t>(&port)->default_value(5000), "Broker port");

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

  print(R"( ▄▄▄ █  ▐▌▗▖     ▄▄▄▄  ▄▄▄ ▄ ▄▄▄▄
█     ▀▄▄▞▘▐▌     █   █ █    ▄ █   █
▀▀▀▙       ▐▛▀▚▖  █▄▄▄▀ █    █ █   █
▄▄▄▛       ▐▙▄▞▘  █          █     ▗▄▖
                  ▀               ▐▌ ▐▌
                                   ▝▀▜▌
                                  ▐▙▄▞▘)");

  println("\n\n--    Press ctrl+c to exit...    --");
  println("Connecting to broker at {}:{}", host, port);

  signal(SIGINT, handleSignal);

  auto sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    println(stderr, "\033[31mSocket creation failed: {}\033[0m",
            strerror(errno));
    return EXIT_FAILURE;
  }

  const auto flags = ::fcntl(sock, F_GETFL, 0);
  if (flags < 0 || ::fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
    println(stderr, "\033[31mFailed to set socket non-blocking: {}\033[0m",
            strerror(errno));
    ::close(sock);
    return EXIT_FAILURE;
  }

  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = ::htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) <= 0) {
    println(stderr, "\033[31mInvalid address: {}\033[0m", strerror(errno));
    ::close(sock);
    return 1;
  }

  if (::bind(sock, (sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
    println(stderr, "\033[31mBind failed: {}\033[0m", strerror(errno));
    ::close(sock);
    return 1;
  }

  if (::listen(sock, SOMAXCONN) < 0) {
    println(stderr, "\033[31mListen failed: {}\033[0m", strerror(errno));
    ::close(sock);
    return 1;
  }

  println("\033[32mSubscriber listening on {}:{}\033[0m", host, port);

  println("Listening for messages...\n");

  array<char, 1024> buffer;

  while (!stopRequested) {
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);

    auto clientSock = ::accept(sock, (sockaddr *)&clientAddr, &clientLen);
    if (clientSock < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        // No incoming connections, sleep briefly and continue
        this_thread::sleep_for(chrono::milliseconds(100));
        continue;
      }
      if (errno == EINTR) {
        if (stopRequested) {
          break;
        }
        continue;
      }
      println(stderr, "\033[31mAccept failed: {}\033[0m", strerror(errno));
      break;
    }

    char clientIP[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
    println("\033[32mAccepted connection from {}:{}\033[0m", clientIP,
            ::ntohs(clientAddr.sin_port));

    // Keep receiving messages from this client until EXIT or disconnect
    bool clientDisconnected = false;
    while (!stopRequested && !clientDisconnected) {
      auto received = ::recv(clientSock, buffer.data(), buffer.size() - 1, 0);
      if (received < 0) {
        if (errno == EINTR) {
          if (stopRequested) {
            break;
          }
          continue;
        }
        println(stderr, "\033[31mReceive failed: {}\033[0m", strerror(errno));
        clientDisconnected = true;
      } else if (received == 0) {
        println("\033[33mConnection closed by client\033[0m");
        clientDisconnected = true;
      } else {
        buffer[received] = '\0';

        // Check for EXIT message
        if (string_view(buffer.data(), received) == EXIT_MESSAGE) {
          println("\033[32mReceived EXIT message - client disconnected "
                  "gracefully\033[0m");
          clientDisconnected = true;
        } else {
          println("\033[36mReceived [{} bytes]: {}\033[0m", received,
                  buffer.data());
        }
      }
    }

    ::close(clientSock);
  }

  ::close(sock);
  println("\nExiting subscriber...");
  return 0;
}
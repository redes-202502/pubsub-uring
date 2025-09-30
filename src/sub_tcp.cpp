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
constexpr string_view EXIT_MESSAGE = "[[EXIT]]\n";

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

  po::options_description desc("Subscriber options");
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

  print(R"( ▄▄▄ █  ▐▌▗▖       █  ▐▌ ▄▄▄ ▄ ▄▄▄▄    
▀▄▄  ▀▄▄▞▘▐▌       ▀▄▄▞▘█    ▄ █   █   
▄▄▄▀      ▐▛▀▚▖         █    █ █   █   
          ▐▙▄▞▘              █     ▗▄▖ 
                                  ▐▌ ▐▌
                                   ▝▀▜▌
                                  ▐▙▄▞▘)");

  println("\n\n--    Press ctrl+c to exit...    --");
  println("Connecting to broker at {}:{}", host, port);
  println("Subscribing to channels: {}", channels);

  signal(SIGINT, handleSignal);

  socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    println(stderr, "\033[31mSocket creation failed: {}\033[0m",
            strerror(errno));
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

  if (::connect(sock, (sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
    println(stderr, "\033[31mConnection failed: {}\033[0m", strerror(errno));
    ::close(sock);
    return 1;
  }

  println("\033[32mConnected to broker at {}:{}\033[0m", host, port);

  const auto handshake =
      channels == 0 ? string("[[SUB:ALL]]") : format("[[SUB:{}]]", channels);
  const auto handshakeSent =
      ::send(sock, handshake.data(), handshake.size(), 0);
  if (handshakeSent < 0) {
    println(stderr, "\033[31mFailed to send handshake: {}\033[0m",
            strerror(errno));
    ::close(sock);
    return 1;
  }
  println("\033[32mHandshake sent: {}\033[0m", handshake);
  println("Listening for messages...\n");

  array<char, 128> buffer;
  array<char, 512> recvBuffer;
  size_t recvBufferLen = 0;

  while (!STOP_REQUESTED) {
    auto received = ::recv(sock, buffer.data(), buffer.size(), 0);

    if (received < 0) {
      if (errno == EINTR) {
        if (STOP_REQUESTED) {
          break;
        }
        continue;
      }
      println(stderr, "\033[31mReceive failed: {}\033[0m", strerror(errno));
      break;
    } else if (received == 0) {
      println("\033[33mConnection closed by broker\033[0m");
      break;
    }

    // Append to buffer and process complete messages (lines)
    if (recvBufferLen + received > recvBuffer.size()) {
      println(stderr, "\033[31mReceive buffer overflow\033[0m");
      break;
    }
    memcpy(recvBuffer.data() + recvBufferLen, buffer.data(), received);
    recvBufferLen += received;

    while (true) {
      // Find newline in the buffer
      char *newline =
          static_cast<char *>(memchr(recvBuffer.data(), '\n', recvBufferLen));
      if (!newline) {
        break; // No complete message yet
      }

      size_t msgLen = newline - recvBuffer.data() + 1;
      string_view message(recvBuffer.data(), msgLen);

      // Check for EXIT message
      if (message.starts_with(EXIT_MESSAGE)) {
        println("\033[32mReceived EXIT message from broker\033[0m");
        STOP_REQUESTED = 1;
        break;
      }

      // Display received message
      println("\033[36mReceived: {}\033[0m",
              string_view(message.data(),
                          message.size() - 1)); // Strip newline for display

      // Remove processed message by shifting remaining data
      recvBufferLen -= msgLen;
      if (recvBufferLen > 0) {
        memmove(recvBuffer.data(), recvBuffer.data() + msgLen, recvBufferLen);
      }
    }
  }

  println("\n\033[33mSending EXIT message...\033[0m");
  ssize_t exitSent = ::send(sock, EXIT_MESSAGE.data(), EXIT_MESSAGE.size(), 0);
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

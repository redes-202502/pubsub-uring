#include <boost/program_options.hpp> // Weird, if put after modules import
// doesnt work

import std;
import MessageGenerator;

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;
namespace po = boost::program_options;

namespace {
constexpr string_view EXIT_MESSAGE = "[[EXIT]]\n";

volatile sig_atomic_t stopRequested = 0;

void handleSignal(int signum) {
  switch (signum) {
  case SIGINT:
    stopRequested = 1;
    break;
  case SIGPIPE:
    println(stderr,
            "\033[31mSIGPIPE: Connection closed by peer during write\033[0m");
    stopRequested = 1;
    break;
  }
}

bool handleSendError(ssize_t sent) {
  if (sent < 0) {
    println(stderr, "\033[31mSend failed: {}\033[0m", strerror(errno));
    return true;
  } else if (sent == 0) {
    println(stderr, "\033[31mConnection closed by peer during send\033[0m");
    return true;
  }
  return false;
}
} // namespace

int main(int argc, char *argv[]) {
  string host;
  uint16_t port;
  uint32_t seed;
  uint32_t delay_ms;
  bool help;

  po::options_description desc("Publisher options");
  desc.add_options()("help,h", po::bool_switch(&help), "Show help message")(
      "host", po::value<string>(&host)->default_value("127.0.0.1"),
      "Broker host address")(
      "port,p", po::value<uint16_t>(&port)->default_value(5000),
      "Broker port")("seed,s", po::value<uint32_t>(&seed)->default_value(0),
                     "Message generator seed (0 = random)")(
      "delay,d", po::value<uint32_t>(&delay_ms)->default_value(500),
      "Delay between messages in milliseconds");

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

  print(R"(▄▄▄▄  █  ▐▌▗▖       █  ▐▌ ▄▄▄ ▄ ▄▄▄▄
█   █ ▀▄▄▞▘▐▌       ▀▄▄▞▘█    ▄ █   █
█▄▄▄▀      ▐▛▀▚▖         █    █ █   █
█          ▐▙▄▞▘              █     ▗▄▖
▀                                  ▐▌ ▐▌
                                    ▝▀▜▌
                                   ▐▙▄▞▘)");

  println("\n\n--    Press ctrl+c to exit...    --");
  println("Connecting to {}:{}", host, port);
  if (seed != 0) {
    println("Using seed: {}", seed);
  }
  println("Message delay: {}ms\n", delay_ms);

  signal(SIGINT, handleSignal);
  signal(SIGPIPE, handleSignal);

  auto genMsg = misc::makeMessageGenerator(
      seed == 0 ? nullopt : optional<uint32_t>{seed});
  array<char, 128> buffer;

  auto sock = ::socket(AF_INET, SOCK_STREAM, 0);
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

  while (!stopRequested) {
    const auto n = genMsg.generateMessage(buffer.data(), buffer.size());
    println("Generated [{} bytes]: {}", n, buffer.data());

    uint32_t totalSent = 0;
    bool sendError = false;

    while (totalSent < n && !sendError) {
      auto sent = ::send(sock, buffer.data() + totalSent, n - totalSent, 0);
      if (sent < 0) {
        println(stderr, "\033[31mSend failed: {}\033[0m", strerror(errno));
        sendError = true;
      } else if (sent == 0) {
        println(stderr, "\033[31mConnection closed by peer during send\033[0m");
        sendError = true;
      } else {
        totalSent += sent;
        println("Sent {} bytes ({}/{} total)", sent, totalSent, n);
      }
    }
    if (sendError) {
      println("\033[31mMessage sending failed - exiting...\033[0m");
      break;
    }

    if (delay_ms != 0)
      this_thread::sleep_for(chrono::milliseconds(delay_ms));
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
  println("\nExiting program...");
  return 0;
}

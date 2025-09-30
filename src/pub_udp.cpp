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

using socket_t = int;

namespace {
constexpr string_view EXIT_MESSAGE = "[[EXIT]]";
constexpr size_t MAX_UDP_PAYLOAD = 1400;

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
  uint32_t seed;
  uint32_t delayMs;
  uint8_t channel;
  bool help;

  po::options_description desc("UDP Publisher options");
  desc.add_options()("help,h", po::bool_switch(&help), "Show help message")(
      "host", po::value<string>(&host)->default_value("127.0.0.1"),
      "Broker host address")(
      "port,p", po::value<uint16_t>(&port)->default_value(5000),
      "Broker port")("seed,s", po::value<uint32_t>(&seed)->default_value(0),
                     "Message generator seed (0 = random)")(
      "delay,d", po::value<uint32_t>(&delayMs)->default_value(500),
      "Delay between messages in milliseconds")(
      "channel,c", po::value<uint8_t>(&channel)->default_value(0),
      "Channel to publish on (0-255, default=0 broadcast)");

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

  print(R"(▄▄▄▄  █  ▐▌▗▖       █  ▐▌ ▄▄▄ ▄ ▄▄▄▄   █  ▐▌▗▖   ▄▄▄
█   █ ▀▄▄▞▘▐▌       ▀▄▄▞▘█    ▄ █   █  ▀▄▄▞▘▐▌  █   █
█▄▄▄▀      ▐▛▀▚▖         █    █ █   █       ▐▛▀▚▖█   █
█          ▐▙▄▞▘              █     ▗▄▖     ▐▙▄▞▘█▄▄▄▀
▀                                  ▐▌ ▐▌            █
                                    ▝▀▜▌            ▀
                                   ▐▙▄▞▘                )");

  println("\n\n--    Press ctrl+c to exit...    --");
  println("Target broker: {}:{}", host, port);
  println("Publishing on channel: {}", channel);
  if (seed != 0) {
    println("Using seed: {}", seed);
  }
  println("Message delay: {}ms", delayMs);
  println("Protocol: UDP (datagram-based)\n");

  signal(SIGINT, handleSignal);

  auto genMsg = misc::makeMessageGenerator(
      seed == 0 ? nullopt : optional<uint32_t>{seed});
  array<char, 128> buffer;

  socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    println(stderr, "\033[31mSocket creation failed: {}\033[0m",
            strerror(errno));
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

  // Send handshake datagram to register as publisher
  const auto handshake = format("[[PUB:{}]]", channel);
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

  while (!STOP_REQUESTED) {
    const auto n = genMsg.generateMessage(buffer.data(), buffer.size());
    println("Generated [{} bytes]: {}", n, buffer.data());

    // Format message with channel prefix: [CH:N]message
    string formattedMsg =
        format("[CH:{}]{}", channel, string_view(buffer.data(), n));

    if (formattedMsg.size() > MAX_UDP_PAYLOAD) {
      println(stderr, "\033[31mMessage too large for UDP ({} > {} bytes), "
                      "truncating\033[0m",
              formattedMsg.size(), MAX_UDP_PAYLOAD);
      formattedMsg.resize(MAX_UDP_PAYLOAD);
    }

    auto sent = ::sendto(sock, formattedMsg.data(), formattedMsg.size(), 0,
                         (sockaddr *)&brokerAddr, sizeof(brokerAddr));
    if (sent < 0) {
      println(stderr, "\033[31mSend failed: {}\033[0m", strerror(errno));
      break;
    } else {
      println("Sent {} bytes via UDP datagram", sent);
    }

    if (delayMs != 0)
      this_thread::sleep_for(chrono::milliseconds(delayMs));
  }

  println("\n\033[33mSending EXIT message...\033[0m");
  const auto exitSent = ::sendto(sock, EXIT_MESSAGE.data(), EXIT_MESSAGE.size(),
                                  0, (sockaddr *)&brokerAddr, sizeof(brokerAddr));
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

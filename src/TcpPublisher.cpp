#include <charconv>
#include <format>
#include <print>
#include <string>
#include <thread>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liburing.h>

#include "MessageGenerator.hpp"
#include "Proto.hpp"

using namespace std;

using socket_t = int;

namespace {
constexpr uint32_t QUEUE_DEPTH = 64;
constexpr uint32_t MAX_MESSAGE_SIZE = 1024;

volatile sig_atomic_t STOP_REQUESTED = 0;

struct Options {
  string host = "127.0.0.1";
  uint16_t port = 5000;
  uint32_t seed = 0;
  uint32_t delayMs = 500;
  uint8_t channel = 0;
  string clientId = "publisher";
  bool help = false;
};

void handleSignal(int signum) {
  switch (signum) {
  case SIGINT:
    STOP_REQUESTED = 1;
    break;
  case SIGPIPE:
    println(stderr, "\033[31mSIGPIPE: Connection closed by peer during "
                    "write\033[0m");
    STOP_REQUESTED = 1;
    break;
  }
}

template <typename T> optional<T> parseNumber(string_view str) {
  T value;
  auto [ptr, ec] = from_chars(str.data(), str.data() + str.size(), value);
  if (ec == errc())
    return value;
  return nullopt;
}

optional<Options> parseArgs(int argc, char **argv) {
  Options opts;

  for (uint32_t i = 1; i < argc; ++i) {
    string_view arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      opts.help = true;
    } else if (arg == "--host") {
      if (i + 1 < argc) {
        opts.host = argv[++i];
      } else {
        println(stderr, "Error: Missing value for --host");
        return nullopt;
      }
    } else if (arg == "--port" || arg == "-p") {
      if (i + 1 < argc) {
        auto portOpt = parseNumber<uint16_t>(argv[++i]);
        if (portOpt) {
          opts.port = *portOpt;
        } else {
          println(stderr, "Error: Invalid value for --port");
          return nullopt;
        }
      } else {
        println(stderr, "Error: Missing value for --port");
        return nullopt;
      }
    } else if (arg == "--seed" || arg == "-s") {
      if (i + 1 < argc) {
        auto seedOpt = parseNumber<uint32_t>(argv[++i]);
        if (seedOpt) {
          opts.seed = *seedOpt;
        } else {
          println(stderr, "Error: Invalid value for --seed");
          return nullopt;
        }
      } else {
        println(stderr, "Error: Missing value for --seed");
        return nullopt;
      }
    } else if (arg == "--delay" || arg == "-d") {
      if (i + 1 < argc) {
        auto delayOpt = parseNumber<uint32_t>(argv[++i]);
        if (delayOpt) {
          opts.delayMs = *delayOpt;
        } else {
          println(stderr, "Error: Invalid value for --delay");
          return nullopt;
        }
      } else {
        println(stderr, "Error: Missing value for --delay");
        return nullopt;
      }
    } else if (arg == "--channel" || arg == "-c") {
      if (i + 1 < argc) {
        auto channelOpt = parseNumber<uint8_t>(argv[++i]);
        if (channelOpt) {
          opts.channel = *channelOpt;
        } else {
          println(stderr, "Error: Invalid value for --channel");
          return nullopt;
        }
      } else {
        println(stderr, "Error: Missing value for --channel");
        return nullopt;
      }
    } else if (arg == "--client-id") {
      if (i + 1 < argc) {
        opts.clientId = argv[++i];
      } else {
        println(stderr, "Error: Missing value for --client-id");
        return nullopt;
      }
    } else {
      println(stderr, "Error: Unknown option '{}'", arg);
      return nullopt;
    }
  }

  return opts;
}

void printHelp() {
  println("Publisher options:");
  println("  -h, --help              Show help message");
  println("  --host <host>           Broker host address (default: 127.0.0.1)");
  println("  -p, --port <port>       Broker port (default: 5000)");
  println("  -s, --seed <seed>       Message generator seed (0 = random, default: 0)");
  println("  -d, --delay <ms>        Delay between messages in milliseconds (default: 500)");
  println("  -c, --channel <channel> Channel to publish on (0-255, default: 0)");
  println("  --client-id <id>        Client identifier (default: publisher)");
}

void printBanner() {
  print(R"(   ■  ▗▞▀▘▄▄▄▄      ▄▄▄▄  █  ▐▌▗▖   █ ▄  ▄▄▄ ▐▌   ▗▞▀▚▖ ▄▄▄
▗▄▟▙▄▖▝▚▄▖█   █     █   █ ▀▄▄▞▘▐▌   █ ▄ ▀▄▄  ▐▌   ▐▛▀▀▘█
  ▐▌      █▄▄▄▀     █▄▄▄▀      ▐▛▀▚▖█ █ ▄▄▄▀ ▐▛▀▚▖▝▚▄▄▖█
  ▐▌      █         █          ▐▙▄▞▘█ █      ▐▌ ▐▌
  ▐▌      ▀         ▀
)");
}
} // namespace

int main(int argc, char **argv) {
  printBanner();

  auto optsR = parseArgs(argc, argv);
  if (!optsR) {
    println("Use --help for usage.");
    return 1;
  }

  const Options &opts = *optsR;

  if (opts.help) {
    printHelp();
    return 0;
  }

  println("\n\n--    Press ctrl+c to exit...    --");
  println("Connecting to {}:{}", opts.host, opts.port);
  println("Publishing on channel: {}", opts.channel);
  println("Client ID: {}", opts.clientId);
  if (opts.seed != 0) {
    println("Using seed: {}", opts.seed);
  }
  println("Message delay: {}ms\n", opts.delayMs);

  signal(SIGINT, handleSignal);
  signal(SIGPIPE, handleSignal);

  socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    println(stderr, "\033[31mSocket creation failed: {}\033[0m",
            strerror(errno));
    return EXIT_FAILURE;
  }

  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = ::htons(opts.port);
  if (::inet_pton(AF_INET, opts.host.c_str(), &serverAddr.sin_addr) <= 0) {
    println(stderr, "\033[31mInvalid address: {}\033[0m", strerror(errno));
    ::close(sock);
    return 1;
  }

  io_uring ring;
  auto ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ret < 0) {
    println(stderr, "\033[31mio_uring initialization failed: {}\033[0m",
            strerror(-ret));
    ::close(sock);
    return 1;
  }

  if (::connect(sock, reinterpret_cast<sockaddr *>(&serverAddr), sizeof(serverAddr)) < 0) {
    println(stderr, "\033[31mConnection failed: {}\033[0m", strerror(errno));
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  println("\033[32mConnected to broker at {}:{}\033[0m", opts.host, opts.port);

  MessageEncoder encoder;
  vector<byte> handshakeBuffer(
      encoder.sizeHandshakePub(opts.clientId));
  encoder.encodeHandshakePub(handshakeBuffer.data(), opts.channel,
                             opts.clientId);

  auto *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    println(stderr, "\033[31mFailed to get SQE for handshake\033[0m");
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  io_uring_prep_send(sqe, sock, handshakeBuffer.data(), handshakeBuffer.size(),
                     0);

  ret = io_uring_submit(&ring);
  if (ret < 0) {
    println(stderr, "\033[31mFailed to submit handshake: {}\033[0m",
            strerror(-ret));
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  io_uring_cqe *cqe;
  ret = io_uring_wait_cqe(&ring, &cqe);
  if (ret < 0) {
    println(stderr, "\033[31mError waiting for handshake completion: {}\033[0m",
            strerror(-ret));
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  if (cqe->res < 0) {
    println(stderr, "\033[31mHandshake send failed: {}\033[0m",
            strerror(-cqe->res));
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  println("\033[32mHandshake sent ({} bytes)\033[0m", cqe->res);
  io_uring_cqe_seen(&ring, cqe);

  vector<byte> ackBuffer(512);
  sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    println(stderr, "\033[31mFailed to get SQE for handshake ACK\033[0m");
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  io_uring_prep_recv(sqe, sock, ackBuffer.data(), ackBuffer.size(), 0);

  ret = io_uring_submit(&ring);
  if (ret < 0) {
    println(stderr, "\033[31mFailed to submit handshake ACK recv: {}\033[0m",
            strerror(-ret));
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  ret = io_uring_wait_cqe(&ring, &cqe);
  if (ret < 0) {
    println(stderr,
            "\033[31mError waiting for handshake ACK completion: {}\033[0m",
            strerror(-ret));
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  if (cqe->res <= 0) {
    println(stderr, "\033[31mHandshake ACK recv failed: {}\033[0m",
            cqe->res < 0 ? strerror(-cqe->res) : "Connection closed");
    io_uring_cqe_seen(&ring, cqe);
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  MessageDecoder decoder;
  auto parseResult = decoder.decode(span<const byte>(ackBuffer.data(), cqe->res));
  io_uring_cqe_seen(&ring, cqe);

  if (!parseResult.message) {
    println(stderr, "\033[31mFailed to parse handshake ACK\033[0m");
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  if (parseResult.message->opcode != OpCode::HANDSHAKE_ACK) {
    println(stderr, "\033[31mUnexpected response opcode: {}\033[0m",
            static_cast<int>(parseResult.message->opcode));
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  println("\033[32mHandshake acknowledged\033[0m");

  auto genMsg = misc::makeMessageGenerator(
      opts.seed == 0 ? nullopt : optional<uint32_t>{opts.seed});
  array<char, MAX_MESSAGE_SIZE> msgBuffer;

  while (!STOP_REQUESTED) {
    const auto msgLen = genMsg.generateMessage(msgBuffer.data(), msgBuffer.size());
    println("Generated [{} bytes]: {}", msgLen, msgBuffer.data());

    span<const byte> msgSpan(reinterpret_cast<const byte *>(msgBuffer.data()),
                             msgLen);
    vector<byte> publishBuffer(encoder.sizePublish(msgSpan));
    encoder.encodePublish(publishBuffer.data(), opts.channel, msgSpan);

    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for message send\033[0m");
      break;
    }

    io_uring_prep_send(sqe, sock, publishBuffer.data(), publishBuffer.size(),
                       0);

    ret = io_uring_submit(&ring);
    if (ret < 0) {
      println(stderr, "\033[31mFailed to submit message: {}\033[0m",
              strerror(-ret));
      break;
    }

    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      println(stderr, "\033[31mError waiting for send completion: {}\033[0m",
              strerror(-ret));
      break;
    }

    if (cqe->res < 0) {
      println(stderr, "\033[31mSend failed: {}\033[0m", strerror(-cqe->res));
      io_uring_cqe_seen(&ring, cqe);
      break;
    } else if (cqe->res == 0) {
      println(stderr, "\033[31mConnection closed by peer during send\033[0m");
      io_uring_cqe_seen(&ring, cqe);
      break;
    }

    println("Sent {} bytes", cqe->res);
    io_uring_cqe_seen(&ring, cqe);

    if (opts.delayMs != 0)
      this_thread::sleep_for(chrono::milliseconds(opts.delayMs));
  }

  println("\n\033[33mSending DISCONNECT message...\033[0m");
  vector<byte> disconnectBuffer(encoder.sizeDisconnect());
  encoder.encodeDisconnect(disconnectBuffer.data());

  sqe = io_uring_get_sqe(&ring);
  if (sqe) {
    io_uring_prep_send(sqe, sock, disconnectBuffer.data(),
                       disconnectBuffer.size(), 0);

    ret = io_uring_submit(&ring);
    if (ret >= 0) {
      ret = io_uring_wait_cqe(&ring, &cqe);
      if (ret >= 0) {
        if (cqe->res > 0) {
          println("\033[32mDISCONNECT message sent\033[0m");
        }
        io_uring_cqe_seen(&ring, cqe);
      }
    }
  }

  io_uring_queue_exit(&ring);
  ::close(sock);
  println("\nExiting program...");

  return 0;
}

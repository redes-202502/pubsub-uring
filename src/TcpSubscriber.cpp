#include <charconv>
#include <format>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liburing.h>

#include "Proto.hpp"

using namespace std;

using socket_t = int;

namespace {
constexpr uint32_t QUEUE_DEPTH = 64;
constexpr uint32_t RECV_BUFFER_SIZE = 4096;

volatile sig_atomic_t STOP_REQUESTED = 0;

struct Options {
  string host = "127.0.0.1";
  uint16_t port = 5000;
  vector<uint8_t> channels = {0}; // Default subscribe to channel 0
  string clientId = "subscriber";
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
  bool channelsSet = false;

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
    } else if (arg == "--channels" || arg == "-c") {
      if (i + 1 < argc) {
        string channelsStr = argv[++i];
        opts.channels.clear();
        channelsSet = true;

        size_t start = 0;
        while (start < channelsStr.size()) {
          size_t comma = channelsStr.find(',', start);
          size_t end = (comma == string::npos) ? channelsStr.size() : comma;

          auto channelOpt = parseNumber<uint8_t>(
              string_view(channelsStr.data() + start, end - start));
          if (channelOpt) {
            opts.channels.push_back(*channelOpt);
          } else {
            println(stderr, "Error: Invalid channel in list");
            return nullopt;
          }

          start = (comma == string::npos) ? channelsStr.size() : comma + 1;
        }
      } else {
        println(stderr, "Error: Missing value for --channels");
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

  if (opts.channels.empty()) {
    println(stderr, "Error: At least one channel must be specified");
    return nullopt;
  }

  return opts;
}

void printHelp() {
  println("Subscriber options:");
  println("  -h, --help              Show help message");
  println("  --host <host>           Broker host address (default: 127.0.0.1)");
  println("  -p, --port <port>       Broker port (default: 5000)");
  println("  -c, --channels <list>   Comma-separated channels to subscribe "
          "(default: 0)");
  println("  --client-id <id>        Client identifier (default: subscriber)");
}

void printBanner() {
  print(R"(   ■  ▗▞▀▘▄▄▄▄       ▄▄▄ █  ▐▌▗▖    ▄▄▄ ▗▞▀▘ ▄▄▄ ▄ ▗▖   ▗▞▀▚▖ ▄▄▄
▗▄▟▙▄▖▝▚▄▖█   █     ▀▄▄  ▀▄▄▞▘▐▌   ▀▄▄  ▝▚▄▖█    ▄ ▐▌   ▐▛▀▀▘█
  ▐▌      █▄▄▄▀     ▄▄▄▀      ▐▛▀▚▖▄▄▄▀     █    █ ▐▛▀▚▖▝▚▄▄▖█
  ▐▌      █                   ▐▙▄▞▘              █ ▐▙▄▞▘
  ▐▌      ▀
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
  print("Subscribing to channels: ");
  for (size_t i = 0; i < opts.channels.size(); ++i) {
    if (i > 0)
      print(", ");
    print("{}", opts.channels[i]);
  }
  println("");
  println("Client ID: {}\n", opts.clientId);

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
  int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
  if (ret < 0) {
    println(stderr, "\033[31mio_uring initialization failed: {}\033[0m",
            strerror(-ret));
    ::close(sock);
    return 1;
  }

  if (::connect(sock, reinterpret_cast<sockaddr *>(&serverAddr),
                sizeof(serverAddr)) < 0) {
    println(stderr, "\033[31mConnection failed: {}\033[0m", strerror(errno));
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  println("\033[32mConnected to broker at {}:{}\033[0m", opts.host, opts.port);

  MessageEncoder encoder;
  span<const uint8_t> channelsSpan(opts.channels.data(), opts.channels.size());
  vector<byte> handshakeBuffer(
      encoder.sizeHandshakeSub(channelsSpan, opts.clientId));
  encoder.encodeHandshakeSub(handshakeBuffer.data(), channelsSpan,
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
  auto parseResult =
      decoder.decode(span<const byte>(ackBuffer.data(), cqe->res));
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
  println("Listening for messages...\n");

  vector<byte> recvBuffer(RECV_BUFFER_SIZE);
  vector<byte> messageBuffer;

  while (!STOP_REQUESTED) {
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for recv\033[0m");
      break;
    }

    io_uring_prep_recv(sqe, sock, recvBuffer.data(), recvBuffer.size(), 0);

    ret = io_uring_submit(&ring);
    if (ret < 0) {
      println(stderr, "\033[31mFailed to submit recv: {}\033[0m",
              strerror(-ret));
      break;
    }

    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0) {
      println(stderr, "\033[31mError waiting for recv completion: {}\033[0m",
              strerror(-ret));
      break;
    }

    if (cqe->res < 0) {
      println(stderr, "\033[31mRecv failed: {}\033[0m", strerror(-cqe->res));
      io_uring_cqe_seen(&ring, cqe);
      break;
    } else if (cqe->res == 0) {
      println("\033[33mConnection closed by broker\033[0m");
      io_uring_cqe_seen(&ring, cqe);
      break;
    }

    messageBuffer.insert(messageBuffer.end(), recvBuffer.begin(),
                         recvBuffer.begin() + cqe->res);
    io_uring_cqe_seen(&ring, cqe);

    uint32_t offset = 0;
    while (offset < messageBuffer.size()) {
      span<const byte> dataSpan(messageBuffer.data() + offset,
                                messageBuffer.size() - offset);
      auto result = decoder.decode(dataSpan);

      if (result.needMoreData) {
        // Not enough data for complete message, wait for more
        break;
      }

      if (!result.message) {
        println(stderr, "\033[31mFailed to parse message\033[0m");
        STOP_REQUESTED = 1;
        break;
      }

      const auto &msg = *result.message;

      switch (msg.opcode) {
      case OpCode::MESSAGE: {
        if (msg.payloadLen >= 9) {
          uint8_t channel = static_cast<uint8_t>(msg.payload[0]);
          uint64_t timestamp;
          memcpy(&timestamp, msg.payload + 1, 8);

          span<const byte> messageData(msg.payload + 9, msg.payloadLen - 9);
          string messageStr(reinterpret_cast<const char *>(messageData.data()),
                            messageData.size());

          println("\033[36m[Channel {}] [{}] {}\033[0m", channel, timestamp,
                  messageStr);
        }
        break;
      }
      case OpCode::ERROR: {
        if (msg.payloadLen >= 1) {
          uint8_t errorCode = static_cast<uint8_t>(msg.payload[0]);
          println(stderr, "\033[31mReceived ERROR from broker: {}\033[0m",
                  errorCode);
        }
        break;
      }
      case OpCode::DISCONNECT: {
        println("\033[33mReceived DISCONNECT from broker\033[0m");
        STOP_REQUESTED = 1;
        break;
      }
      default:
        println(stderr, "\033[33mUnexpected opcode: {}\033[0m",
                static_cast<int>(msg.opcode));
        break;
      }

      offset += result.bytesConsumed;
    }

    if (offset > 0) {
      messageBuffer.erase(messageBuffer.begin(),
                          messageBuffer.begin() + offset);
    }
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

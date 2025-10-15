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
  print(R"(█  ▐▌▐▌▄▄▄▄       ▄▄▄ █  ▐▌▗▖    ▄▄▄ ▗▞▀▘ ▄▄▄ ▄ ▗▖   ▗▞▀▚▖ ▄▄▄
▀▄▄▞▘▐▌█   █     ▀▄▄  ▀▄▄▞▘▐▌   ▀▄▄  ▝▚▄▖█    ▄ ▐▌   ▐▛▀▀▘█
  ▗▞▀▜▌█▄▄▄▀     ▄▄▄▀      ▐▛▀▚▖▄▄▄▀     █    █ ▐▛▀▚▖▝▚▄▄▖█
  ▝▚▄▟▌█                   ▐▙▄▞▘              █ ▐▙▄▞▘
       ▀
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

  socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
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

  println("\033[32mUDP socket created\033[0m");

  MessageEncoder encoder;
  span<const uint8_t> channelsSpan(opts.channels.data(), opts.channels.size());
  vector<byte> handshakeBuffer(
      encoder.sizeHandshakeSub(channelsSpan, opts.clientId));
  encoder.encodeHandshakeSub(handshakeBuffer.data(), channelsSpan,
                             opts.clientId);

  msghdr msg{};
  iovec iov{};
  iov.iov_base = handshakeBuffer.data();
  iov.iov_len = handshakeBuffer.size();

  msg.msg_name = &serverAddr;
  msg.msg_namelen = sizeof(serverAddr);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  auto *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    println(stderr, "\033[31mFailed to get SQE for handshake\033[0m");
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  io_uring_prep_sendmsg(sqe, sock, &msg, 0);

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
  sockaddr_in recvAddr{};

  msghdr recvMsg{};
  iovec recvIov{};
  recvIov.iov_base = ackBuffer.data();
  recvIov.iov_len = ackBuffer.size();

  recvMsg.msg_name = &recvAddr;
  recvMsg.msg_namelen = sizeof(recvAddr);
  recvMsg.msg_iov = &recvIov;
  recvMsg.msg_iovlen = 1;

  sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    println(stderr, "\033[31mFailed to get SQE for handshake ACK\033[0m");
    io_uring_queue_exit(&ring);
    ::close(sock);
    return 1;
  }

  io_uring_prep_recvmsg(sqe, sock, &recvMsg, 0);

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
            cqe->res < 0 ? strerror(-cqe->res) : "No response");
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

  while (!STOP_REQUESTED) {
    sockaddr_in msgAddr{};

    iovec msgIov{};
    msgIov.iov_base = recvBuffer.data();
    msgIov.iov_len = recvBuffer.size();

    msghdr msgHdr{};
    msgHdr.msg_name = &msgAddr;
    msgHdr.msg_namelen = sizeof(msgAddr);
    msgHdr.msg_iov = &msgIov;
    msgHdr.msg_iovlen = 1;

    sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for recv\033[0m");
      break;
    }

    io_uring_prep_recvmsg(sqe, sock, &msgHdr, 0);

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
      println("\033[33mReceived 0 bytes\033[0m");
      io_uring_cqe_seen(&ring, cqe);
      continue;
    }

    span<const byte> dataSpan(recvBuffer.data(), cqe->res);
    auto result = decoder.decode(dataSpan);
    io_uring_cqe_seen(&ring, cqe);

    if (result.needMoreData) {
      if (result.needMoreData) {
        println(stderr, "\033[33m[WARN] Incomplete message received\033[0m");
      }
      continue;
    }

    if (!result.message) {
      println(stderr, "\033[31mFailed to parse message\033[0m");
      continue;
    }

    const auto &parsedMsg = *result.message;

    switch (parsedMsg.opcode) {
    case OpCode::MESSAGE: {
      if (parsedMsg.payloadLen >= 9) {
        uint8_t channel = static_cast<uint8_t>(parsedMsg.payload[0]);
        uint64_t timestamp;
        memcpy(&timestamp, parsedMsg.payload + 1, 8);

        span<const byte> messageData(parsedMsg.payload + 9,
                                     parsedMsg.payloadLen - 9);
        string messageStr(reinterpret_cast<const char *>(messageData.data()),
                          messageData.size());

        println("\033[36m[Channel {}] [{}] {}\033[0m", channel, timestamp,
                messageStr);
      }
      break;
    }
    case OpCode::ERROR: {
      if (parsedMsg.payloadLen >= 1) {
        uint8_t errorCode = static_cast<uint8_t>(parsedMsg.payload[0]);
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
              static_cast<int>(parsedMsg.opcode));
      break;
    }
  }

  println("\n\033[33mSending DISCONNECT message...\033[0m");
  vector<byte> disconnectBuffer(encoder.sizeDisconnect());
  encoder.encodeDisconnect(disconnectBuffer.data());

  iovec disconnIov{};
  disconnIov.iov_base = disconnectBuffer.data();
  disconnIov.iov_len = disconnectBuffer.size();

  msghdr disconnMsg{};
  disconnMsg.msg_name = &serverAddr;
  disconnMsg.msg_namelen = sizeof(serverAddr);
  disconnMsg.msg_iov = &disconnIov;
  disconnMsg.msg_iovlen = 1;

  sqe = io_uring_get_sqe(&ring);
  if (sqe) {
    io_uring_prep_sendmsg(sqe, sock, &disconnMsg, 0);

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

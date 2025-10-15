#include <bitset>
#include <charconv>
#include <chrono>
#include <functional>
#include <map>
#include <print>
#include <queue>
#include <string>
#include <vector>

#include <cerrno>
#include <csignal>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liburing.h>

#include "Proto.hpp"

using namespace std;

using socket_t = int;

namespace {
constexpr uint32_t QUEUE_DEPTH = 256;
constexpr uint32_t RECV_BUFFER_SIZE = 4096;
constexpr uint32_t MAX_SEND_QUEUE = 256;

volatile sig_atomic_t STOP_REQUESTED = 0;

enum class ClientType { UNKNOWN, PUBLISHER, SUBSCRIBER };

enum class OpType : uint64_t {
  RECV = 1,
  SEND = 2,
};

// Helper to compare sockaddr_in for use in maps
struct SockAddrCmp {
  bool operator()(const sockaddr_in &a, const sockaddr_in &b) const {
    if (a.sin_addr.s_addr != b.sin_addr.s_addr)
      return a.sin_addr.s_addr < b.sin_addr.s_addr;
    return a.sin_port < b.sin_port;
  }
};

struct Client {
  sockaddr_in addr;
  ClientType type;
  bitset<256> channels;
  queue<vector<byte>> sendQueue;
  string clientId;

  Client() : type(ClientType::UNKNOWN) { memset(&addr, 0, sizeof(addr)); }

  explicit Client(const sockaddr_in &a) : addr(a), type(ClientType::UNKNOWN) {}
};

inline uint64_t makeUserData(OpType op) { return static_cast<uint64_t>(op); }

inline OpType parseUserData(uint64_t user_data) {
  return static_cast<OpType>(user_data);
}

struct Options {
  string host = "127.0.0.1";
  uint16_t port = 5000;
  bool verbose = false;
  bool help = false;
};

template <typename T> optional<T> parseNumber(string_view str) {
  T value;
  auto [ptr, ec] = from_chars(str.data(), str.data() + str.size(), value);
  if (ec == errc())
    return value;
  return nullopt;
}

optional<Options> parseArgs(int argc, char **argv) {
  Options opts;

  for (int i = 1; i < argc; ++i) {
    string_view arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      opts.help = true;
    } else if (arg == "--verbose" || arg == "-v") {
      opts.verbose = true;
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
    } else {
      println(stderr, "Error: Unknown option '{}'", arg);
      return nullopt;
    }
  }

  return opts;
}

void printHelp() {
  println("Broker options:");
  println("  -h, --help           Show help message");
  println("  --host <host>        Listen host address (default: 127.0.0.1)");
  println("  -p, --port <port>    Listen port (default: 5000)");
  println("  -v, --verbose        Enable verbose logging");
}

void printBanner() {
  print(R"(█  ▐▌▐▌▄▄▄▄      ▗▖    ▄▄▄ ▄▄▄  █  ▄ ▗▞▀▚▖ ▄▄▄
▀▄▄▞▘▐▌█   █     ▐▌   █   █   █ █▄▀  ▐▛▀▀▘█
  ▗▞▀▜▌█▄▄▄▀     ▐▛▀▚▖█   ▀▄▄▄▀ █ ▀▄ ▝▚▄▄▖█
  ▝▚▄▟▌█         ▐▙▄▞▘          █  █
       ▀
)");
}

void handleSignal(int signum) {
  if (signum == SIGINT) {
    STOP_REQUESTED = 1;
  }
}

string addrToString(const sockaddr_in &addr) {
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
  return format("{}:{}", ip, ntohs(addr.sin_port));
}
} // namespace

class Broker {
private:
  io_uring ring;
  socket_t sock;
  map<sockaddr_in, Client, SockAddrCmp> clients;
  array<vector<sockaddr_in>, 256> channelSubscribers;
  bool verbose;

  vector<byte> recvBuffer;
  sockaddr_in recvAddr;
  socklen_t recvAddrLen;
  msghdr recvMsgHdr;
  iovec recvIovec;

  vector<byte> sendBuffer;
  sockaddr_in sendAddr;
  msghdr sendMsgHdr;
  iovec sendIovec;

  MessageEncoder encoder;
  MessageDecoder decoder;
  uint64_t sessionIdCounter;

  bool sendInProgress;

public:
  explicit Broker(bool verbose)
      : sock(-1), verbose(verbose), recvAddrLen(sizeof(recvAddr)),
        sessionIdCounter(1), sendInProgress(false) {
    int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    if (ret < 0) {
      throw runtime_error(
          format("io_uring initialization failed: {}", strerror(-ret)));
    }
    recvBuffer.resize(RECV_BUFFER_SIZE);
    memset(&recvAddr, 0, sizeof(recvAddr));
    memset(&recvMsgHdr, 0, sizeof(recvMsgHdr));
    memset(&recvIovec, 0, sizeof(recvIovec));
    memset(&sendAddr, 0, sizeof(sendAddr));
    memset(&sendMsgHdr, 0, sizeof(sendMsgHdr));
    memset(&sendIovec, 0, sizeof(sendIovec));
  }

  ~Broker() {
    if (sock >= 0) {
      ::close(sock);
    }
    io_uring_queue_exit(&ring);
  }

  void setupSocket(const string &host, uint16_t port) {
    sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
      throw runtime_error(
          format("Socket creation failed: {}", strerror(errno)));
    }

    int opt = 1;
    if (::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
      ::close(sock);
      throw runtime_error(format("setsockopt failed: {}", strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
      ::close(sock);
      throw runtime_error(format("Invalid address: {}", host));
    }

    if (::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      ::close(sock);
      throw runtime_error(format("Bind failed: {}", strerror(errno)));
    }

    println("\033[32mUDP Broker listening on {}:{}\033[0m", host, port);
  }

  Client *getClient(const sockaddr_in &addr) {
    auto it = clients.find(addr);
    return it != clients.end() ? &it->second : nullptr;
  }

  Client *getOrCreateClient(const sockaddr_in &addr) {
    auto it = clients.find(addr);
    if (it == clients.end()) {
      auto [inserted, _] = clients.emplace(addr, Client(addr));
      if (verbose) {
        println("\033[36m[+] Client {} added\033[0m", addrToString(addr));
      }
      return &inserted->second;
    }
    return &it->second;
  }

  void subscribeToChannel(const sockaddr_in &addr, uint8_t channel) {
    auto *client = getClient(addr);
    if (!client)
      return;

    client->channels.set(channel);
    auto &subs = channelSubscribers[channel];
    auto pred = [&addr](const sockaddr_in &a) {
      return a.sin_addr.s_addr == addr.sin_addr.s_addr &&
             a.sin_port == addr.sin_port;
    };
    if (find_if(subs.begin(), subs.end(), pred) == subs.end()) {
      subs.push_back(addr);
    }

    if (verbose) {
      println("\033[33m[SUB] {} subscribed to channel {}\033[0m",
              addrToString(addr), channel);
    }
  }

  void handleHandshake(const sockaddr_in &addr, const DecodedMessage &msg) {
    auto *client = getOrCreateClient(addr);

    if (msg.opcode == OpCode::HANDSHAKE_PUB) {
      // Parse: channel(1) + clientIdLen(1) + clientId
      if (msg.payloadLen >= 2) {
        uint8_t channel = static_cast<uint8_t>(msg.payload[0]);
        uint8_t clientIdLen = static_cast<uint8_t>(msg.payload[1]);

        if (msg.payloadLen >= 2 + clientIdLen) {
          client->clientId = string(
              reinterpret_cast<const char *>(msg.payload + 2), clientIdLen);
          client->type = ClientType::PUBLISHER;
          client->channels.set(channel);

          println("\033[32m[HANDSHAKE] {} ({}) registered as PUBLISHER on "
                  "channel {}\033[0m",
                  addrToString(addr), client->clientId, channel);

          // Send HANDSHAKE_ACK
          vector<byte> ackBuffer(encoder.sizeHandshakeAck());
          encoder.encodeHandshakeAck(ackBuffer.data(), 0, sessionIdCounter++);
          enqueueMessage(addr, std::move(ackBuffer));
        }
      }
    } else if (msg.opcode == OpCode::HANDSHAKE_SUB) {
      // Parse: channelCount(1) + channels[...] + clientIdLen(1) + clientId
      if (msg.payloadLen >= 2) {
        uint8_t channelCount = static_cast<uint8_t>(msg.payload[0]);

        if (msg.payloadLen >= 1 + channelCount + 1) {
          client->type = ClientType::SUBSCRIBER;

          for (uint8_t i = 0; i < channelCount; ++i) {
            uint8_t channel = static_cast<uint8_t>(msg.payload[1 + i]);
            subscribeToChannel(addr, channel);
          }

          // Extract client ID
          uint8_t clientIdLen =
              static_cast<uint8_t>(msg.payload[1 + channelCount]);
          if (msg.payloadLen >= 1 + channelCount + 1 + clientIdLen) {
            client->clientId = string(reinterpret_cast<const char *>(
                                          msg.payload + 1 + channelCount + 1),
                                      clientIdLen);
          }

          print("\033[32m[HANDSHAKE] {} ({}) registered as SUBSCRIBER on "
                "channels: ",
                addrToString(addr), client->clientId);
          for (size_t ch = 0; ch < 256; ++ch) {
            if (client->channels.test(ch)) {
              print("{} ", ch);
            }
          }
          println("\033[0m");

          // Send HANDSHAKE_ACK
          vector<byte> ackBuffer(encoder.sizeHandshakeAck());
          encoder.encodeHandshakeAck(ackBuffer.data(), 0, sessionIdCounter++);
          enqueueMessage(addr, std::move(ackBuffer));
        }
      }
    }
  }

  void processMessage(const sockaddr_in &addr, span<const byte> data) {
    auto parseResult = decoder.decode(data);

    if (parseResult.needMoreData) {
      if (verbose) {
        println("\033[33m[WARN] Incomplete message from {}\033[0m",
                addrToString(addr));
      }
      return;
    }

    if (!parseResult.message) {
      if (verbose) {
        println(stderr,
                "\033[31m[ERROR] Failed to parse message from {}\033[0m",
                addrToString(addr));
      }
      return;
    }

    const auto &msg = *parseResult.message;
    auto *client = getClient(addr);

    switch (msg.opcode) {
    case OpCode::HANDSHAKE_PUB:
    case OpCode::HANDSHAKE_SUB:
      handleHandshake(addr, msg);
      break;

    case OpCode::PUBLISH: {
      if (client && client->type == ClientType::PUBLISHER &&
          msg.payloadLen >= 1) {
        uint8_t channel = static_cast<uint8_t>(msg.payload[0]);
        span<const byte> messageData(msg.payload + 1, msg.payloadLen - 1);

        if (verbose) {
          string msgStr(reinterpret_cast<const char *>(messageData.data()),
                        messageData.size());
          println("\033[35m[PUBLISH] {} channel={}: {}\033[0m",
                  addrToString(addr), channel, msgStr);
        }

        routeMessage(channel, messageData, addr);
      }
      break;
    }

    case OpCode::DISCONNECT: {
      if (verbose) {
        println("\033[33m[DISCONNECT] {} sent disconnect\033[0m",
                addrToString(addr));
      }
      // In UDP, we could remove the client, but it's stateless anyway
      break;
    }

    default:
      if (verbose) {
        println(stderr, "\033[33m[WARN] Unexpected opcode {} from {}\033[0m",
                static_cast<int>(msg.opcode), addrToString(addr));
      }
      break;
    }
  }

  void routeMessage(uint8_t channel, span<const byte> message,
                    const sockaddr_in &senderAddr) {
    // Get current timestamp
    auto now = chrono::system_clock::now();
    uint64_t timestamp =
        chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch())
            .count();

    // Encode MESSAGE frame
    vector<byte> msgBuffer(encoder.sizeMessage(message));
    encoder.encodeMessage(msgBuffer.data(), channel, timestamp, message);

    // Send to all subscribers on this channel
    const auto &subscribers = channelSubscribers[channel];
    for (const auto &subAddr : subscribers) {
      // Don't echo back to sender
      if (subAddr.sin_addr.s_addr == senderAddr.sin_addr.s_addr &&
          subAddr.sin_port == senderAddr.sin_port)
        continue;

      enqueueMessage(subAddr, vector<byte>(msgBuffer));
    }

    if (verbose) {
      println("\033[35m[ROUTE] Channel {} -> {} subscribers\033[0m", channel,
              subscribers.size());
    }
  }

  void enqueueMessage(const sockaddr_in &addr, vector<byte> message) {
    auto *client = getClient(addr);
    if (!client)
      return;

    if (client->sendQueue.size() >= MAX_SEND_QUEUE) {
      if (verbose) {
        println("\033[31m[WARN] Send queue full for {}, dropping "
                "message\033[0m",
                addrToString(addr));
      }
      return;
    }

    client->sendQueue.push(std::move(message));

    // If not already sending, start sending
    if (!sendInProgress) {
      processSendQueue();
    }
  }

  void processSendQueue() {
    // Find first client with pending messages
    for (auto &[addr, client] : clients) {
      if (!client.sendQueue.empty()) {
        submitSend(addr);
        return;
      }
    }
  }

  void submitRecv() {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for recv\033[0m");
      return;
    }

    memset(&recvAddr, 0, sizeof(recvAddr));
    recvAddrLen = sizeof(recvAddr);

    recvIovec.iov_base = recvBuffer.data();
    recvIovec.iov_len = recvBuffer.size();

    recvMsgHdr.msg_name = &recvAddr;
    recvMsgHdr.msg_namelen = recvAddrLen;
    recvMsgHdr.msg_iov = &recvIovec;
    recvMsgHdr.msg_iovlen = 1;

    io_uring_prep_recvmsg(sqe, sock, &recvMsgHdr, 0);
    io_uring_sqe_set_data(sqe,
                          reinterpret_cast<void *>(makeUserData(OpType::RECV)));
  }

  void submitSend(const sockaddr_in &addr) {
    auto *client = getClient(addr);
    if (!client || client->sendQueue.empty())
      return;

    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for send\033[0m");
      return;
    }

    sendInProgress = true;
    sendBuffer = client->sendQueue.front();
    sendAddr = addr;

    sendIovec.iov_base = sendBuffer.data();
    sendIovec.iov_len = sendBuffer.size();

    sendMsgHdr.msg_name = &sendAddr;
    sendMsgHdr.msg_namelen = sizeof(sendAddr);
    sendMsgHdr.msg_iov = &sendIovec;
    sendMsgHdr.msg_iovlen = 1;

    io_uring_prep_sendmsg(sqe, sock, &sendMsgHdr, 0);
    io_uring_sqe_set_data(sqe,
                          reinterpret_cast<void *>(makeUserData(OpType::SEND)));
  }

  void handleCompletion(io_uring_cqe *cqe) {
    OpType op =
        parseUserData(reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe)));
    int res = cqe->res;

    switch (op) {
    case OpType::RECV:
      handleRecv(res);
      break;
    case OpType::SEND:
      handleSend(res);
      break;
    }
  }

  void handleRecv(int res) {
    if (res < 0) {
      if (res != -EAGAIN && res != -EINTR) {
        println(stderr, "\033[31m[ERROR] Recv failed: {}\033[0m",
                strerror(-res));
      }
      submitRecv(); // Resubmit
      return;
    }

    if (res > 0) {
      span<const byte> data(recvBuffer.data(), res);
      processMessage(recvAddr, data);
    }

    submitRecv(); // Resubmit
  }

  void handleSend(int res) {
    if (res < 0) {
      if (res != -EAGAIN && res != -EINTR) {
        if (verbose) {
          println(stderr, "\033[31m[ERROR] Send failed: {}\033[0m",
                  strerror(-res));
        }
      }
    }

    // Pop the sent message from all queues (find which one)
    for (auto &[addr, client] : clients) {
      if (!client.sendQueue.empty() && sendBuffer == client.sendQueue.front()) {
        client.sendQueue.pop();
        break;
      }
    }

    sendBuffer.clear();
    sendInProgress = false;

    // Process next message in queue
    processSendQueue();
  }

  void run() {
    submitRecv();

    while (!STOP_REQUESTED) {
      io_uring_submit(&ring);

      io_uring_cqe *cqe;
      int ret = io_uring_wait_cqe(&ring, &cqe);
      if (ret < 0) {
        if (ret == -EINTR) {
          continue;
        }
        println(stderr, "\033[31mio_uring_wait_cqe failed: {}\033[0m",
                strerror(-ret));
        break;
      }

      handleCompletion(cqe);
      io_uring_cqe_seen(&ring, cqe);
    }

    println("\n\033[33mShutting down broker...\033[0m");
  }
};

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

  signal(SIGINT, handleSignal);
  signal(SIGPIPE, SIG_IGN);

  try {
    Broker broker(opts.verbose);
    broker.setupSocket(opts.host, opts.port);
    broker.run();
  } catch (const exception &e) {
    println(stderr, "\033[31mFatal error: {}\033[0m", e.what());
    return 1;
  }

  return 0;
}

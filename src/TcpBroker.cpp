#include <bitset>
#include <charconv>
#include <chrono>
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

enum class ClientState { HANDSHAKE, READY, CLOSING };

enum class ClientType { UNKNOWN, PUBLISHER, SUBSCRIBER };

enum class OpType : uint64_t {
  ACCEPT = 1,
  RECV = 2,
  SEND = 3,
};

struct Client {
  socket_t fd;
  ClientType type;
  ClientState state;
  bitset<256> channels;
  vector<byte> recvBuffer;
  queue<vector<byte>> sendQueue;
  bool sendInProgress;
  string clientId;

  Client()
      : fd(-1), type(ClientType::UNKNOWN), state(ClientState::HANDSHAKE),
        sendInProgress(false) {}

  explicit Client(socket_t s)
      : fd(s), type(ClientType::UNKNOWN), state(ClientState::HANDSHAKE),
        sendInProgress(false) {}
};

inline uint64_t makeUserData(OpType op, socket_t fd) {
  return (static_cast<uint64_t>(op) << 32) | static_cast<uint32_t>(fd);
}

inline pair<OpType, socket_t> parseUserData(uint64_t user_data) {
  OpType op = static_cast<OpType>(user_data >> 32);
  socket_t fd = static_cast<socket_t>(user_data & 0xFFFFFFFF);
  return {op, fd};
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
  print(R"(   ■  ▗▞▀▘▄▄▄▄      ▗▖    ▄▄▄ ▄▄▄  █  ▄ ▗▞▀▚▖ ▄▄▄
▗▄▟▙▄▖▝▚▄▖█   █     ▐▌   █   █   █ █▄▀  ▐▛▀▀▘█
  ▐▌      █▄▄▄▀     ▐▛▀▚▖█   ▀▄▄▄▀ █ ▀▄ ▝▚▄▄▖█
  ▐▌      █         ▐▙▄▞▘          █  █
  ▐▌      ▀
)");
}

void handleSignal(int signum) {
  if (signum == SIGINT) {
    STOP_REQUESTED = 1;
  }
}
} // namespace

class Broker {
private:
  io_uring ring;
  socket_t listenSock;
  map<socket_t, Client> clients;
  array<vector<socket_t>, 256> channelSubscribers;
  bool verbose;

  map<socket_t, vector<byte>> recvBuffers;
  map<socket_t, vector<byte>> sendBuffers;

  MessageEncoder encoder;
  MessageDecoder decoder;
  uint64_t sessionIdCounter;

public:
  explicit Broker(bool verbose)
      : listenSock(-1), verbose(verbose), sessionIdCounter(1) {
    int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    if (ret < 0) {
      throw runtime_error(
          format("io_uring initialization failed: {}", strerror(-ret)));
    }
  }

  ~Broker() {
    if (listenSock >= 0) {
      ::close(listenSock);
    }
    for (auto &[fd, client] : clients) {
      ::close(fd);
    }
    io_uring_queue_exit(&ring);
  }

  void setupListenSocket(const string &host, uint16_t port) {
    listenSock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock < 0) {
      throw runtime_error(
          format("Socket creation failed: {}", strerror(errno)));
    }

    int opt = 1;
    if (::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
        0) {
      ::close(listenSock);
      throw runtime_error(format("setsockopt failed: {}", strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
      ::close(listenSock);
      throw runtime_error(format("Invalid address: {}", host));
    }

    if (::bind(listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
        0) {
      ::close(listenSock);
      throw runtime_error(format("Bind failed: {}", strerror(errno)));
    }

    if (::listen(listenSock, SOMAXCONN) < 0) {
      ::close(listenSock);
      throw runtime_error(format("Listen failed: {}", strerror(errno)));
    }

    println("\033[32mBroker listening on {}:{}\033[0m", host, port);
  }

  void addClient(socket_t fd) {
    clients.emplace(fd, Client(fd));
    recvBuffers[fd].resize(RECV_BUFFER_SIZE);
    if (verbose) {
      println("\033[36m[+] Client fd={} added (state=HANDSHAKE)\033[0m", fd);
    }
  }

  void removeClient(socket_t fd) {
    auto it = clients.find(fd);
    if (it == clients.end())
      return;

    const auto &client = it->second;

    if (client.type == ClientType::SUBSCRIBER) {
      for (size_t ch = 0; ch < 256; ++ch) {
        if (client.channels.test(ch)) {
          auto &subs = channelSubscribers[ch];
          subs.erase(remove(subs.begin(), subs.end(), fd), subs.end());
        }
      }
    }

    if (verbose) {
      println("\033[36m[-] Client fd={} removed\033[0m", fd);
    }

    ::close(fd);
    recvBuffers.erase(fd);
    sendBuffers.erase(fd);
    clients.erase(it);
  }

  Client *getClient(socket_t fd) {
    auto it = clients.find(fd);
    return it != clients.end() ? &it->second : nullptr;
  }

  void subscribeToChannel(socket_t fd, uint8_t channel) {
    auto *client = getClient(fd);
    if (!client)
      return;

    client->channels.set(channel);
    auto &subs = channelSubscribers[channel];
    if (find(subs.begin(), subs.end(), fd) == subs.end()) {
      subs.push_back(fd);
    }

    if (verbose) {
      println("\033[33m[SUB] fd={} subscribed to channel {}\033[0m", fd,
              channel);
    }
  }

  bool parseHandshake(Client &client) {
    auto &buf = client.recvBuffer;
    span<const byte> dataSpan(buf.data(), buf.size());
    auto parseResult = decoder.decode(dataSpan);

    if (parseResult.needMoreData) {
      return false; // Wait for more data
    }

    if (!parseResult.message) {
      if (verbose) {
        println(stderr, "\033[31m[ERROR] Failed to parse handshake from fd={}\033[0m",
                client.fd);
      }
      client.state = ClientState::CLOSING;
      return false;
    }

    const auto &msg = *parseResult.message;

    if (msg.opcode == OpCode::HANDSHAKE_PUB) {
      // Parse: channel(1) + clientIdLen(1) + clientId
      if (msg.payloadLen >= 2) {
        uint8_t channel = static_cast<uint8_t>(msg.payload[0]);
        uint8_t clientIdLen = static_cast<uint8_t>(msg.payload[1]);

        if (msg.payloadLen >= 2 + clientIdLen) {
          client.clientId =
              string(reinterpret_cast<const char *>(msg.payload + 2),
                     clientIdLen);
          client.type = ClientType::PUBLISHER;
          client.state = ClientState::READY;
          client.channels.set(channel);

          println("\033[32m[HANDSHAKE] fd={} ({}) registered as PUBLISHER on "
                  "channel {}\033[0m",
                  client.fd, client.clientId, channel);

          // Send HANDSHAKE_ACK
          vector<byte> ackBuffer(encoder.sizeHandshakeAck());
          encoder.encodeHandshakeAck(ackBuffer.data(), 0, sessionIdCounter++);
          enqueueMessage(client.fd, std::move(ackBuffer));

          // Remove processed data
          client.recvBuffer.erase(client.recvBuffer.begin(),
                                  client.recvBuffer.begin() +
                                      parseResult.bytesConsumed);
          return true;
        }
      }
    } else if (msg.opcode == OpCode::HANDSHAKE_SUB) {
      // Parse: channelCount(1) + channels[...] + clientIdLen(1) + clientId
      if (msg.payloadLen >= 2) {
        uint8_t channelCount = static_cast<uint8_t>(msg.payload[0]);

        if (msg.payloadLen >= 1 + channelCount + 1) {
          client.type = ClientType::SUBSCRIBER;
          client.state = ClientState::READY;

          for (uint8_t i = 0; i < channelCount; ++i) {
            uint8_t channel = static_cast<uint8_t>(msg.payload[1 + i]);
            subscribeToChannel(client.fd, channel);
          }

          // Extract client ID
          uint8_t clientIdLen =
              static_cast<uint8_t>(msg.payload[1 + channelCount]);
          if (msg.payloadLen >= 1 + channelCount + 1 + clientIdLen) {
            client.clientId = string(
                reinterpret_cast<const char *>(msg.payload + 1 + channelCount +
                                               1),
                clientIdLen);
          }

          print("\033[32m[HANDSHAKE] fd={} ({}) registered as SUBSCRIBER on "
                "channels: ",
                client.fd, client.clientId);
          for (size_t ch = 0; ch < 256; ++ch) {
            if (client.channels.test(ch)) {
              print("{} ", ch);
            }
          }
          println("\033[0m");

          // Send HANDSHAKE_ACK
          vector<byte> ackBuffer(encoder.sizeHandshakeAck());
          encoder.encodeHandshakeAck(ackBuffer.data(), 0, sessionIdCounter++);
          enqueueMessage(client.fd, std::move(ackBuffer));

          // Remove processed data
          client.recvBuffer.erase(client.recvBuffer.begin(),
                                  client.recvBuffer.begin() +
                                      parseResult.bytesConsumed);
          return true;
        }
      }
    }

    return false;
  }

  void processClientBuffer(Client &client) {
    while (true) {
      if (client.state == ClientState::HANDSHAKE) {
        if (!parseHandshake(client)) {
          if (client.recvBuffer.size() > 1024) {
            println(stderr,
                    "\033[31m[ERROR] Handshake too large from fd={}\033[0m",
                    client.fd);
            client.state = ClientState::CLOSING;
          }
          break;
        }
        continue;
      }

      // Parse messages in READY state
      span<const byte> dataSpan(client.recvBuffer.data(),
                                client.recvBuffer.size());
      auto parseResult = decoder.decode(dataSpan);

      if (parseResult.needMoreData) {
        if (client.recvBuffer.size() > MAX_PAYLOAD_SIZE + HEADER_SIZE) {
          println(stderr, "\033[31m[ERROR] Message too large from fd={}\033[0m",
                  client.fd);
          client.state = ClientState::CLOSING;
        }
        break;
      }

      if (!parseResult.message) {
        println(stderr, "\033[31m[ERROR] Failed to parse message from fd={}\033[0m",
                client.fd);
        client.state = ClientState::CLOSING;
        break;
      }

      const auto &msg = *parseResult.message;

      switch (msg.opcode) {
      case OpCode::PUBLISH: {
        if (client.type == ClientType::PUBLISHER && msg.payloadLen >= 1) {
          uint8_t channel = static_cast<uint8_t>(msg.payload[0]);
          span<const byte> messageData(msg.payload + 1, msg.payloadLen - 1);

          if (verbose) {
            string msgStr(reinterpret_cast<const char *>(messageData.data()),
                          messageData.size());
            println("\033[35m[PUBLISH] fd={} channel={}: {}\033[0m", client.fd,
                    channel, msgStr);
          }

          routeMessage(channel, messageData, client.fd);
        }
        break;
      }
      case OpCode::DISCONNECT: {
        println("\033[33m[DISCONNECT] fd={} sent disconnect\033[0m",
                client.fd);
        client.state = ClientState::CLOSING;
        break;
      }
      default:
        if (verbose) {
          println(stderr, "\033[33m[WARN] Unexpected opcode {} from fd={}\033[0m",
                  static_cast<int>(msg.opcode), client.fd);
        }
        break;
      }

      // Remove processed message
      client.recvBuffer.erase(client.recvBuffer.begin(),
                              client.recvBuffer.begin() +
                                  parseResult.bytesConsumed);
    }
  }

  void routeMessage(uint8_t channel, span<const byte> message,
                    socket_t senderFd) {
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
    for (socket_t subFd : subscribers) {
      if (subFd == senderFd)
        continue; // Don't echo back

      enqueueMessage(subFd, vector<byte>(msgBuffer));
    }

    if (verbose) {
      println("\033[35m[ROUTE] Channel {} -> {} subscribers\033[0m", channel,
              subscribers.size());
    }
  }

  void enqueueMessage(socket_t fd, vector<byte> message) {
    auto *client = getClient(fd);
    if (!client || client->state != ClientState::READY)
      return;

    if (client->sendQueue.size() >= MAX_SEND_QUEUE) {
      if (verbose) {
        println("\033[31m[WARN] Send queue full for fd={}, dropping "
                "message\033[0m",
                fd);
      }
      return;
    }

    client->sendQueue.push(std::move(message));

    // If not already sending, start sending
    if (!client->sendInProgress) {
      submitSend(fd);
    }
  }

  void submitAccept() {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for accept\033[0m");
      return;
    }

    io_uring_prep_accept(sqe, listenSock, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe,
                          reinterpret_cast<void *>(makeUserData(OpType::ACCEPT, listenSock)));
  }

  void submitRecv(socket_t fd) {
    auto *client = getClient(fd);
    if (!client)
      return;

    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for recv on fd={}\033[0m",
              fd);
      return;
    }

    auto &buffer = recvBuffers[fd];
    io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(makeUserData(OpType::RECV, fd)));
  }

  void submitSend(socket_t fd) {
    auto *client = getClient(fd);
    if (!client || client->sendQueue.empty())
      return;

    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for send on fd={}\033[0m",
              fd);
      return;
    }

    client->sendInProgress = true;
    sendBuffers[fd] = client->sendQueue.front();
    const auto &msg = sendBuffers[fd];

    io_uring_prep_send(sqe, fd, msg.data(), msg.size(), 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void *>(makeUserData(OpType::SEND, fd)));
  }

  void handleCompletion(io_uring_cqe *cqe) {
    auto [op, fd] = parseUserData(reinterpret_cast<uint64_t>(io_uring_cqe_get_data(cqe)));
    int res = cqe->res;

    switch (op) {
    case OpType::ACCEPT:
      handleAccept(res);
      break;
    case OpType::RECV:
      handleRecv(fd, res);
      break;
    case OpType::SEND:
      handleSend(fd, res);
      break;
    }
  }

  void handleAccept(socket_t newFd) {
    if (newFd < 0) {
      if (newFd != -EINTR && newFd != -EAGAIN) {
        println(stderr, "\033[31mAccept failed: {}\033[0m", strerror(-newFd));
      }
      submitAccept(); // Resubmit
      return;
    }

    addClient(newFd);
    submitRecv(newFd);
    submitAccept(); // Resubmit accept
  }

  void handleRecv(socket_t fd, int res) {
    auto *client = getClient(fd);
    if (!client) {
      return;
    }

    if (res <= 0) {
      if (res == 0) {
        if (verbose) {
          println("\033[33m[DISCONNECT] fd={} closed connection\033[0m", fd);
        }
      } else if (res != -EAGAIN && res != -EINTR) {
        if (verbose) {
          println(stderr, "\033[31m[ERROR] Recv failed on fd={}: {}\033[0m",
                  fd, strerror(-res));
        }
      }
      removeClient(fd);
      return;
    }

    // Append received data to client buffer
    const auto &recvBuf = recvBuffers[fd];
    client->recvBuffer.insert(client->recvBuffer.end(), recvBuf.begin(),
                              recvBuf.begin() + res);

    // Process buffer
    processClientBuffer(*client);

    // Continue receiving or close
    if (client->state != ClientState::CLOSING) {
      submitRecv(fd);
    } else {
      removeClient(fd);
    }
  }

  void handleSend(socket_t fd, int res) {
    auto *client = getClient(fd);
    if (!client) {
      return;
    }

    if (res < 0) {
      if (res != -EAGAIN && res != -EINTR) {
        if (verbose) {
          println(stderr, "\033[31m[ERROR] Send failed on fd={}: {}\033[0m",
                  fd, strerror(-res));
        }
        removeClient(fd);
      }
      return;
    }

    // Message sent successfully
    client->sendQueue.pop();
    sendBuffers.erase(fd);
    client->sendInProgress = false;

    // Send next message if available
    if (!client->sendQueue.empty()) {
      submitSend(fd);
    }
  }

  void run() {
    submitAccept();

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
    broker.setupListenSocket(opts.host, opts.port);
    broker.run();
  } catch (const exception &e) {
    println(stderr, "\033[31mFatal error: {}\033[0m", e.what());
    return 1;
  }

  return 0;
}

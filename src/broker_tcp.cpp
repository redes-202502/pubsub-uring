#include <boost/program_options.hpp>

import std;

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liburing.h>

using namespace std;
namespace po = boost::program_options;

using socket_t = int;

namespace protocol {
constexpr uint8_t MAX_CHANNELS = 255;
constexpr uint8_t CHANNEL_BROADCAST = 0;

constexpr string_view HANDSHAKE_PUB = "[[PUB:";
constexpr string_view HANDSHAKE_SUB = "[[SUB:";
constexpr string_view MSG_PREFIX = "[CH:";
constexpr string_view EXIT_MSG = "[[EXIT]]";

constexpr size_t BUFFER_SIZE = 4096;
constexpr size_t MAX_SEND_QUEUE = 256;
} // namespace protocol

enum class ClientState { HANDSHAKE, READY, CLOSING };

enum class ClientType { UNKNOWN, PUBLISHER, SUBSCRIBER };

enum class OpType : uint64_t {
  ACCEPT = 1,
  RECV = 2,
  SEND = 3,
};

struct Client {
  socket_t S;
  ClientType type;
  ClientState state;
  bitset<256> channels;
  string recvBuffer;
  queue<string> sendQueue;
  bool sendInProgress;

  Client()
      : S(-1), type(ClientType::UNKNOWN), state(ClientState::HANDSHAKE),
        sendInProgress(false) {}

  Client(socket_t s)
      : S(s), type(ClientType::UNKNOWN), state(ClientState::HANDSHAKE),
        sendInProgress(false) {}
};

inline uint64_t makeUserData(OpType op, socket_t fd) {
  return (static_cast<uint64_t>(op) << 32) | static_cast<socket_t>(fd);
}

inline pair<OpType, int> parseUserData(uint64_t user_data) {
  OpType op = static_cast<OpType>(user_data >> 32);
  int fd = static_cast<socket_t>(user_data & 0xFFFFFFFF);
  return {op, fd};
}

class Broker {
private:
  io_uring Ring;
  socket_t Listen;
  map<socket_t, Client> Clients;
  array<vector<socket_t>, 256> channelSubs;
  bool verbose;

  map<socket_t, array<char, protocol::BUFFER_SIZE>> recvBuffers;
  map<socket_t, string> sendBuffers;

public:
  explicit Broker(bool verbose = false) : Listen(-1), verbose(verbose) {
    if (io_uring_queue_init(256, &Ring, 0) < 0) {
      throw runtime_error("Failed to initialize io_uring");
    }
  }

  ~Broker() {
    if (Listen >= 0) {
      ::close(Listen);
    }
    for (auto &[s, client] : Clients) {
      ::close(s);
    }
    io_uring_queue_exit(&Ring);
  }

  void setupListenSocket(const string &host, uint16_t port) {
    Listen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (Listen < 0) {
      throw runtime_error(
          format("Socket creation failed: {}", strerror(errno)));
    }

    int opt = 1;
    if (::setsockopt(Listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
      ::close(Listen);
      throw runtime_error(format("setsockopt failed: {}", strerror(errno)));
    }

    // Set non-blocking
    int flags = ::fcntl(Listen, F_GETFL, 0);
    if (flags < 0 || ::fcntl(Listen, F_SETFL, flags | O_NONBLOCK) < 0) {
      ::close(Listen);
      throw runtime_error(
          format("Failed to set non-blocking: {}", strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
      ::close(Listen);
      throw runtime_error(format("Invalid address: {}", host));
    }

    if (::bind(Listen, (sockaddr *)&addr, sizeof(addr)) < 0) {
      ::close(Listen);
      throw runtime_error(format("Bind failed: {}", strerror(errno)));
    }

    if (::listen(Listen, SOMAXCONN) < 0) {
      ::close(Listen);
      throw runtime_error(format("Listen failed: {}", strerror(errno)));
    }

    println("\033[32mBroker listening on {}:{}\033[0m", host, port);
  }

  void addClient(socket_t fd) {
    Clients.emplace(fd, Client(fd));
    if (verbose) {
      println("\033[36m[+] Client fd={} added (state=HANDSHAKE)\033[0m", fd);
    }
  }

  void removeClient(socket_t fd) {
    auto it = Clients.find(fd);
    if (it == Clients.end())
      return;

    const auto &client = it->second;

    // Remove from channel subscribers
    if (client.type == ClientType::SUBSCRIBER) {
      for (size_t ch = 0; ch < 256; ++ch) {
        if (client.channels.test(ch)) {
          auto &subs = channelSubs[ch];
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
    Clients.erase(it);
  }

  Client *getClient(socket_t fd) {
    auto it = Clients.find(fd);
    return it != Clients.end() ? &it->second : nullptr;
  }

  void subscribeToChannel(socket_t fd, uint8_t channel) {
    auto *client = getClient(fd);
    if (!client)
      return;

    client->channels.set(channel);
    auto &subs = channelSubs[channel];
    if (find(subs.begin(), subs.end(), fd) == subs.end()) {
      subs.push_back(fd);
    }

    if (verbose) {
      println("\033[33m[SUB] fd={} subscribed to channel {}\033[0m", fd,
              channel);
    }
  }

  bool parseHandshake(Client &client) {
    const auto &buf = client.recvBuffer;

    // Look for PUB handshake: [[PUB:123]]
    if (buf.starts_with(protocol::HANDSHAKE_PUB)) {
      size_t end = buf.find("]]");
      if (end == string::npos)
        return false; // Incomplete

      string_view handshake(buf.data(), end + 2);
      string channelStr(handshake.substr(protocol::HANDSHAKE_PUB.size(),
                                         end - protocol::HANDSHAKE_PUB.size()));

      uint8_t channel =
          channelStr.empty() ? protocol::CHANNEL_BROADCAST : stoi(channelStr);

      client.type = ClientType::PUBLISHER;
      client.state = ClientState::READY;
      client.channels.set(channel);

      println("\033[32m[HANDSHAKE] fd={} registered as PUBLISHER on channel "
              "{}\033[0m",
              client.S, channel);

      client.recvBuffer.erase(0, end + 2);
      return true;
    }

    // Look for SUB handshake: [[SUB:1,2,3]] or [[SUB:ALL]]
    if (buf.starts_with(protocol::HANDSHAKE_SUB)) {
      size_t end = buf.find("]]");
      if (end == string::npos)
        return false; // Incomplete

      string_view handshake(buf.data(), end + 2);
      string channelsStr(
          handshake.substr(protocol::HANDSHAKE_SUB.size(),
                           end - protocol::HANDSHAKE_SUB.size()));

      client.type = ClientType::SUBSCRIBER;
      client.state = ClientState::READY;

      if (channelsStr == "ALL") {
        // Subscribe to all channels
        for (size_t ch = 0; ch < 256; ++ch) {
          subscribeToChannel(client.S, ch);
        }
        println("\033[32m[HANDSHAKE] fd={} registered as SUBSCRIBER on ALL "
                "channels\033[0m",
                client.S);
      } else {
        // Parse comma-separated list
        istringstream ss(channelsStr);
        string token;
        vector<uint8_t> channels;
        while (getline(ss, token, ',')) {
          if (!token.empty()) {
            uint8_t ch = stoi(token);
            subscribeToChannel(client.S, ch);
            channels.push_back(ch);
          }
        }

        print(
            "\033[32m[HANDSHAKE] fd={} registered as SUBSCRIBER on channels: ",
            client.S);
        for (size_t i = 0; i < channels.size(); ++i) {
          print("{}{}", channels[i], i + 1 < channels.size() ? "," : "");
        }
        println("\033[0m");
      }

      client.recvBuffer.erase(0, end + 2);
      return true;
    }

    return false; // Unknown handshake
  }

  optional<pair<uint8_t, string_view>> parseMessage(string_view data) {
    // Format: [CH:123]message content\n
    if (!data.starts_with(protocol::MSG_PREFIX)) {
      return nullopt;
    }

    size_t chEnd = data.find(']');
    if (chEnd == string::npos)
      return nullopt;

    string_view channelStr = data.substr(protocol::MSG_PREFIX.size(),
                                         chEnd - protocol::MSG_PREFIX.size());
    uint8_t channel = stoi(string(channelStr));

    string_view content = data.substr(chEnd + 1);
    return make_pair(channel, content);
  }

  void routeMessage(uint8_t channel, string_view message, socket_t senderFd) {
    if (verbose) {
      println("\033[35m[ROUTE] Channel {} from fd={}: {}\033[0m", channel,
              senderFd, message);
    }

    const auto &subscribers = channelSubs[channel];
    for (socket_t subFd : subscribers) {
      if (subFd == senderFd)
        continue; // Don't echo back

      enqueueMessage(subFd, string(message));
    }

    // Also broadcast to channel 0 subscribers if not already channel 0
    if (channel != protocol::CHANNEL_BROADCAST) {
      const auto &broadcastSubs = channelSubs[protocol::CHANNEL_BROADCAST];
      for (socket_t subFd : broadcastSubs) {
        if (subFd == senderFd)
          continue;
        enqueueMessage(subFd, string(message));
      }
    }
  }

  void enqueueMessage(socket_t fd, string message) {
    auto *client = getClient(fd);
    if (!client || client->state != ClientState::READY)
      return;

    if (client->sendQueue.size() >= protocol::MAX_SEND_QUEUE) {
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
    io_uring_sqe *sqe = io_uring_get_sqe(&Ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for accept\033[0m");
      return;
    }

    io_uring_prep_accept(sqe, Listen, nullptr, nullptr, 0);
    io_uring_sqe_set_data(sqe, (void *)makeUserData(OpType::ACCEPT, Listen));
  }

  void submitRecv(socket_t fd) {
    auto *client = getClient(fd);
    if (!client)
      return;

    io_uring_sqe *sqe = io_uring_get_sqe(&Ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for recv on fd={}\033[0m", fd);
      return;
    }

    auto &buffer = recvBuffers[fd];
    io_uring_prep_recv(sqe, fd, buffer.data(), buffer.size(), 0);
    io_uring_sqe_set_data(sqe, (void *)makeUserData(OpType::RECV, fd));
  }

  void submitSend(socket_t fd) {
    auto *client = getClient(fd);
    if (!client || client->sendQueue.empty())
      return;

    io_uring_sqe *sqe = io_uring_get_sqe(&Ring);
    if (!sqe) {
      println(stderr, "\033[31mFailed to get SQE for send on fd={}\033[0m", fd);
      return;
    }

    client->sendInProgress = true;
    sendBuffers[fd] = client->sendQueue.front();
    const auto &msg = sendBuffers[fd];

    io_uring_prep_send(sqe, fd, msg.data(), msg.size(), 0);
    io_uring_sqe_set_data(sqe, (void *)makeUserData(OpType::SEND, fd));
  }

  void handleCompletion(io_uring_cqe *cqe) {
    auto [op, fd] = parseUserData((uint64_t)io_uring_cqe_get_data(cqe));
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

    // Set non-blocking
    int flags = ::fcntl(newFd, F_GETFL, 0);
    if (flags >= 0) {
      ::fcntl(newFd, F_SETFL, flags | O_NONBLOCK);
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
          println(stderr, "\033[31m[ERROR] Recv failed on fd={}: {}\033[0m", fd,
                  strerror(-res));
        }
      }
      removeClient(fd);
      return;
    }

    // Append received data to buffer
    const auto &recvBuf = recvBuffers[fd];
    client->recvBuffer.append(recvBuf.data(), res);

    // Process buffer
    processClientBuffer(*client);

    // Continue receiving
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
          println(stderr, "\033[31m[ERROR] Send failed on fd={}: {}\033[0m", fd,
                  strerror(-res));
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

  void processClientBuffer(Client &client) {
    while (true) {
      // Handle handshake phase
      if (client.state == ClientState::HANDSHAKE) {
        if (!parseHandshake(client)) {
          // Need more data or invalid handshake
          if (client.recvBuffer.size() > 128) {
            println(stderr,
                    "\033[31m[ERROR] Invalid handshake from fd={}\033[0m",
                    client.S);
            client.state = ClientState::CLOSING;
          }
          break;
        }
        continue; // Try to process more data
      }

      // Handle ready phase - look for complete messages (lines)
      size_t newline = client.recvBuffer.find('\n');
      if (newline == string::npos) {
        // No complete message yet
        if (client.recvBuffer.size() > protocol::BUFFER_SIZE) {
          println(stderr, "\033[31m[ERROR] Message too large from fd={}\033[0m",
                  client.S);
          client.state = ClientState::CLOSING;
        }
        break;
      }

      // Extract complete message
      string_view line(client.recvBuffer.data(), newline + 1);

      // Check for EXIT
      if (line.starts_with(protocol::EXIT_MSG)) {
        println("\033[33m[EXIT] fd={} sent EXIT message\033[0m", client.S);
        client.state = ClientState::CLOSING;
        break;
      }

      // Parse and route message if publisher
      if (client.type == ClientType::PUBLISHER) {
        if (auto msg = parseMessage(line)) {
          auto [channel, content] = *msg;
          routeMessage(channel, content, client.S);
        } else {
          if (verbose) {
            println(stderr,
                    "\033[31m[ERROR] Invalid message format from fd={}: "
                    "{}\033[0m",
                    client.S, line);
          }
        }
      }

      // Remove processed message
      client.recvBuffer.erase(0, newline + 1);
    }
  }

  void run() {
    submitAccept();

    while (!STOP_REQUESTED) {
      io_uring_submit(&Ring);

      io_uring_cqe *cqe;
      int ret = io_uring_wait_cqe(&Ring, &cqe);
      if (ret < 0) {
        if (ret == -EINTR) {
          continue;
        }
        println(stderr, "\033[31mio_uring_wait_cqe failed: {}\033[0m",
                strerror(-ret));
        break;
      }

      handleCompletion(cqe);
      io_uring_cqe_seen(&Ring, cqe);
    }

    println("\n\033[33mShutting down broker...\033[0m");
  }

  static inline volatile sig_atomic_t STOP_REQUESTED = 0;
};

void handleSignal(int signum) {
  if (signum == SIGINT) {
    Broker::STOP_REQUESTED = 1;
  }
}

int main(int argc, char *argv[]) {
  string host;
  uint16_t port;
  bool verbose;
  bool help;

  po::options_description desc("Broker options");
  desc.add_options()("help,h", po::bool_switch(&help), "Show help message")(
      "host", po::value<string>(&host)->default_value("127.0.0.1"),
      "Listen host address")(
      "port,p", po::value<uint16_t>(&port)->default_value(5000), "Listen port")(
      "verbose,v", po::bool_switch(&verbose), "Enable verbose logging");

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

  print(R"(▗▖    ▄▄▄ ▄▄▄  █  ▄ ▗▞▀▚▖ ▄▄▄     █  ▐▌ ▄▄▄ ▄ ▄▄▄▄    
▐▌   █   █   █ █▄▀  ▐▛▀▀▘█        ▀▄▄▞▘█    ▄ █   █   
▐▛▀▚▖█   ▀▄▄▄▀ █ ▀▄ ▝▚▄▄▖█             █    █ █   █   
▐▙▄▞▘          █  █                         █     ▗▄▖ 
                                                 ▐▌ ▐▌
                                                  ▝▀▜▌
                                                 ▐▙▄▞▘)");

  println("\n\n--    Press ctrl+c to exit...    --");

  signal(SIGINT, handleSignal);
  signal(SIGPIPE, SIG_IGN);

  try {
    Broker broker(verbose);
    broker.setupListenSocket(host, port);
    broker.run();
  } catch (const exception &e) {
    println(stderr, "\033[31mFatal error: {}\033[0m", e.what());
    return 1;
  }

  return 0;
}

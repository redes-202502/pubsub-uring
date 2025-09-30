#include <boost/program_options.hpp>

import std;

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

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

constexpr size_t MAX_UDP_PAYLOAD = 2048;
} // namespace protocol

enum class ClientType { UNKNOWN, PUBLISHER, SUBSCRIBER };

struct ClientAddr {
  sockaddr_in addr;

  bool operator==(const ClientAddr &other) const {
    return addr.sin_addr.s_addr == other.addr.sin_addr.s_addr &&
           addr.sin_port == other.addr.sin_port;
  }

  bool operator<(const ClientAddr &other) const {
    if (addr.sin_addr.s_addr != other.addr.sin_addr.s_addr)
      return addr.sin_addr.s_addr < other.addr.sin_addr.s_addr;
    return addr.sin_port < other.addr.sin_port;
  }

  string toString() const {
    char ip[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
    return format("{}:{}", ip, ntohs(addr.sin_port));
  }
};

struct Client {
  ClientAddr addr;
  ClientType type;
  bitset<256> channels;

  Client() : type(ClientType::UNKNOWN) {}

  Client(const sockaddr_in &address)
      : type(ClientType::UNKNOWN) {
    addr.addr = address;
  }
};

class BrokerUDP {
private:
  socket_t sock;
  map<ClientAddr, Client> clients;
  array<vector<ClientAddr>, 256> channelSubs;
  bool verbose;

public:
  explicit BrokerUDP(bool verbose = false) : sock(-1), verbose(verbose) {}

  ~BrokerUDP() {
    if (sock >= 0) {
      ::close(sock);
    }
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

    if (::bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
      ::close(sock);
      throw runtime_error(format("Bind failed: {}", strerror(errno)));
    }

    println("\033[32mUDP Broker listening on {}:{}\033[0m", host, port);
  }

  Client *getClient(const ClientAddr &addr) {
    auto it = clients.find(addr);
    return it != clients.end() ? &it->second : nullptr;
  }

  void addClient(const sockaddr_in &address) {
    ClientAddr caddr;
    caddr.addr = address;
    if (clients.find(caddr) == clients.end()) {
      clients.emplace(caddr, Client(address));
      if (verbose) {
        println("\033[36m[+] Client {} added\033[0m", caddr.toString());
      }
    }
  }

  void removeClient(const ClientAddr &addr) {
    auto it = clients.find(addr);
    if (it == clients.end())
      return;

    const auto &client = it->second;

    // Remove from channel subscribers
    if (client.type == ClientType::SUBSCRIBER) {
      for (size_t ch = 0; ch < 256; ++ch) {
        if (client.channels.test(ch)) {
          auto &subs = channelSubs[ch];
          subs.erase(remove(subs.begin(), subs.end(), addr), subs.end());
        }
      }
    }

    if (verbose) {
      println("\033[36m[-] Client {} removed\033[0m", addr.toString());
    }

    clients.erase(it);
  }

  void subscribeToChannel(const ClientAddr &addr, uint8_t channel) {
    auto *client = getClient(addr);
    if (!client)
      return;

    client->channels.set(channel);
    auto &subs = channelSubs[channel];
    if (find(subs.begin(), subs.end(), addr) == subs.end()) {
      subs.push_back(addr);
    }

    if (verbose) {
      println("\033[33m[SUB] {} subscribed to channel {}\033[0m",
              addr.toString(), channel);
    }
  }

  bool parseHandshake(Client &client, string_view data) {
    // Look for PUB handshake: [[PUB:123]]
    if (data.starts_with(protocol::HANDSHAKE_PUB)) {
      size_t end = data.find("]]");
      if (end == string::npos)
        return false; // Incomplete

      string_view handshake = data.substr(0, end + 2);
      string channelStr(handshake.substr(protocol::HANDSHAKE_PUB.size(),
                                         end - protocol::HANDSHAKE_PUB.size()));

      uint8_t channel =
          channelStr.empty() ? protocol::CHANNEL_BROADCAST : stoi(channelStr);

      client.type = ClientType::PUBLISHER;
      client.channels.set(channel);

      println("\033[32m[HANDSHAKE] {} registered as PUBLISHER on channel "
              "{}\033[0m",
              client.addr.toString(), channel);

      return true;
    }

    // Look for SUB handshake: [[SUB:1,2,3]] or [[SUB:ALL]]
    if (data.starts_with(protocol::HANDSHAKE_SUB)) {
      size_t end = data.find("]]");
      if (end == string::npos)
        return false; // Incomplete

      string_view handshake = data.substr(0, end + 2);
      string channelsStr(
          handshake.substr(protocol::HANDSHAKE_SUB.size(),
                           end - protocol::HANDSHAKE_SUB.size()));

      client.type = ClientType::SUBSCRIBER;

      if (channelsStr == "ALL") {
        // Subscribe to all channels
        for (size_t ch = 0; ch < 256; ++ch) {
          subscribeToChannel(client.addr, ch);
        }
        println("\033[32m[HANDSHAKE] {} registered as SUBSCRIBER on ALL "
                "channels\033[0m",
                client.addr.toString());
      } else {
        // Parse comma-separated list or single channel
        string channelsString{channelsStr};
        istringstream ss{channelsString};
        string token;
        vector<uint8_t> channels;
        while (std::getline(ss, token, ',')) {
          if (!token.empty()) {
            uint8_t ch = stoi(token);
            subscribeToChannel(client.addr, ch);
            channels.push_back(ch);
          }
        }

        print("\033[32m[HANDSHAKE] {} registered as SUBSCRIBER on channels: ",
              client.addr.toString());
        for (size_t i = 0; i < channels.size(); ++i) {
          print("{}{}", channels[i], i + 1 < channels.size() ? "," : "");
        }
        println("\033[0m");
      }

      return true;
    }

    return false; // Unknown handshake
  }

  optional<pair<uint8_t, string_view>> parseMessage(string_view data) {
    // Format: [CH:123]message content
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

  void routeMessage(uint8_t channel, string_view message,
                    const ClientAddr &senderAddr) {
    if (verbose) {
      println("\033[35m[ROUTE] Channel {} from {}: {}\033[0m", channel,
              senderAddr.toString(), message);
    }

    const auto &subscribers = channelSubs[channel];
    for (const auto &subAddr : subscribers) {
      if (subAddr == senderAddr)
        continue; // Don't echo back

      sendMessage(subAddr, message);
    }

    // Also broadcast to channel 0 subscribers if not already channel 0
    if (channel != protocol::CHANNEL_BROADCAST) {
      const auto &broadcastSubs = channelSubs[protocol::CHANNEL_BROADCAST];
      for (const auto &subAddr : broadcastSubs) {
        if (subAddr == senderAddr)
          continue;
        sendMessage(subAddr, message);
      }
    }
  }

  void sendMessage(const ClientAddr &addr, string_view message) {
    ssize_t sent = ::sendto(sock, message.data(), message.size(), 0,
                            (const sockaddr *)&addr.addr, sizeof(addr.addr));
    if (sent < 0) {
      if (verbose) {
        println(stderr, "\033[31m[ERROR] Failed to send to {}: {}\033[0m",
                addr.toString(), strerror(errno));
      }
    } else if (verbose) {
      println("\033[34m[SEND] Sent {} bytes to {}\033[0m", sent,
              addr.toString());
    }
  }

  void run() {
    println("Broker running, waiting for datagrams...\n");

    array<char, protocol::MAX_UDP_PAYLOAD> buffer;
    sockaddr_in clientAddr{};
    socklen_t clientAddrLen;

    while (!STOP_REQUESTED) {
      clientAddrLen = sizeof(clientAddr);
      ssize_t received =
          ::recvfrom(sock, buffer.data(), buffer.size(), 0,
                     (sockaddr *)&clientAddr, &clientAddrLen);

      if (received < 0) {
        if (errno == EINTR) {
          continue;
        }
        println(stderr, "\033[31mReceive failed: {}\033[0m", strerror(errno));
        break;
      }

      if (received == 0) {
        continue; // Empty datagram
      }

      ClientAddr caddr;
      caddr.addr = clientAddr;
      string_view data(buffer.data(), received);

      // Check for EXIT message
      if (data.starts_with(protocol::EXIT_MSG)) {
        println("\033[33m[EXIT] {} sent EXIT message\033[0m",
                caddr.toString());
        removeClient(caddr);
        continue;
      }

      // Get or create client
      auto *client = getClient(caddr);
      if (!client) {
        addClient(clientAddr);
        client = getClient(caddr);
      }

      // If client type is unknown, try to parse handshake
      if (client->type == ClientType::UNKNOWN) {
        if (!parseHandshake(*client, data)) {
          if (verbose) {
            println(stderr,
                    "\033[31m[ERROR] Invalid handshake from {}: {}\033[0m",
                    caddr.toString(), data);
          }
          removeClient(caddr);
        }
        continue;
      }

      // If it's a publisher, parse and route the message
      if (client->type == ClientType::PUBLISHER) {
        if (auto msg = parseMessage(data)) {
          auto [channel, content] = *msg;
          routeMessage(channel, content, caddr);
        } else {
          if (verbose) {
            println(stderr,
                    "\033[31m[ERROR] Invalid message format from {}: "
                    "{}\033[0m",
                    caddr.toString(), data);
          }
        }
      }
      // Subscribers just listen, they don't send regular messages
    }

    println("\n\033[33mShutting down broker...\033[0m");
  }

  static inline volatile sig_atomic_t STOP_REQUESTED = 0;
};

void handleSignal(int signum) {
  if (signum == SIGINT) {
    BrokerUDP::STOP_REQUESTED = 1;
  }
}

int main(int argc, char *argv[]) {
  string host;
  uint16_t port;
  bool verbose;
  bool help;

  po::options_description desc("UDP Broker options");
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

  print(R"(▗▖    ▄▄▄ ▄▄▄  █  ▄ ▗▞▀▚▖ ▄▄▄     █  ▐▌ ▄▄▄ ▄ ▄▄▄▄    █  ▐▌▗▖   ▄▄▄
▐▌   █   █   █ █▄▀  ▐▛▀▀▘█        ▀▄▄▞▘█    ▄ █   █   ▀▄▄▞▘▐▌  █   █
▐▛▀▚▖█   ▀▄▄▄▀ █ ▀▄ ▝▚▄▄▖█             █    █ █   █        ▐▛▀▚▖█   █
▐▙▄▞▘          █  █                         █     ▗▄▖      ▐▙▄▞▘█▄▄▄▀
                                                 ▐▌ ▐▌                █
                                                  ▝▀▜▌                ▀
                                                 ▐▙▄▞▘                   )");

  println("\n\n--    Press ctrl+c to exit...    --");

  signal(SIGINT, handleSignal);

  try {
    BrokerUDP broker(verbose);
    broker.setupSocket(host, port);
    broker.run();
  } catch (const exception &e) {
    println(stderr, "\033[31mFatal error: {}\033[0m", e.what());
    return 1;
  }

  return 0;
}

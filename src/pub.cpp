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

namespace {
volatile sig_atomic_t stopRequested = 0;

void handleSignal(int signum) {
  if (signum == SIGINT) {
    stopRequested = 1;
  }
}
} // namespace

int main(int argc, char *argv[]) {
  print(R"(▄▄▄▄  █  ▐▌▗▖       █  ▐▌ ▄▄▄ ▄ ▄▄▄▄    
█   █ ▀▄▄▞▘▐▌       ▀▄▄▞▘█    ▄ █   █   
█▄▄▄▀      ▐▛▀▚▖         █    █ █   █   
█          ▐▙▄▞▘              █     ▗▄▖ 
▀                                  ▐▌ ▐▌
                                    ▝▀▜▌
                                   ▐▙▄▞▘)");

  print("\n\n--    Press ctrl+c to exit...    --\n\n");

  signal(SIGINT, handleSignal);

  auto genMsg = misc::makeMessageGenerator();
  array<char, 128> buffer;

  auto sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    println(stderr, "\033[31mSocket creation failed: {}\033[0m",
            strerror(errno));
    return EXIT_FAILURE;
  }

  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(5000);
  if (inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) <= 0) {
    println(stderr, "\033[31mInvalid address: {}\033[0m", strerror(errno));
    close(sock);
    return 1;
  }

  if (connect(sock, (sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
    println(stderr, "\033[31mConnection failed: {}\033[0m", strerror(errno));
    close(sock);
    return 1;
  }

  println("\033[32mConnected to broker at 127.0.0.1:5000\033[0m");

  while (!stopRequested) {
    const auto n = genMsg.generateMessage(buffer.data(), buffer.size());
    println("Generated [{} bytes]: {}", n, buffer.data());

    auto sent = send(sock, buffer.data(), n, 0);
    if (sent < 0) {
      println(stderr, "\033[31mSend failed: {}\033[0m", strerror(errno));
      break;
    }

    this_thread::sleep_for(500ms); // slow down a bit
    // TODO: choose an config if to sleep or keep sending messages
  }
  close(sock);

  println("\nExiting program...");
  return 0;
}

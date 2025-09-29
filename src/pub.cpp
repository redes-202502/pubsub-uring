import std;
import MessageGenerator;

#include <csignal>

using namespace std;

namespace {
volatile sig_atomic_t stopRequested = 0;

void handleSignal(int signum) {
  if (signum == SIGINT) {
    stopRequested = 1;
  }
}
} // namespace

int main() {
  print(R"(▄▄▄▄  █  ▐▌▗▖       █  ▐▌ ▄▄▄ ▄ ▄▄▄▄    
█   █ ▀▄▄▞▘▐▌       ▀▄▄▞▘█    ▄ █   █   
█▄▄▄▀      ▐▛▀▚▖         █    █ █   █   
█          ▐▙▄▞▘              █     ▗▄▖ 
▀                                  ▐▌ ▐▌
                                    ▝▀▜▌
                                   ▐▙▄▞▘)");

  print("\n\n--    Press ctrl+c to exit...    --\n\n");
  auto genMsg = misc::makeMessageGenerator();

  array<char, 128> buffer;

  // dont run this for too long...
  // really this is fast... your space will be 🪦 if you redirect to a file
  // while (true) {
  //   const auto n = genMsg.generateMessage(buffer.data(), buffer.size());
  //   println("Generated [{} bytes]: {}", n, buffer.data());
  // }

  for (uint32_t i = 0; i < 10; i++) {
    const auto n = genMsg.generateMessage(buffer.data(), buffer.size());
    println("Generated [{} bytes]: {}", n, buffer.data());
  }

  println("Exiting program...");
}

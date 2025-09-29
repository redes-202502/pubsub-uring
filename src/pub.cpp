import std;
import MessageGenerator;

using namespace std;

int main() {
  auto genMsg = misc::makeMessageGenerator();

  array<char, 128> buffer;

  while (true) {
    const auto n = genMsg.generateMessage(buffer.data(), buffer.size());
    println("Generated [{} bytes]: {}", n, buffer.data());
  }
}

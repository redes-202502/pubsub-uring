#include <charconv>
#include <print>
#include <string>

using namespace std;

namespace {
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
        print("Error: Missing value for --host\n");
        return nullopt;
      }
    } else if (arg == "--port" || arg == "-p") {
      if (i + 1 < argc) {
        auto portOpt = parseNumber<uint16_t>(argv[++i]);
        if (portOpt) {
          opts.port = *portOpt;
        } else {
          print("Error: Invalid value for --port\n");
          return nullopt;
        }
      } else {
        print("Error: Missing value for --port\n");
        return nullopt;
      }
    } else {
      print("Error: Unknown option '{}'\n", arg);
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

  println("Host: {}", opts.host);
  println("Port: {}", opts.port);
  println("Verbose: {}", opts.verbose);

  return 0;
}
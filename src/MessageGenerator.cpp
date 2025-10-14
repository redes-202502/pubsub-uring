#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <string_view>
#include <vector>

#include "MessageGenerator.hpp"

using namespace std;

namespace misc {

constexpr auto teams = to_array<string_view>({
#include "teams.txt"
});

constexpr auto players = to_array<string_view>({
#include "players.txt"
});

uint32_t MessageGenerator::initSeed() {
  if (const char *env = getenv("MsgGen_SEED")) {
    uint32_t value{};
    if (auto [_, ec] = from_chars(env, env + strlen(env), value);
        ec == errc{}) {
      return value;
    }
  }
  return static_cast<uint32_t>(random_device{}());
}

MessageGenerator::MessageGenerator(vector<Fn> gens, optional<uint32_t> seed)
    : generators(std::move(gens)), gen(seed.value_or(initSeed())),
      pick(0, generators.empty() ? 0 : generators.size() - 1) {}

uint32_t MessageGenerator::generateMessage(char *buffer, uint32_t sz) {
  if (generators.empty() || sz == 0)
    return 0;
  uint32_t written = 0;
  generators[pick(gen)](gen, buffer, sz, written);
  return written;
}

MessageGenerator makeMessageGenerator(optional<uint32_t> seed) {
  vector<MessageGenerator::Fn> functors;

  functors.emplace_back(
      [](auto &gen, char *buffer, uint32_t sz, uint32_t &written) {
        uniform_int_distribution<size_t> teamDist(0, teams.size() - 1);
        uniform_int_distribution<uint32_t> minuteDist(1, 90);

        auto team = teams[teamDist(gen)];
        auto minute = minuteDist(gen);

        auto result =
            format_to_n(buffer, sz - 1, "Gol de {} al minuto {}", team, minute);
        *result.out = '\0';
        written = static_cast<uint32_t>(result.out - buffer);
      });

  functors.emplace_back(
      [](auto &gen, char *buffer, uint32_t sz, uint32_t &written) {
        uniform_int_distribution<size_t> playerDist(0, players.size() - 1);
        auto player = players[playerDist(gen)];
        auto result = format_to_n(buffer, sz - 1, "Cambio entra {}", player);
        *result.out = '\0';
        written = static_cast<uint32_t>(result.out - buffer);
      });

  functors.emplace_back([](auto &gen, char *buffer, uint32_t sz,
                           uint32_t &written) {
    uniform_int_distribution<size_t> playerDist(0, players.size() - 1);
    uniform_int_distribution<uint32_t> minuteDist(1, 90);
    auto player = players[playerDist(gen)];
    auto minute = minuteDist(gen);
    auto result =
        format_to_n(buffer, sz - 1, "Tarjeta amarilla 🟨 para {} al minuto {}",
                    player, minute);
    *result.out = '\0';
    written = static_cast<uint32_t>(result.out - buffer);
  });

  functors.emplace_back([](auto &gen, char *buffer, uint32_t sz,
                           uint32_t &written) {
    uniform_int_distribution<size_t> playerDist(0, players.size() - 1);
    uniform_int_distribution<uint32_t> minuteDist(1, 90);
    auto player = players[playerDist(gen)];
    auto minute = minuteDist(gen);
    auto result = format_to_n(
        buffer, sz - 1, "Tarjeta roja 🟥 para {} al minuto {}", player, minute);
    *result.out = '\0';
    written = static_cast<uint32_t>(result.out - buffer);
  });

  functors.emplace_back(
      [](auto &gen, char *buffer, uint32_t sz, uint32_t &written) {
        uniform_int_distribution<size_t> playerDist(0, players.size() - 1);
        auto player = players[playerDist(gen)];
        auto result = format_to_n(buffer, sz - 1, "Cambio sale {}", player);
        *result.out = '\0';
        written = static_cast<uint32_t>(result.out - buffer);
      });

  functors.emplace_back([](auto &gen, char *buffer, uint32_t sz,
                           uint32_t &written) {
    uniform_int_distribution<size_t> teamDist(0, teams.size() - 1);
    auto team = teams[teamDist(gen)];
    auto result = format_to_n(buffer, sz - 1,
                              "Se agregan 3 minutos al partido en {}", team);
    *result.out = '\0';
    written = static_cast<uint32_t>(result.out - buffer);
  });

  functors.emplace_back(
      [](auto &gen, char *buffer, uint32_t sz, uint32_t &written) {
        uniform_int_distribution<size_t> playerDist(0, players.size() - 1);
        auto player = players[playerDist(gen)];
        auto result = format_to_n(
            buffer, sz - 1, "{} está lesionado y pide atención médica", player);
        *result.out = '\0';
        written = static_cast<uint32_t>(result.out - buffer);
      });

  functors.emplace_back(
      [](auto &gen, char *buffer, uint32_t sz, uint32_t &written) {
        uniform_int_distribution<size_t> teamDist(0, teams.size() - 1);
        uniform_int_distribution<uint32_t> minuteDist(1, 90);
        auto team = teams[teamDist(gen)];
        auto minute = minuteDist(gen);
        auto result = format_to_n(buffer, sz - 1,
                                  "Penalti para {} al minuto {}", team, minute);
        *result.out = '\0';
        written = static_cast<uint32_t>(result.out - buffer);
      });

  functors.emplace_back([](auto &gen, char *buffer, uint32_t sz,
                           uint32_t &written) {
    uniform_int_distribution<size_t> teamDist(0, teams.size() - 1);
    auto team = teams[teamDist(gen)];
    auto result = format_to_n(buffer, sz - 1, "Saque de esquina para {}", team);
    *result.out = '\0';
    written = static_cast<uint32_t>(result.out - buffer);
  });

  functors.emplace_back(
      [](auto &gen, char *buffer, uint32_t sz, uint32_t &written) {
        uniform_int_distribution<size_t> playerDist(0, players.size() - 1);
        auto player = players[playerDist(gen)];
        auto result =
            format_to_n(buffer, sz - 1, "Gran atajada del portero {}", player);
        *result.out = '\0';
        written = static_cast<uint32_t>(result.out - buffer);
      });

  functors.emplace_back([](auto &gen, char *buffer, uint32_t sz,
                           uint32_t &written) {
    uniform_int_distribution<size_t> teamDist(0, teams.size() - 1);
    auto team = teams[teamDist(gen)];
    auto result =
        format_to_n(buffer, sz - 1, "Comienza el segundo tiempo en {}", team);
    *result.out = '\0';
    written = static_cast<uint32_t>(result.out - buffer);
  });

  functors.emplace_back(
      [](auto &gen, char *buffer, uint32_t sz, uint32_t &written) {
        uniform_int_distribution<size_t> teamDist(0, teams.size() - 1);
        auto team = teams[teamDist(gen)];
        auto result =
            format_to_n(buffer, sz - 1, "Finaliza el partido en {}", team);
        *result.out = '\0';
        written = static_cast<uint32_t>(result.out - buffer);
      });

  return MessageGenerator(std::move(functors), seed);
}
} // namespace misc

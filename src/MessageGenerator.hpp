#ifndef MESSAGE_GENERATOR_HPP
#define MESSAGE_GENERATOR_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <random>

namespace misc {
class MessageGenerator {
public:
  using Fn = std::function<void(std::minstd_rand &gen, char *buffer,
                                uint32_t sz, uint32_t &written)>;

  static uint32_t initSeed();

  explicit MessageGenerator(std::vector<Fn> gens,
                            std::optional<uint32_t> seed = std::nullopt);

  uint32_t generateMessage(char *buffer, uint32_t sz);

private:
  std::vector<Fn> generators;
  std::minstd_rand gen;
  std::uniform_int_distribution<uint32_t> pick;
};

MessageGenerator
makeMessageGenerator(std::optional<uint32_t> seed = std::nullopt);
} // namespace misc
#endif
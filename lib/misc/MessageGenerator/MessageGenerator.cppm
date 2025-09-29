export module MessageGenerator;

import std;

using namespace std;

export namespace misc {

class MessageGenerator {
public:
  using Fn = function<void(minstd_rand &gen, char *buffer, uint32_t sz,
                           uint32_t &written)>;

  static uint32_t initSeed();

  explicit MessageGenerator(vector<Fn> gens, optional<uint32_t> seed = nullopt);

  uint32_t generateMessage(char *buffer, uint32_t sz);

private:
  vector<Fn> generators;
  minstd_rand gen;
  uniform_int_distribution<uint32_t> pick;
};

MessageGenerator makeMessageGenerator(optional<uint32_t> seed = nullopt);
} // namespace misc

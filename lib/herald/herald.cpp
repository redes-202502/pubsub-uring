module;

export module herald;

export import herald.types;
export import herald.proto;

import herald.event;
import herald.log;

export namespace herald {
struct HeraldConfig {};

struct HeraldState {
  types::socket_t S;
};

class Herald {
public:
  Herald(HeraldConfig cfg) : CFG{cfg} {}

  void run(); // event loop

private:
  HeraldConfig CFG;
};
} // namespace herald
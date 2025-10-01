module;

export module herald;

export import herald.proto;

struct HeraldConfig {};

class Herald {
public:
  Herald(HeraldConfig cfg) : CFG{cfg} {}

  void run(); // event loop

private:
  HeraldConfig CFG;
};
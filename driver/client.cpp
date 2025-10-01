module;

export module herald.driver.client;

import herald.driver.config;

export namespace herald::driver::client {
class PubClient {
public:
  PubClient(PubConfig cfg) : CFG{cfg} {}

private:
  PubConfig CFG;
};

class SubClient {
public:
  SubClient(SubConfig cfg) : CFG{cfg} {}

private:
  SubConfig CFG;
};
} // namespace herald::driver::client
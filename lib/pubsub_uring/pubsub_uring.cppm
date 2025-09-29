module;

#include <liburing.h>

export module pubsub_uring;

import std;

using namespace std;

export void hello_world() { println("Hello world"); }

export void test_pubsub_uring() {
  io_uring ring;
  io_uring_queue_init(2, &ring, 0);

  io_uring_queue_exit(&ring);
}
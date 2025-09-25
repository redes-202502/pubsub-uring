add_library(pubsub-uring)
target_sources(pubsub-uring
  PUBLIC
  FILE_SET CXX_MODULES FILES pubsub_uring.cppm
)
target_link_libraries(pubsub-uring
  PRIVATE
  liburing::liburing
)
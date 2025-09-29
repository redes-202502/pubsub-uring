add_executable(pub)
target_sources(pub
  PRIVATE
  pub.cpp
)
target_link_libraries(pub
  PRIVATE misc
)

add_executable(sub)
target_sources(sub
  PRIVATE
  sub.cpp
)

add_executable(broker)
target_sources(broker
  PRIVATE
  broker.cpp
)

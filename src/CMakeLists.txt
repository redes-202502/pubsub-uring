add_executable(pub_tcp)
target_sources(pub_tcp
  PRIVATE
  pub_tcp.cpp
)
target_link_libraries(pub_tcp
  PRIVATE
  misc
  Boost::program_options
)

add_executable(sub_tcp)
target_sources(sub_tcp
  PRIVATE
  sub_tcp.cpp
)
target_link_libraries(sub_tcp
  PRIVATE
  Boost::program_options
)

add_executable(broker_tcp)
target_sources(broker_tcp
  PRIVATE
  broker_tcp.cpp
)

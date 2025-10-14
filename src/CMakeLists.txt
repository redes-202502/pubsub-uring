add_executable(tcp-publisher)
target_sources(tcp-publisher
  PRIVATE
  TcpPublisher.cpp
  MessageGenerator.cpp
)

add_executable(udp-publisher)
target_sources(udp-publisher
  PRIVATE
  UdpPublisher.cpp
  MessageGenerator.cpp
)

add_executable(tcp-broker)
target_sources(tcp-broker
  PRIVATE
  TcpBroker.cpp
)
target_link_libraries(tcp-broker
  PRIVATE
  liburing::liburing
)


add_executable(udp-broker)
target_sources(udp-broker
  PRIVATE
  UdpBroker.cpp
)
target_link_libraries(udp-broker
  PRIVATE
  liburing::liburing
)

add_library(herald_proto)
target_sources(herald_proto
  PUBLIC
  FILE_SET CXX_MODULES FILES proto.cppm
)
add_subdirectory(proto)

add_library(herald)
target_sources(herald
PUBLIC
FILE_SET CXX_MODULES FILES herald.cpp
)
target_link_libraries(herald
  PRIVATE
  herald_proto
)
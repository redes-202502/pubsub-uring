add_subdirectory(event)
add_subdirectory(log)
add_subdirectory(proto)

add_library(herald)
target_sources(herald
  PUBLIC
  FILE_SET CXX_MODULES
  FILES
  herald.cpp
  types.cpp
)
target_link_libraries(herald
  PRIVATE
  herald_event
  herald_log
  herald_proto
)
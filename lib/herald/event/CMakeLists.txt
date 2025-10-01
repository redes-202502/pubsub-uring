add_library(herald_event)
target_sources(herald_event
  PUBLIC
  FILE_SET CXX_MODULES
  FILES
  event.cpp
)
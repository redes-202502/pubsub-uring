add_library(herald_log)
target_sources(herald_log
  PUBLIC
  FILE_SET CXX_MODULES
  FILES
  log.cpp
)
target_sources(misc
  PRIVATE
  MessageGenerator.cpp
  PUBLIC
  FILE_SET CXX_MODULES FILES MessageGenerator.cppm
)
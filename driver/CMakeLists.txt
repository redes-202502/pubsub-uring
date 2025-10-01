add_library(herald_driver)
target_sources(herald_driver
  PUBLIC
  FILE_SET CXX_MODULES 
  FILES
  driver.cpp
  config.cpp
  client.cpp
)
find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
mark_as_advanced(LIBURING_INCLUDE_DIR)

find_library(LIBURING_LIBRARY NAMES uring)
mark_as_advanced(LIBURING_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  liburing
  REQUIRED_VARS LIBURING_LIBRARY LIBURING_INCLUDE_DIR
)

if(liburing_FOUND)
  set(LIBURING_LIBRARIES ${LIBURING_LIBRARY})
  set(LIBURING_INCLUDE_DIRS ${LIBURING_INCLUDE_DIR})

  add_library(liburing::liburing UNKNOWN IMPORTED)
  set_target_properties(liburing::liburing PROPERTIES
    IMPORTED_LOCATION ${LIBURING_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${LIBURING_INCLUDE_DIR}
  )
endif()
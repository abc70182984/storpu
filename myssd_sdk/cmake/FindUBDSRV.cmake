include(FindPackageHandleStandardArgs)

find_path(UBDSRV_INCLUDE_DIR ublksrv.h)
find_library(UBDSRV_LIBRARY ublksrv)

find_package_handle_standard_args(UBDSRV
 DEFAULT_MSG
  UBDSRV_INCLUDE_DIR
  UBDSRV_LIBRARY
)

mark_as_advanced(UBDSRV_LIBRARY UBDSRV_INCLUDE_DIR)

if(UBDSRV_FOUND)
  set(UBDSRV_LIBRARIES    ${UBDSRV_LIBRARY})
  set(UBDSRV_INCLUDE_DIRS ${UBDSRV_INCLUDE_DIR})
endif()

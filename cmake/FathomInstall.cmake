if(APPLE)
  set(_fathom_origin "@loader_path")
elseif(UNIX)
  set(_fathom_origin "$ORIGIN")
else()
  if(FATHOM_PYTHON_WHEEL)
    message(FATAL_ERROR "Wheel packaging currently supports macOS and Linux")
  endif()
endif()
if(FATHOM_PYTHON_WHEEL)
  if(NOT FATHOM_HAS_ARROW OR NOT FATHOM_ENABLE_PYTHON)
    message(FATAL_ERROR "Python wheels require Arrow and the Python binding")
  endif()
  # C library lives in site-packages/ankurafathom/.libs; PyArrow is its sibling.
  set_target_properties(fathom_c_api PROPERTIES INSTALL_RPATH "${_fathom_origin}/../../pyarrow")
  install(TARGETS fathom_c_api LIBRARY DESTINATION ankurafathom/.libs COMPONENT Python)
else()
  # Native installs use an external SDK, not the Python wheel dependency layout.
  set(_fathom_sdk_rpath "")
  foreach(_dependency IN LISTS FATHOM_ARROW_LIBRARIES)
    get_target_property(_location ${_dependency} IMPORTED_LOCATION)
    if(_location)
      get_filename_component(_directory "${_location}" DIRECTORY)
      list(APPEND _fathom_sdk_rpath "${_directory}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _fathom_sdk_rpath)
  set_target_properties(fathom_c_api PROPERTIES INSTALL_RPATH "${_fathom_sdk_rpath}" EXPORT_NAME c_api)
  set_target_properties(fathom PROPERTIES INSTALL_RPATH "${_fathom_sdk_rpath}")
  install(TARGETS fathom_c_api EXPORT AnkuraFathomTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development)
  install(TARGETS fathom RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime)
  install(DIRECTORY runtime/include/ankurafathom DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT Development)
  include(CMakePackageConfigHelpers)
  configure_package_config_file(cmake/AnkuraFathomConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AnkuraFathomConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AnkuraFathom")
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/AnkuraFathomConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(EXPORT AnkuraFathomTargets NAMESPACE AnkuraFathom::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AnkuraFathom" COMPONENT Development)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/AnkuraFathomConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AnkuraFathomConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AnkuraFathom" COMPONENT Development)
  foreach(_notice arrow_c/LICENSE.txt arrow_c/NOTICE.txt nlohmann_json/LICENSE.MIT sobol/LICENSE.txt)
    get_filename_component(_notice_dir "${_notice}" DIRECTORY)
    install(FILES "third_party/${_notice}" DESTINATION "${CMAKE_INSTALL_DATADIR}/AnkuraFathom/licenses/${_notice_dir}"
      COMPONENT Runtime)
  endforeach()
endif()

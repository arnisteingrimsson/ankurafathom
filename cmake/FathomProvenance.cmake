# Content fingerprint of compiled project and vendored headers. Track edits as
# configure dependencies so incremental builds refresh the generated identity.
file(GLOB_RECURSE _provenance_sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.hpp.in"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/bindings/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/bindings/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/bindings/*/CMakeLists.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*.cmake")
list(APPEND _provenance_sources "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT _provenance_sources)
set(_provenance_content "AnkuraFathom.source.v1\n")
foreach(_source IN LISTS _provenance_sources)
  file(SHA256 "${_source}" _hash)
  file(RELATIVE_PATH _relative "${CMAKE_CURRENT_SOURCE_DIR}" "${_source}")
  string(APPEND _provenance_content "${_relative}:${_hash}\n")
endforeach()
string(SHA256 FATHOM_SOURCE_HASH "${_provenance_content}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_provenance_sources})
string(TOUPPER "${CMAKE_BUILD_TYPE}" _provenance_configuration)
set(FATHOM_EFFECTIVE_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_provenance_configuration}} -ffp-contract=off -fno-fast-math")
string(REPLACE "\\" "\\\\" FATHOM_EFFECTIVE_FLAGS "${FATHOM_EFFECTIVE_FLAGS}")
string(REPLACE "\"" "\\\"" FATHOM_EFFECTIVE_FLAGS "${FATHOM_EFFECTIVE_FLAGS}")
set(FATHOM_GIT_COMMIT "")
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE FATHOM_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
configure_file(runtime/provenance/build.hpp.in generated/fathom_build.hpp @ONLY)

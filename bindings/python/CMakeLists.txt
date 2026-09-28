set(_fathom_python_components Interpreter Development.Module)
if(FATHOM_ENABLE_SANITIZERS AND BUILD_TESTING)
  list(APPEND _fathom_python_components Development.Embed)
endif()
# Resolve all components together so headers, embedding library and interpreter match.
find_package(Python 3.10 REQUIRED COMPONENTS ${_fathom_python_components})
execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_VARIABLE _nanobind_dir OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _nanobind_status)
if(NOT _nanobind_status EQUAL 0)
  message(FATAL_ERROR "Selected Python must have nanobind installed")
endif()
find_package(nanobind 3.1 CONFIG REQUIRED PATHS "${_nanobind_dir}" NO_DEFAULT_PATH)
nanobind_add_module(fathom_python NB_STATIC NOMINSIZE NOSTRIP module.cpp)
target_link_libraries(fathom_python PRIVATE fathom_c_api)
target_compile_options(fathom_python PRIVATE -Wall -Wextra -Wpedantic -Werror)
set(_package_dir "${CMAKE_BINARY_DIR}/python/ankurafathom")
set_target_properties(fathom_python PROPERTIES OUTPUT_NAME _native LIBRARY_OUTPUT_DIRECTORY "${_package_dir}")
configure_file(ankurafathom/__init__.py "${_package_dir}/__init__.py" COPYONLY)
if(BUILD_TESTING)
set(_python_test_command "${Python_EXECUTABLE}")
if(FATHOM_ENABLE_SANITIZERS)
  # The host executable loads the sanitizer before Python imports the extension.
  add_executable(fathom_python_test_host ../../tests/python_test_host.cpp)
  target_link_libraries(fathom_python_test_host PRIVATE Python::Python)
  target_compile_definitions(fathom_python_test_host PRIVATE FATHOM_PYTHON_EXECUTABLE="${Python_EXECUTABLE}")
  set(_python_test_command $<TARGET_FILE:fathom_python_test_host>)
endif()
set(_python_test_args "")
if(NOT FATHOM_HAS_ARROW)
  list(APPEND _python_test_args --without-arrow)
endif()
add_test(NAME runtime_python_contract COMMAND ${_python_test_command}
  "${PROJECT_SOURCE_DIR}/tests/python_contract.py" "${CMAKE_BINARY_DIR}/python"
  $<TARGET_FILE:c_api_probe> ${_python_test_args})
set_tests_properties(runtime_python_contract PROPERTIES TIMEOUT 180)
if(FATHOM_HAS_ARROW)
  add_test(NAME ankura_pilot_python COMMAND ${_python_test_command}
    "${PROJECT_SOURCE_DIR}/tests/ankura_pilot_contract.py" $<TARGET_FILE:fathom>
    $<TARGET_FILE:economics_monthly_probe> --package "${CMAKE_BINARY_DIR}/python" ${_parameter_schema_args})
  set_tests_properties(ankura_pilot_python PROPERTIES TIMEOUT 300)
endif()

add_test(NAME runtime_api_provenance_contract COMMAND ${_python_test_command}
  "${PROJECT_SOURCE_DIR}/tests/api_provenance_contract.py" "${CMAKE_BINARY_DIR}/python"
  $<TARGET_FILE:c_api_probe> $<TARGET_FILE:fathom> ${_python_test_args} ${_parameter_schema_args})
set_tests_properties(runtime_api_provenance_contract PROPERTIES TIMEOUT 180)
add_test(NAME runtime_callbacks_contract COMMAND ${_python_test_command}
  "${PROJECT_SOURCE_DIR}/tests/callbacks_contract.py" "${CMAKE_BINARY_DIR}/python" ${_python_test_args})
set_tests_properties(runtime_callbacks_contract PROPERTIES TIMEOUT 180)
endif() # BUILD_TESTING
if(FATHOM_PYTHON_WHEEL)
  set_target_properties(fathom_python PROPERTIES INSTALL_RPATH "${_fathom_origin}/.libs")
  install(TARGETS fathom_python LIBRARY DESTINATION ankurafathom COMPONENT Python)
endif()

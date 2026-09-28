cmake_minimum_required(VERSION 3.20)
project(AnkuraFathom VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(GNUInstallDirs)
option(FATHOM_PYTHON_WHEEL "Build a relocatable Python wheel layout" OFF)
if(FATHOM_PYTHON_WHEEL)
  if(FATHOM_ENABLE_SANITIZERS)
    message(FATAL_ERROR "Sanitizer builds are not distributable wheels")
  endif()
  find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
  set(FATHOM_ARROW_PYTHON "${Python_EXECUTABLE}" CACHE FILEPATH "Wheel Arrow SDK" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FATHOM_ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FATHOM_ENABLE_FUZZING "Build Clang libFuzzer loader/parser targets" OFF)
if(FATHOM_ENABLE_FUZZING)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT FATHOM_ENABLE_SANITIZERS OR FATHOM_PYTHON_WHEEL)
    message(FATAL_ERROR "Fuzzing requires Clang, sanitizers, and a non-wheel build")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()
if(FATHOM_ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(fathom_core src/economics.cpp)
target_include_directories(fathom_core PUBLIC include)
target_compile_options(fathom_core PRIVATE -Wall -Wextra -Wpedantic -Werror PUBLIC -ffp-contract=off -fno-fast-math)

include(cmake/FathomArrow.cmake)
add_library(fathom_outputs runtime/outputs/observations.cpp)
target_link_libraries(fathom_outputs PUBLIC fathom_core)
target_compile_definitions(fathom_outputs PRIVATE FATHOM_HAS_ARROW=$<BOOL:${FATHOM_HAS_ARROW}>)
target_compile_options(fathom_outputs PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(FATHOM_HAS_ARROW)
  target_link_libraries(fathom_outputs PUBLIC ${FATHOM_ARROW_LIBRARIES})
endif()

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE third_party/nlohmann_json/include)
target_link_libraries(fathom_outputs PRIVATE nlohmann_json)

add_library(fathom_data runtime/data/table.cpp)
target_link_libraries(fathom_data PUBLIC fathom_core)
target_compile_definitions(fathom_data PRIVATE FATHOM_HAS_ARROW=$<BOOL:${FATHOM_HAS_ARROW}>)
target_compile_options(fathom_data PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(FATHOM_HAS_ARROW)
  target_link_libraries(fathom_data PRIVATE ${FATHOM_ARROW_LIBRARIES})
endif()

include(cmake/FathomProvenance.cmake)
add_library(fathom_ir src/visualization.cpp src/explanation.cpp src/validation.cpp src/ir.cpp src/data_bindings.cpp src/typed_des.cpp src/typed_des_runtime.cpp src/typed_abm.cpp src/agent_stock_sd.cpp runtime/provenance/manifest.cpp runtime/provenance/bundle.cpp)
target_include_directories(fathom_ir PUBLIC include)
target_include_directories(fathom_ir PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(fathom_ir PRIVATE nlohmann_json PUBLIC fathom_core fathom_outputs fathom_data)
target_compile_options(fathom_ir PRIVATE -Wall -Wextra -Wpedantic -Werror)

set_target_properties(fathom_core fathom_outputs fathom_data fathom_ir PROPERTIES
  POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
add_library(fathom_c_api SHARED runtime/c_api.cpp)
target_include_directories(fathom_c_api PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(fathom_c_api PRIVATE fathom_ir nlohmann_json)
target_include_directories(fathom_c_api PRIVATE src)
target_compile_definitions(fathom_c_api PRIVATE FATHOM_BUILDING_API=1 FATHOM_HAS_ARROW=$<BOOL:${FATHOM_HAS_ARROW}>)
target_compile_options(fathom_c_api PRIVATE -Wall -Wextra -Wpedantic -Werror)
set_target_properties(fathom_c_api PROPERTIES OUTPUT_NAME ankurafathom VERSION 1.0.0 SOVERSION 1
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
set(_fathom_c_exports fathom_abi_version fathom_last_error fathom_load_json fathom_load_file
  fathom_run fathom_run_with_callbacks fathom_results_size fathom_results_observation fathom_results_sha256
  fathom_model_free fathom_results_free fathom_results_arrow fathom_results_manifest fathom_results_arrow_with_manifest)
if(APPLE)
  set(_fathom_exports "")
  foreach(_symbol IN LISTS _fathom_c_exports)
    string(APPEND _fathom_exports "_${_symbol}\n")
  endforeach()
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/fathom_c_api.exports" CONTENT "${_fathom_exports}")
  target_link_options(fathom_c_api PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/generated/fathom_c_api.exports")
  set_property(TARGET fathom_c_api APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/fathom_c_api.exports")
elseif(UNIX)
  string(JOIN ";\n" _fathom_exports ${_fathom_c_exports})
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/fathom_c_api.map"
    CONTENT "ANKURAFATHOM_1 {\nglobal:\n${_fathom_exports};\nlocal: *;\n};\n")
  target_link_options(fathom_c_api PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_BINARY_DIR}/generated/fathom_c_api.map")
  set_property(TARGET fathom_c_api APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/generated/fathom_c_api.map")
endif()

# Pinned upstream oracle. AnkuraFathom's DEVS runtime will be a separate target.
if(BUILD_TESTING)
add_library(adevs_reference STATIC third_party/adevs/src/adevs.cpp)
target_include_directories(adevs_reference PUBLIC third_party/adevs/include)
endif()

add_executable(fathom src/main.cpp)
target_link_libraries(fathom PRIVATE fathom_ir)
target_compile_options(fathom PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
add_executable(c_api_tests tests/c_api_tests.c)
target_link_libraries(c_api_tests PRIVATE fathom_c_api)
target_compile_options(c_api_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_c_api COMMAND c_api_tests ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.ir.json)
add_executable(c_api_probe tests/c_api_probe.c)
target_link_libraries(c_api_probe PRIVATE fathom_c_api)
target_compile_options(c_api_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(c_api_arrow_tests tests/c_api_arrow_tests.c)
target_link_libraries(c_api_arrow_tests PRIVATE fathom_c_api)
target_compile_definitions(c_api_arrow_tests PRIVATE FATHOM_HAS_ARROW=$<BOOL:${FATHOM_HAS_ARROW}>)
target_compile_options(c_api_arrow_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_c_api_arrow COMMAND c_api_arrow_tests ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.ir.json)
add_executable(fathom_tests tests/economics_tests.cpp)
target_link_libraries(fathom_tests PRIVATE fathom_core)
target_compile_options(fathom_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME economics COMMAND fathom_tests ${CMAKE_CURRENT_SOURCE_DIR}/models/synthetic_practices.csv)
add_test(NAME cli_synthetic COMMAND fathom --input ${CMAKE_CURRENT_SOURCE_DIR}/models/synthetic_practices.csv)

add_executable(adevs_schedule_test third_party/adevs/test/sched_test.cpp)
target_link_libraries(adevs_schedule_test PRIVATE adevs_reference)
add_test(NAME adevs_reference_schedule COMMAND adevs_schedule_test)

add_executable(devs_kernel_tests tests/devs_kernel_tests.cpp)
target_link_libraries(devs_kernel_tests PRIVATE fathom_core)
target_compile_options(devs_kernel_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME devs_kernel COMMAND devs_kernel_tests)

add_executable(devs_differential_tests tests/devs_differential_tests.cpp)
target_link_libraries(devs_differential_tests PRIVATE fathom_core adevs_reference)
target_compile_options(devs_differential_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME devs_differential COMMAND devs_differential_tests)

add_executable(devs_devstone_tests tests/conformance/devs/devstone.cpp)
target_link_libraries(devs_devstone_tests PRIVATE fathom_core adevs_reference)
target_compile_options(devs_devstone_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME devs_devstone COMMAND devs_devstone_tests ${CMAKE_CURRENT_BINARY_DIR}/devstone-timings.csv)
set_tests_properties(devs_devstone PROPERTIES TIMEOUT 120)

# Global new/delete interception is confined to these two executables.
# The instrumented workload retains all trace/count oracles; allocation totals
# and timings are diagnostics, never performance thresholds.
find_package(Threads REQUIRED)
target_link_libraries(fathom_core PUBLIC Threads::Threads)
target_link_libraries(c_api_tests PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(c_api_tests PRIVATE m)
endif()
add_executable(allocation_counter_tests tests/allocation_counter_tests.cpp bench/allocation_counter.cpp)
target_include_directories(allocation_counter_tests PRIVATE bench)
target_link_libraries(allocation_counter_tests PRIVATE Threads::Threads)
target_compile_options(allocation_counter_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -fsized-deallocation)
add_test(NAME allocation_counter COMMAND allocation_counter_tests)

add_executable(devs_allocation_bench tests/conformance/devs/devstone.cpp bench/allocation_counter.cpp)
target_include_directories(devs_allocation_bench PRIVATE bench)
target_compile_definitions(devs_allocation_bench PRIVATE FATHOM_ALLOCATION_BENCH=1)
target_link_libraries(devs_allocation_bench PRIVATE fathom_core adevs_reference)
target_compile_options(devs_allocation_bench PRIVATE -Wall -Wextra -Wpedantic -Werror -fsized-deallocation)
add_test(NAME devs_allocation_report COMMAND devs_allocation_bench ${CMAKE_CURRENT_BINARY_DIR}/devstone-allocations.csv)
set_tests_properties(devs_allocation_report PROPERTIES TIMEOUT 120)

add_executable(devs_conformance_tests tests/conformance/devs/models.cpp)
target_link_libraries(devs_conformance_tests PRIVATE fathom_core adevs_reference)
target_compile_options(devs_conformance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME devs_conformance COMMAND devs_conformance_tests)
set_tests_properties(devs_conformance PROPERTIES TIMEOUT 120)
add_executable(devs_dynamic_conformance_tests tests/conformance/devs/dynamic.cpp)
target_link_libraries(devs_dynamic_conformance_tests PRIVATE fathom_core adevs_reference)
target_compile_options(devs_dynamic_conformance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME devs_dynamic_conformance COMMAND devs_dynamic_conformance_tests)

add_executable(devs_hierarchy_conformance_tests tests/conformance/devs/hierarchy.cpp)
target_link_libraries(devs_hierarchy_conformance_tests PRIVATE fathom_core adevs_reference)
target_compile_options(devs_hierarchy_conformance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME devs_hierarchy_conformance COMMAND devs_hierarchy_conformance_tests)
set_tests_properties(devs_hierarchy_conformance PROPERTIES TIMEOUT 120)

add_executable(devs_coupled_tests tests/devs_coupled_tests.cpp)
target_link_libraries(devs_coupled_tests PRIVATE fathom_core)
target_compile_options(devs_coupled_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME devs_coupled COMMAND devs_coupled_tests)

add_executable(sd_model_tests tests/sd_model_tests.cpp)
target_link_libraries(sd_model_tests PRIVATE fathom_core)
target_compile_options(sd_model_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME sd_model COMMAND sd_model_tests)

add_executable(sd_nonlinear_tests tests/sd/nonlinear_tests.cpp)
target_link_libraries(sd_nonlinear_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(sd_nonlinear_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME sd_nonlinear COMMAND sd_nonlinear_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/sd/reference.json
  ${CMAKE_CURRENT_BINARY_DIR}/sd-convergence.csv)
set_tests_properties(sd_nonlinear PROPERTIES TIMEOUT 120)

add_executable(sd_lookup_tests tests/sd_lookup_tests.cpp)
target_link_libraries(sd_lookup_tests PRIVATE fathom_core)
target_compile_options(sd_lookup_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME sd_lookup COMMAND sd_lookup_tests)

add_executable(sd_delay_tests tests/sd_delay_tests.cpp)
target_link_libraries(sd_delay_tests PRIVATE fathom_core)
target_compile_options(sd_delay_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME sd_delay COMMAND sd_delay_tests)

add_executable(des_single_server_tests tests/des_single_server_tests.cpp)
target_link_libraries(des_single_server_tests PRIVATE fathom_core)
target_compile_options(des_single_server_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_single_server COMMAND des_single_server_tests)

add_executable(des_multi_server_tests tests/des_multi_server_tests.cpp)
target_link_libraries(des_multi_server_tests PRIVATE fathom_core)
target_compile_options(des_multi_server_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_multi_server COMMAND des_multi_server_tests)

add_executable(des_resource_pool_tests tests/des_resource_pool_tests.cpp)
target_link_libraries(des_resource_pool_tests PRIVATE fathom_core)
target_compile_options(des_resource_pool_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_resource_pool COMMAND des_resource_pool_tests)

add_executable(hybrid_population_capacity_tests tests/hybrid_population_capacity_tests.cpp)
target_link_libraries(hybrid_population_capacity_tests PRIVATE fathom_core)
target_compile_options(hybrid_population_capacity_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_population_capacity COMMAND hybrid_population_capacity_tests)

add_executable(hybrid_agent_pool_broker_tests tests/hybrid_agent_pool_broker_tests.cpp)
target_link_libraries(hybrid_agent_pool_broker_tests PRIVATE fathom_core)
target_compile_options(hybrid_agent_pool_broker_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_agent_pool_broker COMMAND hybrid_agent_pool_broker_tests)

add_executable(hybrid_agent_pool_transaction_tests tests/hybrid_agent_pool_transaction_tests.cpp)
target_link_libraries(hybrid_agent_pool_transaction_tests PRIVATE fathom_core)
target_compile_options(hybrid_agent_pool_transaction_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_agent_pool_transaction COMMAND hybrid_agent_pool_transaction_tests)
add_executable(hybrid_agent_pool_process_tests tests/hybrid_agent_pool_process_tests.cpp)
target_link_libraries(hybrid_agent_pool_process_tests PRIVATE fathom_core)
target_compile_options(hybrid_agent_pool_process_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_agent_pool_process COMMAND hybrid_agent_pool_process_tests)
add_executable(hybrid_agent_pool_process_atomic_tests tests/hybrid_agent_pool_process_atomic_tests.cpp)
target_link_libraries(hybrid_agent_pool_process_atomic_tests PRIVATE fathom_core)
target_compile_options(hybrid_agent_pool_process_atomic_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_agent_pool_process_atomic COMMAND hybrid_agent_pool_process_atomic_tests)

add_executable(des_process_tests tests/des_process_tests.cpp)
target_link_libraries(des_process_tests PRIVATE fathom_core)
target_compile_options(des_process_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_process COMMAND des_process_tests)

add_executable(des_queue_oracle_tests tests/des_queue_oracle_tests.cpp)
target_link_libraries(des_queue_oracle_tests PRIVATE fathom_core)
target_compile_options(des_queue_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_queue_oracle COMMAND des_queue_oracle_tests)
add_executable(des_clock_tests tests/des_clock_tests.cpp)
target_link_libraries(des_clock_tests PRIVATE fathom_core)
target_compile_options(des_clock_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_clock COMMAND des_clock_tests)
add_executable(des_statistical_tests tests/des_statistical_tests.cpp)
target_link_libraries(des_statistical_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(des_statistical_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_statistical COMMAND des_statistical_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/queue-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/des-statistical-report.json)
add_executable(des_jackson_tests tests/des_jackson_tests.cpp)
target_link_libraries(des_jackson_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(des_jackson_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_jackson COMMAND des_jackson_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/jackson-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/des-jackson-report.json)
add_test(NAME des_branch COMMAND des_jackson_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/branch-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/des-branch-report.json)
add_executable(des_service_tests tests/des_service_tests.cpp)
target_link_libraries(des_service_tests PRIVATE fathom_ir)
target_compile_options(des_service_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_service COMMAND des_service_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/independent_service_process.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_independent_service.ir.json)
add_executable(des_probability_tests tests/des_probability_tests.cpp)
target_link_libraries(des_probability_tests PRIVATE fathom_ir)
target_compile_options(des_probability_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_probability COMMAND des_probability_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/probability_process.ir.json)
add_executable(des_discipline_tests tests/des_discipline_tests.cpp)
target_link_libraries(des_discipline_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(des_discipline_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_discipline COMMAND des_discipline_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/discipline-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/des-discipline-report.json)
add_executable(des_entity_store_tests tests/des_entity_store_tests.cpp)
target_link_libraries(des_entity_store_tests PRIVATE fathom_core)
target_compile_options(des_entity_store_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_entity_store COMMAND des_entity_store_tests)
add_executable(des_reference_process_tests tests/des_reference_process_tests.cpp)
target_link_libraries(des_reference_process_tests PRIVATE fathom_core)
target_compile_options(des_reference_process_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_reference_process COMMAND des_reference_process_tests)
add_executable(des_reference_oracle_tests tests/des_reference_oracle_tests.cpp)
target_link_libraries(des_reference_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(des_reference_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_reference_oracle COMMAND des_reference_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/discipline-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/des-reference-report.json)
add_executable(des_reference_resource_tests tests/des_reference_resource_tests.cpp)
target_link_libraries(des_reference_resource_tests PRIVATE fathom_core)
target_compile_options(des_reference_resource_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_reference_resource COMMAND des_reference_resource_tests)
add_executable(des_reference_resource_oracle_tests tests/des_reference_resource_oracle_tests.cpp)
target_link_libraries(des_reference_resource_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(des_reference_resource_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_reference_resource_oracle COMMAND des_reference_resource_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/discipline-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/des-reference-resource-report.json)
add_executable(des_reference_flow_tests tests/des_reference_flow_tests.cpp)
target_link_libraries(des_reference_flow_tests PRIVATE fathom_core)
target_compile_options(des_reference_flow_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_reference_flow COMMAND des_reference_flow_tests)

add_executable(des_runtime_store_tests tests/des_runtime_store_tests.cpp)
target_link_libraries(des_runtime_store_tests PRIVATE fathom_core)
target_compile_options(des_runtime_store_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_runtime_store COMMAND des_runtime_store_tests)

add_executable(abm_space_tests tests/abm_space_tests.cpp)
target_link_libraries(abm_space_tests PRIVATE fathom_core)
target_compile_options(abm_space_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_space COMMAND abm_space_tests)

add_executable(abm_network_tests tests/abm_network_tests.cpp)
target_link_libraries(abm_network_tests PRIVATE fathom_core)
target_compile_options(abm_network_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_network COMMAND abm_network_tests)

add_executable(abm_broker_tests tests/abm_broker_tests.cpp)
target_link_libraries(abm_broker_tests PRIVATE fathom_core)
target_compile_options(abm_broker_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_broker COMMAND abm_broker_tests)

add_executable(abm_foundation_oracle_tests tests/abm_foundation_oracle_tests.cpp)
target_link_libraries(abm_foundation_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_foundation_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_foundation_oracle COMMAND abm_foundation_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/foundation-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-foundation-report.json)
set_tests_properties(abm_foundation_oracle PROPERTIES FIXTURES_SETUP abm_foundation_report)

add_executable(abm_population_tests tests/abm_population_tests.cpp)
target_link_libraries(abm_population_tests PRIVATE fathom_core)
target_compile_options(abm_population_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_population COMMAND abm_population_tests)

add_executable(abm_typed_population_tests tests/abm_typed_population_tests.cpp)
target_link_libraries(abm_typed_population_tests PRIVATE fathom_core)
target_compile_options(abm_typed_population_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_typed_population COMMAND abm_typed_population_tests)

add_executable(abm_spatial_population_tests tests/abm_spatial_population_tests.cpp)
target_link_libraries(abm_spatial_population_tests PRIVATE fathom_core)
target_compile_options(abm_spatial_population_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_spatial_population COMMAND abm_spatial_population_tests)

add_executable(abm_statechart_tests tests/abm_statechart_tests.cpp)
target_link_libraries(abm_statechart_tests PRIVATE fathom_core)
target_compile_options(abm_statechart_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_statechart COMMAND abm_statechart_tests)

add_executable(abm_generated_publication_tests tests/abm_generated_publication_tests.cpp)
target_link_libraries(abm_generated_publication_tests PRIVATE fathom_core)
target_compile_options(abm_generated_publication_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_generated_publication COMMAND abm_generated_publication_tests)

add_executable(abm_topic_population_tests tests/abm_topic_population_tests.cpp)
target_link_libraries(abm_topic_population_tests PRIVATE fathom_core)
target_compile_options(abm_topic_population_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_topic_population COMMAND abm_topic_population_tests)

add_executable(abm_atomic_tests tests/abm_atomic_tests.cpp)
target_link_libraries(abm_atomic_tests PRIVATE fathom_core)
target_compile_options(abm_atomic_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_atomic COMMAND abm_atomic_tests)

add_executable(hybrid_clock_tests tests/hybrid_clock_tests.cpp)
target_link_libraries(hybrid_clock_tests PRIVATE fathom_core)
target_compile_options(hybrid_clock_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_clock COMMAND hybrid_clock_tests)

add_executable(rng_philox_tests tests/rng_philox_tests.cpp)
target_link_libraries(rng_philox_tests PRIVATE fathom_core)
target_compile_options(rng_philox_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME rng_philox COMMAND rng_philox_tests)

add_executable(hybrid_des_sd_tests tests/hybrid_des_sd_tests.cpp)
target_link_libraries(hybrid_des_sd_tests PRIVATE fathom_core)
target_compile_options(hybrid_des_sd_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_des_sd COMMAND hybrid_des_sd_tests)

add_executable(hybrid_rate_tests tests/hybrid_rate_tests.cpp)
target_link_libraries(hybrid_rate_tests PRIVATE fathom_core)
target_compile_options(hybrid_rate_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_rate COMMAND hybrid_rate_tests)

add_executable(hybrid_population_stock_tests tests/hybrid_population_stock_tests.cpp)
target_link_libraries(hybrid_population_stock_tests PRIVATE fathom_core)
target_compile_options(hybrid_population_stock_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_population_stock COMMAND hybrid_population_stock_tests)

add_executable(hybrid_bass_tests tests/hybrid_bass_tests.cpp)
target_link_libraries(hybrid_bass_tests PRIVATE fathom_core)
target_compile_options(hybrid_bass_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME hybrid_bass COMMAND hybrid_bass_tests)

add_executable(ir_model_tests tests/ir_model_tests.cpp)
add_executable(expression_limits_tests tests/expression_limits_tests.cpp)
add_executable(expression_decision_tests tests/expression_decision_tests.cpp)
target_link_libraries(expression_decision_tests PRIVATE fathom_core)
target_compile_options(expression_decision_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_expression_decisions COMMAND expression_decision_tests)
target_link_libraries(expression_limits_tests PRIVATE fathom_ir)
target_compile_options(expression_limits_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_expression_limits COMMAND expression_limits_tests ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.ir.json)
target_link_libraries(ir_model_tests PRIVATE fathom_ir)
target_compile_options(ir_model_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(ir_agent_pool_tests tests/ir_agent_pool_tests.cpp)
target_link_libraries(ir_agent_pool_tests PRIVATE fathom_ir)
target_compile_options(ir_agent_pool_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_agent_pool COMMAND ir_agent_pool_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool.ir.json)
add_executable(ir_agent_pool_phase_tests tests/ir_agent_pool_phase_tests.cpp)
target_link_libraries(ir_agent_pool_phase_tests PRIVATE fathom_ir)
target_compile_options(ir_agent_pool_phase_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_agent_pool_phases COMMAND ir_agent_pool_phase_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_phases.ir.json)
add_test(NAME ir_agent_pool_phases_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_phases.ir.json)
set_tests_properties(ir_agent_pool_phases_cli PROPERTIES PASS_REGULAR_EXPRESSION "3,capacity,24")
add_executable(ir_agent_pool_delivery_tests tests/ir_agent_pool_delivery_tests.cpp)
target_link_libraries(ir_agent_pool_delivery_tests PRIVATE fathom_ir)
target_compile_options(ir_agent_pool_delivery_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_agent_pool_delivery COMMAND ir_agent_pool_delivery_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_delivery.ir.json)
add_test(NAME ir_agent_pool_delivery_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_delivery.ir.json)
set_tests_properties(ir_agent_pool_delivery_cli PROPERTIES PASS_REGULAR_EXPRESSION "3.5,cycle_total,9.25")
add_executable(ir_agent_pool_sd_tests tests/ir_agent_pool_sd_tests.cpp)
target_link_libraries(ir_agent_pool_sd_tests PRIVATE fathom_ir)
target_compile_options(ir_agent_pool_sd_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_agent_pool_sd COMMAND ir_agent_pool_sd_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_sd.ir.json)
add_test(NAME ir_agent_pool_sd_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_sd.ir.json)
set_tests_properties(ir_agent_pool_sd_cli PROPERTIES PASS_REGULAR_EXPRESSION "4,revenue_time,875")
add_executable(agent_pool_statistics_tests tests/agent_pool_statistics_tests.cpp)
target_link_libraries(agent_pool_statistics_tests PRIVATE fathom_ir)
target_compile_options(agent_pool_statistics_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME agent_pool_statistics COMMAND agent_pool_statistics_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_utilization.ir.json)
add_test(NAME ir_agent_pool_utilization_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool_utilization.ir.json)
set_tests_properties(ir_agent_pool_utilization_cli PROPERTIES PASS_REGULAR_EXPRESSION "4,allocated_time,5.75")
add_test(NAME ir_model COMMAND ir_model_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_symbol.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_unit.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_field.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/lookup.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_table_unit.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/delay.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/delay_chain.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_delay_unit.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/process.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_process_link.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/stochastic_process.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/tandem_process.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_process_branch.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_completion.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_hybrid_bridge_unit.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_stochastic.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_feedback.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_hybrid_feedback_overlap.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_rate.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_hybrid_rate_unit.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_hybrid_rate_stream.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/abm_adoption.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/invalid_abm_rate_unit.ir.json)
add_test(NAME ir_lint_cli COMMAND fathom lint ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.ir.json)
add_test(NAME ir_agent_pool_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_pool.ir.json)
set_tests_properties(ir_agent_pool_cli PROPERTIES PASS_REGULAR_EXPRESSION "2,third_granted,1")
add_test(NAME ir_lookup_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/lookup.ir.json)
set_tests_properties(ir_lookup_cli PROPERTIES PASS_REGULAR_EXPRESSION "2,material_ts,97")
add_test(NAME ir_delay_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/delay.ir.json)
set_tests_properties(ir_delay_cli PROPERTIES PASS_REGULAR_EXPRESSION "4,delivered_ts,21.25")
add_test(NAME ir_process_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/process.ir.json)
add_executable(des_routing_tests tests/des_routing_tests.cpp)
target_link_libraries(des_routing_tests PRIVATE fathom_ir)
target_compile_options(des_routing_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_routing COMMAND des_routing_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/routed_process.ir.json)
add_executable(des_bounded_queue_tests tests/des_bounded_queue_tests.cpp)
target_link_libraries(des_bounded_queue_tests PRIVATE fathom_ir)
target_compile_options(des_bounded_queue_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME des_bounded_queue COMMAND des_bounded_queue_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/bounded_priority_process.ir.json
  ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_completion.ir.json)
add_test(NAME ir_bounded_priority_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/bounded_priority_process.ir.json)
set_tests_properties(ir_bounded_priority_cli PROPERTIES PASS_REGULAR_EXPRESSION "8,cycle_total,17")
set_tests_properties(ir_process_cli PROPERTIES PASS_REGULAR_EXPRESSION "3,cycle_total,7")
add_test(NAME ir_process_experiment_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/process.ir.json
  --experiment ${CMAKE_CURRENT_SOURCE_DIR}/models/process.experiment.json)
set_tests_properties(ir_process_experiment_cli PROPERTIES PASS_REGULAR_EXPRESSION "0,1,3,cycle_total,7")
add_test(NAME ir_stochastic_process_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/stochastic_process.ir.json
  --experiment ${CMAKE_CURRENT_SOURCE_DIR}/models/stochastic_process.experiment.json)
set_tests_properties(ir_stochastic_process_cli PROPERTIES PASS_REGULAR_EXPRESSION "2,2,60,completed,30")
add_test(NAME ir_tandem_process_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/tandem_process.ir.json)
set_tests_properties(ir_tandem_process_cli PROPERTIES PASS_REGULAR_EXPRESSION "5,cycle_total,8")
add_test(NAME ir_hybrid_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_completion.ir.json)
set_tests_properties(ir_hybrid_cli PROPERTIES PASS_REGULAR_EXPRESSION "3,stock_ts,1.59375")
add_test(NAME ir_hybrid_feedback_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_feedback.ir.json)
set_tests_properties(ir_hybrid_feedback_cli PROPERTIES PASS_REGULAR_EXPRESSION "3,stock,3")
add_test(NAME ir_hybrid_rate_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/hybrid_rate.ir.json)
set_tests_properties(ir_hybrid_rate_cli PROPERTIES PASS_REGULAR_EXPRESSION "3,accepted,6")
add_test(NAME ir_abm_adoption_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/abm_adoption.ir.json)
set_tests_properties(ir_abm_adoption_cli PROPERTIES PASS_REGULAR_EXPRESSION "2,adopted_ts,19")
add_test(NAME ir_abm_adoption_experiment_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/abm_adoption.ir.json
  --experiment ${CMAKE_CURRENT_SOURCE_DIR}/models/abm_adoption.experiment.json)
set_tests_properties(ir_abm_adoption_experiment_cli PROPERTIES PASS_REGULAR_EXPRESSION "1,1,2,adopted_ts,3")

add_executable(experiment_tests tests/experiment_tests.cpp)
target_link_libraries(experiment_tests PRIVATE fathom_core)
target_compile_options(experiment_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME experiment COMMAND experiment_tests)
add_test(NAME ir_experiment_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.ir.json
  --experiment ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.experiment.json)
set_tests_properties(ir_experiment_cli PROPERTIES PASS_REGULAR_EXPRESSION "1,1,0,material_ts,100")

add_executable(sd_function_tests tests/sd/function_tests.cpp)
target_link_libraries(sd_function_tests PRIVATE fathom_core)
target_compile_options(sd_function_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME sd_functions COMMAND sd_function_tests)

# Importer and end-to-end contracts use only Python's standard library.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME visualization_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/validation/visualization_contract.py $<TARGET_FILE:fathom>)
set_tests_properties(visualization_contract PROPERTIES TIMEOUT 120)
add_test(NAME explanation_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/validation/explanation_contract.py $<TARGET_FILE:fathom> $<$<NOT:$<BOOL:${FATHOM_HAS_ARROW}>>:--without-arrow>)
add_test(NAME validation_declared_checks COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/validation/declared_checks.py $<TARGET_FILE:fathom>)
add_test(NAME ir_sd_auxiliaries COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/sd_auxiliary_contract.py $<TARGET_FILE:fathom>)
add_executable(economics_monthly_probe tests/economics_monthly_probe.cpp)
target_link_libraries(economics_monthly_probe PRIVATE fathom_core)
target_compile_options(economics_monthly_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_ir_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_ir_contract.py $<TARGET_FILE:fathom>)
add_test(NAME abm_generated_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_generated_contract.py $<TARGET_FILE:fathom>)
add_test(NAME abm_topics_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_topics_contract.py $<TARGET_FILE:fathom>)
add_test(NAME abm_neighbors_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_neighbors_contract.py $<TARGET_FILE:fathom>)
add_test(NAME abm_interaction_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/interaction_plan.py --verify)
add_test(NAME abm_interaction_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/interaction_oracle.py --contract)
add_test(NAME abm_interaction_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/interaction_oracle.py --native $<TARGET_FILE:fathom>)
add_test(NAME abm_foundation_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/foundation_plan.py --verify)
add_test(NAME abm_foundation_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/foundation_oracle.py --contract)
add_test(NAME abm_foundation_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/foundation_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-foundation-report.json)
set_tests_properties(abm_foundation_comparison PROPERTIES FIXTURES_REQUIRED abm_foundation_report)
add_test(NAME des_reference_resource_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/discipline_oracles.py --unbounded-only
  --native ${CMAKE_CURRENT_BINARY_DIR}/des-reference-resource-report.json)
set_tests_properties(des_reference_resource_oracle PROPERTIES FIXTURES_SETUP des_reference_resource_report)
set_tests_properties(des_reference_resource_comparison PROPERTIES FIXTURES_REQUIRED des_reference_resource_report)
add_test(NAME des_reference_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/discipline_oracles.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/des-reference-report.json)
set_tests_properties(des_reference_oracle PROPERTIES FIXTURES_SETUP des_reference_report)
set_tests_properties(des_reference_comparison PROPERTIES FIXTURES_REQUIRED des_reference_report)
add_test(NAME des_discipline_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/discipline_plan.py --verify)
add_test(NAME des_discipline_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/discipline_oracles.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/des-discipline-report.json)
add_test(NAME des_discipline_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_discipline_contract.py
  ${CMAKE_CURRENT_BINARY_DIR}/des-discipline-report.json)
set_tests_properties(des_discipline PROPERTIES FIXTURES_SETUP des_discipline_report)
set_tests_properties(des_discipline_comparison des_discipline_contract PROPERTIES FIXTURES_REQUIRED des_discipline_report)
add_test(NAME des_engine_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_engine_contract.py)
add_test(NAME des_engine_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/compare_engines.py
  --queue ${CMAKE_CURRENT_BINARY_DIR}/des-statistical-report.json
  --jackson ${CMAKE_CURRENT_BINARY_DIR}/des-jackson-report.json
  --branch ${CMAKE_CURRENT_BINARY_DIR}/des-branch-report.json
  --output ${CMAKE_CURRENT_BINARY_DIR}/des-engine-comparison.json)
set_tests_properties(des_statistical PROPERTIES FIXTURES_SETUP des_queue_report)
set_tests_properties(des_jackson PROPERTIES FIXTURES_SETUP des_jackson_report)
set_tests_properties(des_branch PROPERTIES FIXTURES_SETUP des_branch_report)
set_tests_properties(des_engine_comparison PROPERTIES FIXTURES_REQUIRED "des_queue_report;des_jackson_report;des_branch_report")
add_test(NAME des_jackson_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/jackson_plan.py --verify)
add_test(NAME des_branch_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/branch_plan.py --verify)
add_test(NAME des_service_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_service_contract.py $<TARGET_FILE:fathom>)
add_test(NAME des_probability_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_probability_contract.py $<TARGET_FILE:fathom>)
add_test(NAME des_lifo_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_lifo_contract.py $<TARGET_FILE:fathom>)
add_test(NAME des_typed_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_typed_contract.py $<TARGET_FILE:fathom>)
add_test(NAME des_typed_oracle COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_typed_oracle.py $<TARGET_FILE:fathom>
  ${CMAKE_CURRENT_BINARY_DIR}/des-typed-report.json)
add_test(NAME des_statistical_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/des/queue_plan.py --verify)
add_test(NAME des_graph_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/des_graph_contract.py $<TARGET_FILE:fathom>
  ${CMAKE_CURRENT_SOURCE_DIR}/models/routed_process.ir.json)
add_test(NAME ir_functions COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_function_contract.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_import COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_import_tests.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_functions COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_function_tests.py $<TARGET_FILE:fathom>)
add_test(NAME ir_variable_delays COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_variable_delay_tests.py $<TARGET_FILE:fathom>)
add_test(NAME ir_outputs COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_output_tests.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_coverage COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_coverage_tests.py)
add_test(NAME ir_integrators COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_integrator_tests.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_rk4 COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_rk4_tests.py $<TARGET_FILE:fathom>)
add_executable(sd_clipping_tests tests/sd_clipping_tests.cpp)
target_link_libraries(sd_clipping_tests PRIVATE fathom_core)
target_compile_options(sd_clipping_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME sd_clipping COMMAND sd_clipping_tests)
add_test(NAME ir_xmile_clipping COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/clipping_tests.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_delay_source COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_delay_source_tests.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_dialects COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_dialect_tests.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_history COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_history_tests.py $<TARGET_FILE:fathom>)
add_test(NAME xmile_inputs COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/xmile_input_tests.py $<TARGET_FILE:fathom>)

add_executable(sd_analytic_tests tests/sd/analytic_tests.cpp)
target_link_libraries(sd_analytic_tests PRIVATE fathom_core)
target_compile_options(sd_analytic_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME sd_analytic COMMAND sd_analytic_tests ${CMAKE_CURRENT_BINARY_DIR}/sd-analytic.csv)
set_tests_properties(sd_analytic PROPERTIES TIMEOUT 120)

add_executable(abm_lifecycle_tests tests/abm_lifecycle_tests.cpp)
target_link_libraries(abm_lifecycle_tests PRIVATE fathom_core)
target_compile_options(abm_lifecycle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_lifecycle COMMAND abm_lifecycle_tests)
add_test(NAME abm_lifecycle_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_lifecycle_contract.py $<TARGET_FILE:fathom>)

add_executable(abm_behavior_tests tests/abm_behavior_tests.cpp)
target_link_libraries(abm_behavior_tests PRIVATE fathom_core)
target_compile_options(abm_behavior_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_behavior COMMAND abm_behavior_tests)
add_test(NAME abm_behavior_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_behavior_contract.py $<TARGET_FILE:fathom>)

add_executable(abm_network_population_tests tests/abm_network_population_tests.cpp)
target_link_libraries(abm_network_population_tests PRIVATE fathom_core)
target_compile_options(abm_network_population_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_network_population COMMAND abm_network_population_tests)
add_test(NAME abm_network_updates_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_network_updates_contract.py $<TARGET_FILE:fathom>)

add_executable(abm_generator_oracle_tests tests/abm_generator_oracle_tests.cpp)
target_link_libraries(abm_generator_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_generator_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_generator_native COMMAND abm_generator_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/generator-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-generator-graphs.jsonl)
set_tests_properties(abm_generator_native PROPERTIES FIXTURES_SETUP abm_generator_report TIMEOUT 180)
add_test(NAME abm_generator_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/generator_plan.py --verify)
add_test(NAME abm_generator_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/generator_oracle.py --contract)
add_test(NAME abm_generator_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/generator_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-generator-graphs.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-generator-report.json)
set_tests_properties(abm_generator_comparison PROPERTIES FIXTURES_REQUIRED abm_generator_report TIMEOUT 120)
add_test(NAME abm_generator_ir_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/abm_generator_ir_contract.py $<TARGET_FILE:fathom>)

add_executable(fathom_wealth examples/wealth_exchange.cpp)
target_link_libraries(fathom_wealth PRIVATE fathom_core)
target_compile_options(fathom_wealth PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(abm_wealth_tests tests/abm_wealth_tests.cpp)
target_link_libraries(abm_wealth_tests PRIVATE fathom_core)
target_compile_options(abm_wealth_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_wealth COMMAND abm_wealth_tests)
add_executable(abm_wealth_oracle_tests tests/abm_wealth_oracle_tests.cpp)
target_link_libraries(abm_wealth_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_wealth_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_wealth_native COMMAND abm_wealth_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/wealth-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-wealth-trajectories.jsonl)
set_tests_properties(abm_wealth_native PROPERTIES FIXTURES_SETUP abm_wealth_report TIMEOUT 300)
add_test(NAME abm_wealth_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/wealth_plan.py --verify)
add_test(NAME abm_wealth_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/wealth_oracle.py --contract)
add_test(NAME abm_wealth_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/wealth_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-wealth-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-wealth-report.json)
set_tests_properties(abm_wealth_comparison PROPERTIES FIXTURES_REQUIRED abm_wealth_report TIMEOUT 120)

add_executable(fathom_schelling examples/schelling.cpp)
target_link_libraries(fathom_schelling PRIVATE fathom_core)
target_compile_options(fathom_schelling PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(abm_schelling_tests tests/abm_schelling_tests.cpp)
target_link_libraries(abm_schelling_tests PRIVATE fathom_core)
target_compile_options(abm_schelling_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_schelling COMMAND abm_schelling_tests)
add_executable(abm_schelling_oracle_tests tests/abm_schelling_oracle_tests.cpp)
target_link_libraries(abm_schelling_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_schelling_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_schelling_native COMMAND abm_schelling_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/schelling-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-schelling-trajectories.jsonl)
set_tests_properties(abm_schelling_native PROPERTIES FIXTURES_SETUP abm_schelling_report TIMEOUT 300)
add_test(NAME abm_schelling_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/schelling_plan.py --verify)
add_test(NAME abm_schelling_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/schelling_oracle.py --contract)
add_test(NAME abm_schelling_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/schelling_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-schelling-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-schelling-report.json)
set_tests_properties(abm_schelling_comparison PROPERTIES FIXTURES_REQUIRED abm_schelling_report TIMEOUT 120)

add_executable(fathom_boids examples/boids.cpp)
target_link_libraries(fathom_boids PRIVATE fathom_core)
target_compile_options(fathom_boids PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_executable(abm_boids_tests tests/abm_boids_tests.cpp)
target_link_libraries(abm_boids_tests PRIVATE fathom_core)
target_compile_options(abm_boids_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME abm_boids COMMAND abm_boids_tests)
add_executable(abm_boids_oracle_tests tests/abm_boids_oracle_tests.cpp)
target_link_libraries(abm_boids_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_boids_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME abm_boids_native COMMAND abm_boids_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/boids-plan.json
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/boids-reference.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-boids-trajectories.jsonl)
set_tests_properties(abm_boids_native PROPERTIES FIXTURES_SETUP abm_boids_report TIMEOUT 600)
add_test(NAME abm_boids_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/boids_plan.py --verify)
add_test(NAME abm_boids_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/boids_oracle.py --contract)
add_test(NAME abm_boids_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/boids_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-boids-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-boids-report.json)
set_tests_properties(abm_boids_comparison PROPERTIES FIXTURES_REQUIRED abm_boids_report TIMEOUT 120)

add_executable(fathom_sugarscape examples/sugarscape.cpp)
target_link_libraries(fathom_sugarscape PRIVATE fathom_core)
target_compile_options(fathom_sugarscape PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(abm_sugarscape_tests tests/abm_sugarscape_tests.cpp)
target_link_libraries(abm_sugarscape_tests PRIVATE fathom_core)
target_compile_options(abm_sugarscape_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_sugarscape COMMAND abm_sugarscape_tests)
add_executable(abm_sugarscape_oracle_tests tests/abm_sugarscape_oracle_tests.cpp)
target_link_libraries(abm_sugarscape_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_sugarscape_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME abm_sugarscape_native COMMAND abm_sugarscape_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sugarscape-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-sugarscape-trajectories.jsonl)
set_tests_properties(abm_sugarscape_native PROPERTIES FIXTURES_SETUP abm_sugarscape_report TIMEOUT 300)
add_test(NAME abm_sugarscape_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sugarscape_plan.py --verify)
add_test(NAME abm_sugarscape_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sugarscape_oracle.py --contract)
add_test(NAME abm_sugarscape_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sugarscape_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-sugarscape-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-sugarscape-report.json)
set_tests_properties(abm_sugarscape_comparison PROPERTIES FIXTURES_REQUIRED abm_sugarscape_report TIMEOUT 120)

add_executable(fathom_sir examples/sir.cpp)
target_link_libraries(fathom_sir PRIVATE fathom_core)
target_compile_options(fathom_sir PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_executable(abm_sir_tests tests/abm_sir_tests.cpp)
target_link_libraries(abm_sir_tests PRIVATE fathom_core)
target_compile_options(abm_sir_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME abm_sir COMMAND abm_sir_tests)
add_executable(abm_sir_oracle_tests tests/abm_sir_oracle_tests.cpp)
target_link_libraries(abm_sir_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_sir_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME abm_sir_native COMMAND abm_sir_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-trajectories.jsonl)
set_tests_properties(abm_sir_native PROPERTIES FIXTURES_SETUP abm_sir_report TIMEOUT 300)
add_test(NAME abm_sir_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_plan.py --verify)
add_test(NAME abm_sir_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_oracle.py --contract)
add_test(NAME abm_sir_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-report.json)
set_tests_properties(abm_sir_comparison PROPERTIES FIXTURES_REQUIRED abm_sir_report TIMEOUT 120)

add_executable(fathom_sir_async examples/sir_async.cpp)
target_link_libraries(fathom_sir_async PRIVATE fathom_core)
target_compile_options(fathom_sir_async PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_executable(abm_sir_async_tests tests/abm_sir_async_tests.cpp)
target_link_libraries(abm_sir_async_tests PRIVATE fathom_core)
target_compile_options(abm_sir_async_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME abm_sir_async COMMAND abm_sir_async_tests)
add_executable(abm_sir_async_oracle_tests tests/abm_sir_async_oracle_tests.cpp)
target_link_libraries(abm_sir_async_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(abm_sir_async_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME abm_sir_async_native COMMAND abm_sir_async_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir-async-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-async-trajectories.jsonl
  ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-convergence-trajectories.jsonl)
set_tests_properties(abm_sir_async_native PROPERTIES FIXTURES_SETUP abm_sir_async_report TIMEOUT 600)
add_test(NAME abm_sir_async_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_async_plan.py --verify)
add_test(NAME abm_sir_async_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_async_oracle.py --contract)
add_test(NAME abm_sir_async_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_async_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-async-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-async-report.json)
set_tests_properties(abm_sir_async_comparison PROPERTIES FIXTURES_REQUIRED abm_sir_async_report TIMEOUT 120)
add_test(NAME abm_sir_convergence_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_convergence.py --contract)
add_test(NAME abm_sir_convergence COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/abm/sir_convergence.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-convergence-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/abm-sir-convergence-report.json)
set_tests_properties(abm_sir_convergence PROPERTIES FIXTURES_REQUIRED abm_sir_async_report TIMEOUT 120)

add_executable(hybrid_sir_tests tests/hybrid_sir_tests.cpp)
target_link_libraries(hybrid_sir_tests PRIVATE fathom_core)
target_compile_options(hybrid_sir_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_sir COMMAND hybrid_sir_tests)
add_executable(hybrid_sir_oracle_tests tests/hybrid_sir_oracle_tests.cpp)
target_link_libraries(hybrid_sir_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_sir_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_sir_native COMMAND hybrid_sir_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/sir-mean-field-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-sir-trajectories.jsonl)
set_tests_properties(hybrid_sir_native PROPERTIES FIXTURES_SETUP hybrid_sir_report TIMEOUT 1200)
add_test(NAME hybrid_sir_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/sir_mean_field_plan.py --verify)
add_test(NAME hybrid_sir_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/sir_mean_field_oracle.py --contract)
add_test(NAME hybrid_sir_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/sir_mean_field_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-sir-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-sir-report.json)
set_tests_properties(hybrid_sir_comparison PROPERTIES FIXTURES_REQUIRED hybrid_sir_report TIMEOUT 120)

add_executable(bass_tests tests/bass_tests.cpp)
add_executable(fathom_bass examples/bass.cpp)
target_link_libraries(fathom_bass PRIVATE fathom_core)
target_compile_options(fathom_bass PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
target_link_libraries(bass_tests PRIVATE fathom_core)
target_compile_options(bass_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME bass COMMAND bass_tests)
add_executable(hybrid_bass_oracle_tests tests/hybrid_bass_oracle_tests.cpp)
target_link_libraries(hybrid_bass_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_bass_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_bass_native COMMAND hybrid_bass_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/bass-mean-field-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-bass-trajectories.jsonl)
set_tests_properties(hybrid_bass_native PROPERTIES FIXTURES_SETUP hybrid_bass_report TIMEOUT 1800)
add_test(NAME hybrid_bass_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/bass_mean_field_plan.py --verify)
add_test(NAME hybrid_bass_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/bass_mean_field_oracle.py --contract)
add_test(NAME hybrid_bass_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/bass_mean_field_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-bass-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-bass-report.json)
set_tests_properties(hybrid_bass_comparison PROPERTIES FIXTURES_REQUIRED hybrid_bass_report TIMEOUT 120)

add_executable(hybrid_fluid_tests tests/hybrid_fluid_tests.cpp)
add_executable(fathom_queue_backlog examples/queue_backlog.cpp)
target_link_libraries(fathom_queue_backlog PRIVATE fathom_core)
target_compile_options(fathom_queue_backlog PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
target_link_libraries(hybrid_fluid_tests PRIVATE fathom_core)
target_compile_options(hybrid_fluid_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_fluid COMMAND hybrid_fluid_tests)
add_executable(hybrid_fluid_oracle_tests tests/hybrid_fluid_oracle_tests.cpp)
target_link_libraries(hybrid_fluid_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_fluid_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_fluid_native COMMAND hybrid_fluid_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/des-fluid-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-fluid-trajectories.jsonl)
set_tests_properties(hybrid_fluid_native PROPERTIES FIXTURES_SETUP hybrid_fluid_report TIMEOUT 1800)
add_test(NAME hybrid_fluid_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/des_fluid_plan.py --verify)
add_test(NAME hybrid_fluid_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/des_fluid_oracle.py --contract)
add_test(NAME hybrid_fluid_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/des_fluid_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-fluid-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-fluid-report.json)
set_tests_properties(hybrid_fluid_comparison PROPERTIES FIXTURES_REQUIRED hybrid_fluid_report TIMEOUT 120)

add_executable(hybrid_agent_stocks_tests tests/hybrid_agent_stocks_tests.cpp)
add_executable(fathom_agent_stocks examples/agent_stocks.cpp)
add_executable(hybrid_agent_stocks_oracle_tests tests/hybrid_agent_stocks_oracle_tests.cpp)
foreach(target hybrid_agent_stocks_tests fathom_agent_stocks hybrid_agent_stocks_oracle_tests)
  target_link_libraries(${target} PRIVATE fathom_core)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
endforeach()
target_link_libraries(hybrid_agent_stocks_oracle_tests PRIVATE nlohmann_json)
add_test(NAME hybrid_agent_stocks COMMAND hybrid_agent_stocks_tests)
add_test(NAME hybrid_agent_stocks_native COMMAND hybrid_agent_stocks_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent-stock-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-agent-stocks-trajectories.jsonl)
set_tests_properties(hybrid_agent_stocks_native PROPERTIES FIXTURES_SETUP hybrid_agent_stocks_report)
add_test(NAME hybrid_agent_stocks_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stocks_oracle.py --contract)
add_test(NAME hybrid_agent_stocks_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stocks_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-agent-stocks-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-agent-stocks-report.json)
set_tests_properties(hybrid_agent_stocks_comparison PROPERTIES FIXTURES_REQUIRED hybrid_agent_stocks_report)

add_executable(hybrid_typed_aggregate_tests tests/hybrid_typed_aggregate_tests.cpp)
target_link_libraries(hybrid_typed_aggregate_tests PRIVATE fathom_core)
target_compile_options(hybrid_typed_aggregate_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_typed_aggregate COMMAND hybrid_typed_aggregate_tests)
add_executable(hybrid_typed_aggregate_oracle_tests tests/hybrid_typed_aggregate_oracle_tests.cpp)
add_executable(fathom_typed_aggregate examples/typed_aggregate.cpp)
foreach(target hybrid_typed_aggregate_oracle_tests fathom_typed_aggregate)
  target_link_libraries(${target} PRIVATE fathom_core)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
endforeach()
target_link_libraries(hybrid_typed_aggregate_oracle_tests PRIVATE nlohmann_json)
add_test(NAME hybrid_typed_aggregate_native COMMAND hybrid_typed_aggregate_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed-aggregate-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-aggregate-trajectories.jsonl)
set_tests_properties(hybrid_typed_aggregate_native PROPERTIES FIXTURES_SETUP hybrid_typed_aggregate_report)
add_test(NAME hybrid_typed_aggregate_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_aggregate_oracle.py --verify)
add_test(NAME hybrid_typed_aggregate_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_aggregate_oracle.py --contract)
add_test(NAME hybrid_typed_aggregate_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_aggregate_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-aggregate-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-aggregate-report.json)
set_tests_properties(hybrid_typed_aggregate_comparison PROPERTIES FIXTURES_REQUIRED hybrid_typed_aggregate_report)

add_executable(hybrid_agent_stocks_atomic_tests tests/hybrid_agent_stocks_atomic_tests.cpp)
target_link_libraries(hybrid_agent_stocks_atomic_tests PRIVATE fathom_core)
target_compile_options(hybrid_agent_stocks_atomic_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_agent_stocks_atomic COMMAND hybrid_agent_stocks_atomic_tests)
add_executable(fathom_agent_stocks_coupled examples/agent_stocks_coupled.cpp)
target_link_libraries(fathom_agent_stocks_coupled PRIVATE fathom_core)
target_compile_options(fathom_agent_stocks_coupled PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_executable(hybrid_agent_stocks_atomic_oracle_tests tests/hybrid_agent_stocks_atomic_oracle_tests.cpp)
target_link_libraries(hybrid_agent_stocks_atomic_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_agent_stocks_atomic_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_agent_stocks_atomic_native COMMAND hybrid_agent_stocks_atomic_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent-stocks-atomic-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-agent-stocks-atomic-trajectories.jsonl)
set_tests_properties(hybrid_agent_stocks_atomic_native PROPERTIES FIXTURES_SETUP hybrid_agent_stocks_atomic_report)
add_test(NAME hybrid_agent_stocks_atomic_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stocks_atomic_oracle.py --verify)
add_test(NAME hybrid_agent_stocks_atomic_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stocks_atomic_oracle.py --contract)
add_test(NAME hybrid_agent_stocks_atomic_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stocks_atomic_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-agent-stocks-atomic-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-agent-stocks-atomic-report.json)
set_tests_properties(hybrid_agent_stocks_atomic_comparison PROPERTIES FIXTURES_REQUIRED hybrid_agent_stocks_atomic_report)

add_executable(hybrid_population_result_tests tests/hybrid_population_result_tests.cpp)
target_link_libraries(hybrid_population_result_tests PRIVATE fathom_core)
target_compile_options(hybrid_population_result_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_population_result COMMAND hybrid_population_result_tests)

add_executable(ir_agent_stock_sd_tests tests/ir_agent_stock_sd_tests.cpp)
target_link_libraries(ir_agent_stock_sd_tests PRIVATE fathom_ir nlohmann_json)
target_compile_options(ir_agent_stock_sd_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME ir_agent_stock_sd COMMAND ir_agent_stock_sd_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_stock_sd.ir.json
  ${CMAKE_CURRENT_BINARY_DIR}/agent-stock-sd-test-input.json)
add_test(NAME ir_agent_stock_sd_cli COMMAND fathom run ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_stock_sd.ir.json)
add_test(NAME ir_agent_stock_sd_experiment_cli COMMAND fathom run
  ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_stock_sd.ir.json
  --experiment ${CMAKE_CURRENT_SOURCE_DIR}/models/agent_stock_sd.experiment.json)
add_test(NAME ir_agent_stock_sd_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stock_ir_oracle.py --verify)
add_test(NAME ir_agent_stock_sd_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stock_ir_oracle.py --contract)
add_test(NAME ir_agent_stock_sd_oracle COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/agent_stock_ir_oracle.py --fathom $<TARGET_FILE:fathom>
  --report ${CMAKE_CURRENT_BINARY_DIR}/ir-agent-stock-sd-report.json)

add_executable(hybrid_typed_pulse_tests tests/hybrid_typed_pulse_tests.cpp)
target_link_libraries(hybrid_typed_pulse_tests PRIVATE fathom_core)
target_compile_options(hybrid_typed_pulse_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_typed_pulse COMMAND hybrid_typed_pulse_tests)
add_executable(hybrid_typed_pulse_oracle_tests tests/hybrid_typed_pulse_oracle_tests.cpp)
target_link_libraries(hybrid_typed_pulse_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_typed_pulse_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_typed_pulse_native COMMAND hybrid_typed_pulse_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed-pulse-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-pulse-trajectories.jsonl)
set_tests_properties(hybrid_typed_pulse_native PROPERTIES FIXTURES_SETUP hybrid_typed_pulse_report)
add_test(NAME hybrid_typed_pulse_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_pulse_oracle.py --verify)
add_test(NAME hybrid_typed_pulse_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_pulse_oracle.py --contract)
add_test(NAME hybrid_typed_pulse_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_pulse_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-pulse-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-pulse-report.json)
set_tests_properties(hybrid_typed_pulse_comparison PROPERTIES FIXTURES_REQUIRED hybrid_typed_pulse_report)
add_executable(fathom_typed_event_pulse examples/typed_event_pulse.cpp)
target_link_libraries(fathom_typed_event_pulse PRIVATE fathom_core)
target_compile_options(fathom_typed_event_pulse PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

add_executable(hybrid_lifecycle_bridge_tests tests/hybrid_lifecycle_bridge_tests.cpp)
target_link_libraries(hybrid_lifecycle_bridge_tests PRIVATE fathom_core)
target_compile_options(hybrid_lifecycle_bridge_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_lifecycle_bridge COMMAND hybrid_lifecycle_bridge_tests)
add_executable(hybrid_lifecycle_bridge_oracle_tests tests/hybrid_lifecycle_bridge_oracle_tests.cpp)
target_link_libraries(hybrid_lifecycle_bridge_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_lifecycle_bridge_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_lifecycle_bridge_native COMMAND hybrid_lifecycle_bridge_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/lifecycle-bridge-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-lifecycle-bridge-trajectories.jsonl)
set_tests_properties(hybrid_lifecycle_bridge_native PROPERTIES FIXTURES_SETUP hybrid_lifecycle_bridge_report)
add_test(NAME hybrid_lifecycle_bridge_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/lifecycle_bridge_oracle.py --verify)
add_test(NAME hybrid_lifecycle_bridge_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/lifecycle_bridge_oracle.py --contract)
add_test(NAME hybrid_lifecycle_bridge_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/lifecycle_bridge_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-lifecycle-bridge-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-lifecycle-bridge-report.json)
set_tests_properties(hybrid_lifecycle_bridge_comparison PROPERTIES FIXTURES_REQUIRED hybrid_lifecycle_bridge_report)
add_executable(fathom_typed_lifecycle examples/typed_lifecycle.cpp)
target_link_libraries(fathom_typed_lifecycle PRIVATE fathom_core)
target_compile_options(fathom_typed_lifecycle PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

add_executable(hybrid_entity_agent_tests tests/hybrid_entity_agent_tests.cpp)
target_link_libraries(hybrid_entity_agent_tests PRIVATE fathom_core)
target_compile_options(hybrid_entity_agent_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_entity_agent COMMAND hybrid_entity_agent_tests)
add_executable(hybrid_entity_agent_oracle_tests tests/hybrid_entity_agent_oracle_tests.cpp)
target_link_libraries(hybrid_entity_agent_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_entity_agent_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_entity_agent_native COMMAND hybrid_entity_agent_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/entity-agent-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-entity-agent-trajectories.jsonl)
set_tests_properties(hybrid_entity_agent_native PROPERTIES FIXTURES_SETUP hybrid_entity_agent_report)
add_test(NAME hybrid_entity_agent_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/entity_agent_oracle.py --verify)
add_test(NAME hybrid_entity_agent_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/entity_agent_oracle.py --contract)
add_test(NAME hybrid_entity_agent_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/entity_agent_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-entity-agent-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-entity-agent-report.json)
set_tests_properties(hybrid_entity_agent_comparison PROPERTIES FIXTURES_REQUIRED hybrid_entity_agent_report)
add_executable(fathom_entity_agent examples/entity_agent.cpp)
target_link_libraries(fathom_entity_agent PRIVATE fathom_core)
target_compile_options(fathom_entity_agent PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

add_executable(hybrid_signal_rate_tests tests/hybrid_signal_rate_tests.cpp)
target_link_libraries(hybrid_signal_rate_tests PRIVATE fathom_core)
target_compile_options(hybrid_signal_rate_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_signal_rate COMMAND hybrid_signal_rate_tests)
add_executable(hybrid_signal_rate_oracle_tests tests/hybrid_signal_rate_oracle_tests.cpp)
target_link_libraries(hybrid_signal_rate_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_signal_rate_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_signal_rate_native COMMAND hybrid_signal_rate_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/signal-rate-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-signal-rate-trajectories.jsonl)
set_tests_properties(hybrid_signal_rate_native PROPERTIES FIXTURES_SETUP hybrid_signal_rate_report)
add_test(NAME hybrid_signal_rate_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/signal_rate_oracle.py --verify)
add_test(NAME hybrid_signal_rate_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/signal_rate_oracle.py --contract)
add_test(NAME hybrid_signal_rate_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/signal_rate_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-signal-rate-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-signal-rate-report.json)
set_tests_properties(hybrid_signal_rate_comparison PROPERTIES FIXTURES_REQUIRED hybrid_signal_rate_report)
add_executable(fathom_signal_rate examples/signal_rate.cpp)
target_link_libraries(fathom_signal_rate PRIVATE fathom_core)
target_compile_options(fathom_signal_rate PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

add_executable(hybrid_typed_pool_tests tests/hybrid_typed_pool_tests.cpp)
target_link_libraries(hybrid_typed_pool_tests PRIVATE fathom_core)
target_compile_options(hybrid_typed_pool_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_typed_pool COMMAND hybrid_typed_pool_tests)
add_executable(hybrid_typed_pool_oracle_tests tests/hybrid_typed_pool_oracle_tests.cpp)
target_link_libraries(hybrid_typed_pool_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_typed_pool_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_typed_pool_native COMMAND hybrid_typed_pool_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed-pool-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-pool-trajectories.jsonl)
set_tests_properties(hybrid_typed_pool_native PROPERTIES FIXTURES_SETUP hybrid_typed_pool_report)
add_test(NAME hybrid_typed_pool_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_pool_oracle.py --verify)
add_test(NAME hybrid_typed_pool_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_pool_oracle.py --contract)
add_test(NAME hybrid_typed_pool_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/typed_pool_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-pool-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-typed-pool-report.json)
set_tests_properties(hybrid_typed_pool_comparison PROPERTIES FIXTURES_REQUIRED hybrid_typed_pool_report)
add_executable(fathom_typed_agent_pool examples/typed_agent_pool.cpp)
target_link_libraries(fathom_typed_agent_pool PRIVATE fathom_core)
target_compile_options(fathom_typed_agent_pool PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

add_executable(hybrid_dynamic_agent_stocks_tests tests/hybrid_dynamic_agent_stocks_tests.cpp)
target_link_libraries(hybrid_dynamic_agent_stocks_tests PRIVATE fathom_core)
target_compile_options(hybrid_dynamic_agent_stocks_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_dynamic_agent_stocks COMMAND hybrid_dynamic_agent_stocks_tests)
add_executable(hybrid_dynamic_agent_stocks_oracle_tests tests/hybrid_dynamic_agent_stocks_oracle_tests.cpp)
target_link_libraries(hybrid_dynamic_agent_stocks_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(hybrid_dynamic_agent_stocks_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)
add_test(NAME hybrid_dynamic_agent_stocks_native COMMAND hybrid_dynamic_agent_stocks_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/dynamic-agent-stocks-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/hybrid-dynamic-agent-stocks-trajectories.jsonl)
set_tests_properties(hybrid_dynamic_agent_stocks_native PROPERTIES FIXTURES_SETUP hybrid_dynamic_agent_stocks_report)
add_test(NAME hybrid_dynamic_agent_stocks_plan COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/dynamic_agent_stocks_oracle.py --verify)
add_test(NAME hybrid_dynamic_agent_stocks_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/dynamic_agent_stocks_oracle.py --contract)
add_test(NAME hybrid_dynamic_agent_stocks_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/hybrid/dynamic_agent_stocks_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/hybrid-dynamic-agent-stocks-trajectories.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/hybrid-dynamic-agent-stocks-report.json)
set_tests_properties(hybrid_dynamic_agent_stocks_comparison PROPERTIES FIXTURES_REQUIRED hybrid_dynamic_agent_stocks_report)
add_executable(fathom_dynamic_agent_stocks examples/dynamic_agent_stocks.cpp)
target_link_libraries(fathom_dynamic_agent_stocks PRIVATE fathom_core)
target_compile_options(fathom_dynamic_agent_stocks PRIVATE -Wall -Wextra -Wpedantic -Werror -ffp-contract=off -fno-fast-math)

add_executable(experiment_parallel_tests tests/experiment_parallel_tests.cpp)
target_link_libraries(experiment_parallel_tests PRIVATE fathom_core)
target_compile_options(experiment_parallel_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME experiment_parallel COMMAND experiment_parallel_tests)
add_test(NAME experiment_cli_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/experiment_cli_contract.py $<TARGET_FILE:fathom>
  ${CMAKE_CURRENT_BINARY_DIR}/experiment-cli-report.json)

add_executable(experiment_design_tests tests/experiment_design_tests.cpp)
target_link_libraries(experiment_design_tests PRIVATE fathom_core)
target_compile_options(experiment_design_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME experiment_design COMMAND experiment_design_tests)
add_executable(experiment_design_oracle_tests tests/experiment_design_oracle_tests.cpp)
target_link_libraries(experiment_design_oracle_tests PRIVATE fathom_core nlohmann_json)
target_compile_options(experiment_design_oracle_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME experiment_design_native COMMAND experiment_design_oracle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/runtime/design-plan.json
  ${CMAKE_CURRENT_BINARY_DIR}/experiment-designs.jsonl)
set_tests_properties(experiment_design_native PROPERTIES FIXTURES_SETUP experiment_design_report)
add_test(NAME experiment_design_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/runtime/design_oracle.py --contract)
add_test(NAME experiment_design_comparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/oracles/runtime/design_oracle.py
  --native ${CMAKE_CURRENT_BINARY_DIR}/experiment-designs.jsonl
  --report ${CMAKE_CURRENT_BINARY_DIR}/experiment-design-report.json)
set_tests_properties(experiment_design_comparison PROPERTIES FIXTURES_REQUIRED experiment_design_report)
add_test(NAME experiment_design_cli COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/experiment_design_cli.py $<TARGET_FILE:fathom>
  ${CMAKE_CURRENT_BINARY_DIR}/experiment-design-cli-report.json)

add_executable(runtime_outputs_tests tests/runtime_outputs_tests.cpp)
target_link_libraries(runtime_outputs_tests PRIVATE fathom_outputs)
target_compile_definitions(runtime_outputs_tests PRIVATE FATHOM_HAS_ARROW=$<BOOL:${FATHOM_HAS_ARROW}>)
target_compile_options(runtime_outputs_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_outputs COMMAND runtime_outputs_tests ${CMAKE_CURRENT_BINARY_DIR}/output-fixtures)
set_tests_properties(runtime_outputs PROPERTIES FIXTURES_SETUP output_fixtures)
if(FATHOM_HAS_ARROW)
  set(_output_python "${Python3_EXECUTABLE}")
  if(FATHOM_ARROW_PYTHON)
    set(_output_python "${FATHOM_ARROW_PYTHON}")
  endif()
  execute_process(COMMAND "${_output_python}" -c "import pyarrow" RESULT_VARIABLE _output_python_status
    OUTPUT_QUIET ERROR_QUIET)
  if(_output_python_status EQUAL 0)
    add_test(NAME runtime_outputs_contract COMMAND "${_output_python}"
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_outputs_contract.py $<TARGET_FILE:fathom>
      --fixtures ${CMAKE_CURRENT_BINARY_DIR}/output-fixtures
      --report ${CMAKE_CURRENT_BINARY_DIR}/runtime-outputs-report.json)
    set_tests_properties(runtime_outputs_contract PROPERTIES FIXTURES_REQUIRED output_fixtures TIMEOUT 180)
  else()
    message(STATUS "PyArrow reader unavailable: only native output tests are configured")
  endif()
else()
  add_test(NAME runtime_outputs_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_outputs_contract.py $<TARGET_FILE:fathom>
    --without-arrow --report ${CMAKE_CURRENT_BINARY_DIR}/runtime-outputs-report.json)
endif()

add_executable(sha256_tests tests/sha256_tests.cpp)
target_link_libraries(sha256_tests PRIVATE fathom_core)
target_compile_options(sha256_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_sha256 COMMAND sha256_tests ${CMAKE_CURRENT_BINARY_DIR}/sha256-report.json)
set_tests_properties(runtime_sha256 PROPERTIES FIXTURES_SETUP sha256_report)
add_test(NAME runtime_sha256_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/sha256_contract.py ${CMAKE_CURRENT_BINARY_DIR}/sha256-report.json)
set_tests_properties(runtime_sha256_contract PROPERTIES FIXTURES_REQUIRED sha256_report)
add_executable(runtime_data_probe tests/runtime_data_probe.cpp)
target_link_libraries(runtime_data_probe PRIVATE fathom_data nlohmann_json)
target_compile_options(runtime_data_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(fathom_data_table examples/data_table.cpp)
target_link_libraries(fathom_data_table PRIVATE fathom_data)
target_compile_options(fathom_data_table PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(population_init_tests tests/population_init_tests.cpp)
target_link_libraries(population_init_tests PRIVATE fathom_data)
target_compile_options(population_init_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_population_init COMMAND population_init_tests ${CMAKE_CURRENT_BINARY_DIR}/population-init-fixtures)
add_executable(data_inputs_tests tests/data_inputs_tests.cpp)
target_link_libraries(data_inputs_tests PRIVATE fathom_data)
target_compile_options(data_inputs_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_data_inputs COMMAND data_inputs_tests ${CMAKE_CURRENT_BINARY_DIR}/data-input-fixtures)
add_executable(fathom_series_from_data examples/series_from_data.cpp)
target_link_libraries(fathom_series_from_data PRIVATE fathom_data)
target_compile_options(fathom_series_from_data PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(fathom_population_from_data examples/population_from_data.cpp)
target_link_libraries(fathom_population_from_data PRIVATE fathom_data)
target_compile_options(fathom_population_from_data PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  add_test(NAME runtime_data_inputs_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/data_inputs_contract.py $<TARGET_FILE:runtime_data_probe>
    --report ${CMAKE_CURRENT_BINARY_DIR}/data-inputs-report.json)
  add_test(NAME runtime_population_init_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/population_init_contract.py $<TARGET_FILE:runtime_data_probe>
    --report ${CMAKE_CURRENT_BINARY_DIR}/population-init-report.json)
  add_test(NAME runtime_data_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_data_contract.py $<TARGET_FILE:runtime_data_probe>
    --report ${CMAKE_CURRENT_BINARY_DIR}/runtime-data-report.json)
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME runtime_data_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_data_contract.py $<TARGET_FILE:runtime_data_probe>
    --without-arrow --report ${CMAKE_CURRENT_BINARY_DIR}/runtime-data-report.json)
endif()

add_executable(ir_parameter_data_tests tests/ir_parameter_data_tests.cpp)
target_link_libraries(ir_parameter_data_tests PRIVATE fathom_ir nlohmann_json)
target_compile_options(ir_parameter_data_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_parameter_data COMMAND ir_parameter_data_tests ${CMAKE_CURRENT_BINARY_DIR}/ir-parameter-data-fixtures)
if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  execute_process(COMMAND "${_output_python}" -c "import jsonschema"
    RESULT_VARIABLE _parameter_schema_status OUTPUT_QUIET ERROR_QUIET)
  set(_parameter_schema_args "")
  if(_parameter_schema_status EQUAL 0)
    set(_parameter_schema_args --schema)
  endif()
  add_test(NAME ir_parameter_data_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_parameter_data_contract.py $<TARGET_FILE:fathom> ${_parameter_schema_args})
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME ir_parameter_data_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_parameter_data_contract.py $<TARGET_FILE:fathom> --without-arrow)
endif()

add_executable(ir_series_data_tests tests/ir_series_data_tests.cpp)
target_link_libraries(ir_series_data_tests PRIVATE fathom_ir nlohmann_json)
target_compile_options(ir_series_data_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_series_data COMMAND ir_series_data_tests ${CMAKE_CURRENT_BINARY_DIR}/ir-series-data-fixtures
  ${CMAKE_CURRENT_SOURCE_DIR}/models/data/seasonal_stock.ir.json)
if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  add_test(NAME ir_series_data_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_series_data_contract.py $<TARGET_FILE:fathom> ${_parameter_schema_args})
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME ir_series_data_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_series_data_contract.py $<TARGET_FILE:fathom> --without-arrow)
endif()

add_executable(ir_population_data_tests tests/ir_population_data_tests.cpp)
target_link_libraries(ir_population_data_tests PRIVATE fathom_ir nlohmann_json)
target_compile_options(ir_population_data_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME ir_population_data COMMAND ir_population_data_tests ${CMAKE_CURRENT_BINARY_DIR}/ir-population-data-fixtures
  ${CMAKE_CURRENT_SOURCE_DIR}/models/data/workforce.ir.json)
if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  add_test(NAME ir_population_data_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_population_data_contract.py $<TARGET_FILE:fathom> ${_parameter_schema_args})
  add_test(NAME ir_abm_data_composition COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_abm_data_composition.py $<TARGET_FILE:fathom> ${_parameter_schema_args})
  add_test(NAME ankura_pilot_reference COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ankura_pilot_contract.py $<TARGET_FILE:fathom> $<TARGET_FILE:economics_monthly_probe> ${_parameter_schema_args})
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME ir_population_data_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_population_data_contract.py $<TARGET_FILE:fathom> --without-arrow)
  add_test(NAME ir_abm_data_composition COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir_abm_data_composition.py $<TARGET_FILE:fathom> --without-arrow)
endif()

add_executable(provenance_tests tests/provenance_tests.cpp)
target_include_directories(provenance_tests PRIVATE src)
target_link_libraries(provenance_tests PRIVATE fathom_ir nlohmann_json)
target_compile_options(provenance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_provenance COMMAND provenance_tests ${CMAKE_CURRENT_BINARY_DIR}/provenance-hashes.json)
set_tests_properties(runtime_provenance PROPERTIES FIXTURES_SETUP provenance_hashes)
add_test(NAME runtime_provenance_hash_contract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/provenance_hash_contract.py ${CMAKE_CURRENT_BINARY_DIR}/provenance-hashes.json)
set_tests_properties(runtime_provenance_hash_contract PROPERTIES FIXTURES_REQUIRED provenance_hashes)
add_executable(output_provenance_tests tests/output_provenance_tests.cpp)
target_include_directories(output_provenance_tests PRIVATE src)
target_link_libraries(output_provenance_tests PRIVATE fathom_ir nlohmann_json)
target_compile_options(output_provenance_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME runtime_output_provenance COMMAND output_provenance_tests
  ${CMAKE_CURRENT_BINARY_DIR}/lineage-fixtures ${CMAKE_CURRENT_SOURCE_DIR}/models/decay.ir.json)
set_tests_properties(runtime_output_provenance PROPERTIES FIXTURES_SETUP lineage_fixtures)
if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  add_test(NAME runtime_output_provenance_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/output_provenance_contract.py ${CMAKE_CURRENT_BINARY_DIR}/lineage-fixtures)
  set_tests_properties(runtime_output_provenance_contract PROPERTIES FIXTURES_REQUIRED lineage_fixtures)
  add_test(NAME runtime_result_verification_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/result_verification_contract.py $<TARGET_FILE:fathom> ${CMAKE_CURRENT_BINARY_DIR}/lineage-fixtures)
  set_tests_properties(runtime_result_verification_contract PROPERTIES FIXTURES_REQUIRED lineage_fixtures TIMEOUT 180)
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME runtime_output_provenance_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/output_provenance_contract.py ${CMAKE_CURRENT_BINARY_DIR}/lineage-fixtures --without-arrow)
  set_tests_properties(runtime_output_provenance_contract PROPERTIES FIXTURES_REQUIRED lineage_fixtures)
  add_test(NAME runtime_result_verification_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/result_verification_contract.py $<TARGET_FILE:fathom> ${CMAKE_CURRENT_BINARY_DIR}/lineage-fixtures --without-arrow)
  set_tests_properties(runtime_result_verification_contract PROPERTIES FIXTURES_REQUIRED lineage_fixtures TIMEOUT 180)
endif()
if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  add_test(NAME runtime_provenance_cli_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/provenance_cli_contract.py $<TARGET_FILE:fathom> ${_parameter_schema_args})
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME runtime_provenance_cli_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/provenance_cli_contract.py $<TARGET_FILE:fathom> --without-arrow)
endif()

if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  add_test(NAME runtime_default_manifest_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/default_manifest_contract.py $<TARGET_FILE:fathom> ${_parameter_schema_args})
  set_tests_properties(runtime_default_manifest_contract PROPERTIES TIMEOUT 180)
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME runtime_default_manifest_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/default_manifest_contract.py $<TARGET_FILE:fathom> --without-arrow)
  set_tests_properties(runtime_default_manifest_contract PROPERTIES TIMEOUT 180)
endif()

if(FATHOM_HAS_ARROW AND _output_python_status EQUAL 0)
  add_test(NAME runtime_bundle_contract COMMAND "${_output_python}"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/bundle_contract.py $<TARGET_FILE:fathom> ${_parameter_schema_args})
  set_tests_properties(runtime_bundle_contract PROPERTIES TIMEOUT 240)
elseif(NOT FATHOM_HAS_ARROW)
  add_test(NAME runtime_bundle_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/bundle_contract.py $<TARGET_FILE:fathom> --without-arrow)
  set_tests_properties(runtime_bundle_contract PROPERTIES TIMEOUT 240)
endif()

if(FATHOM_HAS_ARROW)
  add_test(NAME runtime_c_api_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/c_api_contract.py $<TARGET_FILE:fathom> $<TARGET_FILE:c_api_probe>)
else()
  add_test(NAME runtime_c_api_contract COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/c_api_contract.py $<TARGET_FILE:fathom> $<TARGET_FILE:c_api_probe> --without-arrow)
endif()
set_tests_properties(runtime_c_api_contract PROPERTIES TIMEOUT 180)

if(FATHOM_HAS_ARROW)
  add_executable(c_api_arrow_ipc_probe tests/c_api_arrow_ipc_probe.cpp)
  target_link_libraries(c_api_arrow_ipc_probe PRIVATE fathom_c_api ${FATHOM_ARROW_LIBRARIES})
  target_compile_options(c_api_arrow_ipc_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(_output_python_status EQUAL 0)
    add_test(NAME runtime_c_api_arrow_contract COMMAND "${_output_python}"
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/c_api_arrow_contract.py
      $<TARGET_FILE:c_api_arrow_ipc_probe> $<TARGET_FILE:c_api_probe>)
    set_tests_properties(runtime_c_api_arrow_contract PROPERTIES TIMEOUT 180)
  endif()
endif()

endif() # BUILD_TESTING

include(cmake/FathomInstall.cmake)

if(FATHOM_ENABLE_FUZZING)
  add_subdirectory(tests/fuzz)
endif()

option(FATHOM_ENABLE_PYTHON "Build the optional nanobind Python interface" OFF)
if(FATHOM_ENABLE_PYTHON)
  add_subdirectory(bindings/python)
endif()

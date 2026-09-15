cmake_minimum_required(VERSION 3.25)

project(FabricTopology
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral, generation-bound topology governance runtime for the Distributed Fabric Infrastructure"
    HOMEPAGE_URL "https://github.com/pngen/Fabric-Topology"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    set(FABRIC_TOPOLOGY_IS_TOP_LEVEL ON)
else()
    set(FABRIC_TOPOLOGY_IS_TOP_LEVEL OFF)
endif()

option(FABRIC_TOPOLOGY_BUILD_TESTS "Build the Fabric Topology test suite" ${FABRIC_TOPOLOGY_IS_TOP_LEVEL})
option(FABRIC_TOPOLOGY_BUILD_TOOLS "Build the ftcli / ftcoordinator / ftpublisher executables" ${FABRIC_TOPOLOGY_IS_TOP_LEVEL})
option(FABRIC_TOPOLOGY_BUILD_EXAMPLES "Build the Fabric Topology examples" ${FABRIC_TOPOLOGY_IS_TOP_LEVEL})
option(FABRIC_TOPOLOGY_BUILD_BENCHMARKS "Build the guarded Fabric Topology benchmarks" ${FABRIC_TOPOLOGY_IS_TOP_LEVEL})
option(FABRIC_TOPOLOGY_INSTALL_TOOLS "Install the Fabric Topology executables alongside the library" ON)
option(FABRIC_TOPOLOGY_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(FABRIC_TOPOLOGY_ENABLE_ASAN "Build with AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# First-party build profile. Applied to each target directly; no interface target is
# exported, so consumers never inherit our warning or sanitizer configuration.
# ---------------------------------------------------------------------------
function(fabric_topology_apply_profile target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            /EHsc
            /MP
            /diagnostics:caret)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0A00)
        if(FABRIC_TOPOLOGY_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(FABRIC_TOPOLOGY_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
        if(FABRIC_TOPOLOGY_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(FABRIC_TOPOLOGY_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(fabric_topology STATIC
    src/ids.cpp
    src/version.cpp
    src/types.cpp
    src/result.cpp
    src/limits.cpp
    src/registry.cpp
    src/metadata.cpp
    src/digest.cpp
    src/codec.cpp
    src/graph_state.cpp
    src/topology_engine.cpp
    src/topology_mutations.cpp
    src/topology_query.cpp
    src/validation.cpp
    src/persistence.cpp
    src/snapshot.cpp
    src/diff.cpp
    src/reconciliation.cpp
    src/traversal.cpp
    src/synthetic.cpp
    src/protocol.cpp)

add_library(SummonSoftwareLabs::FabricTopology ALIAS fabric_topology)

target_include_directories(fabric_topology
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(fabric_topology PUBLIC cxx_std_20)
fabric_topology_apply_profile(fabric_topology)
set_target_properties(fabric_topology PROPERTIES
    OUTPUT_NAME "fabric_topology"
    EXPORT_NAME "FabricTopology"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Runtime library (sockets, coordinator/publisher session, host discovery)
# ---------------------------------------------------------------------------
add_library(fabric_topology_runtime STATIC
    src/runtime/socket_platform.cpp
    src/runtime/frame_transport.cpp
    src/runtime/coordinator.cpp
    src/runtime/publisher_client.cpp
    src/runtime/host_discovery.cpp
    src/runtime/registry_seed.cpp)

add_library(SummonSoftwareLabs::FabricTopologyRuntime ALIAS fabric_topology_runtime)

target_include_directories(fabric_topology_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(fabric_topology_runtime PUBLIC cxx_std_20)
target_link_libraries(fabric_topology_runtime PUBLIC fabric_topology)
fabric_topology_apply_profile(fabric_topology_runtime)

if(WIN32)
    target_link_libraries(fabric_topology_runtime PUBLIC ws2_32 iphlpapi setupapi advapi32)
endif()

set_target_properties(fabric_topology_runtime PROPERTIES
    OUTPUT_NAME "fabric_topology_runtime"
    EXPORT_NAME "FabricTopologyRuntime"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
if(FABRIC_TOPOLOGY_BUILD_TOOLS)
    add_executable(ftcli tools/ftcli/main.cpp tools/ftcli/commands.cpp)
    target_link_libraries(ftcli PRIVATE fabric_topology fabric_topology_runtime)
    fabric_topology_apply_profile(ftcli)

    add_executable(ftcoordinator tools/ftcoordinator/main.cpp)
    target_link_libraries(ftcoordinator PRIVATE fabric_topology fabric_topology_runtime)
    fabric_topology_apply_profile(ftcoordinator)

    add_executable(ftpublisher tools/ftpublisher/main.cpp)
    target_link_libraries(ftpublisher PRIVATE fabric_topology fabric_topology_runtime)
    fabric_topology_apply_profile(ftpublisher)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FABRIC_TOPOLOGY_BUILD_TESTS)
    enable_testing()

    add_library(fabric_topology_test_support STATIC tests/support/test_framework.cpp)
    target_include_directories(fabric_topology_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
    target_link_libraries(fabric_topology_test_support PUBLIC fabric_topology fabric_topology_runtime)
    fabric_topology_apply_profile(fabric_topology_test_support)

    function(fabric_topology_add_test name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE fabric_topology_test_support)
        fabric_topology_apply_profile(${name})
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
    endfunction()

    fabric_topology_add_test(ft_test_core tests/core/test_core.cpp)
    fabric_topology_add_test(ft_test_invariants tests/core/test_invariants.cpp)
    fabric_topology_add_test(ft_test_generation tests/core/test_generation.cpp)
    fabric_topology_add_test(ft_test_authority tests/core/test_authority.cpp)
    fabric_topology_add_test(ft_test_reconciliation tests/core/test_reconciliation.cpp)
    fabric_topology_add_test(ft_test_determinism tests/core/test_determinism.cpp)
    fabric_topology_add_test(ft_test_traversal tests/core/test_traversal.cpp)
    fabric_topology_add_test(ft_test_persistence tests/persistence/test_persistence.cpp)
    fabric_topology_add_test(ft_test_persistence_corruption tests/persistence/test_corruption.cpp)
    fabric_topology_add_test(ft_test_bounds tests/bounds/test_bounds.cpp)
    fabric_topology_add_test(ft_test_property tests/property/test_property.cpp)
    fabric_topology_add_test(ft_test_race tests/race/test_race.cpp)
    fabric_topology_add_test(ft_test_adversarial tests/adversarial/test_adversarial.cpp)
    fabric_topology_add_test(ft_test_protocol tests/protocol/test_protocol.cpp)
    fabric_topology_add_test(ft_test_host_discovery tests/host/test_host_discovery.cpp)
    fabric_topology_add_test(ft_test_synthetic tests/synthetic/test_synthetic.cpp)

    add_executable(ft_test_distributed tests/distributed/test_distributed.cpp)
    target_link_libraries(ft_test_distributed PRIVATE fabric_topology_test_support)
    fabric_topology_apply_profile(ft_test_distributed)
    target_compile_definitions(ft_test_distributed PRIVATE
        FT_COORDINATOR_EXE="$<TARGET_FILE:ftcoordinator>"
        FT_PUBLISHER_EXE="$<TARGET_FILE:ftpublisher>")
    add_dependencies(ft_test_distributed ftcoordinator ftpublisher)
    add_test(NAME ft_test_distributed COMMAND ft_test_distributed)
    set_tests_properties(ft_test_distributed PROPERTIES SKIP_RETURN_CODE 77)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(FABRIC_TOPOLOGY_BUILD_EXAMPLES)
    foreach(example_name IN ITEMS
            ex_basic_physical_topology
            ex_leaf_spine_synthetic
            ex_logical_and_physical
            ex_stale_generation_rejection
            ex_worker_fencing
            ex_snapshot_and_diff
            ex_device_replacement
            ex_persistence_recovery)
        add_executable(${example_name} examples/${example_name}.cpp)
        target_link_libraries(${example_name} PRIVATE fabric_topology fabric_topology_runtime)
        fabric_topology_apply_profile(${example_name})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks (guarded: never registered with CTest)
# ---------------------------------------------------------------------------
if(FABRIC_TOPOLOGY_BUILD_BENCHMARKS)
    add_executable(ft_benchmarks benchmarks/ft_benchmarks.cpp)
    target_link_libraries(ft_benchmarks PRIVATE fabric_topology fabric_topology_runtime)
    fabric_topology_apply_profile(ft_benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS fabric_topology fabric_topology_runtime
    EXPORT FabricTopologyTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(FABRIC_TOPOLOGY_BUILD_TOOLS AND FABRIC_TOPOLOGY_INSTALL_TOOLS)
    install(TARGETS ftcli ftcoordinator ftpublisher
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(FABRIC_TOPOLOGY_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/FabricTopology)

install(EXPORT FabricTopologyTargets
    FILE FabricTopologyTargets.cmake
    NAMESPACE SummonSoftwareLabs::
    DESTINATION ${FABRIC_TOPOLOGY_CMAKE_INSTALL_DIR})

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FabricTopologyConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/FabricTopologyConfig.cmake
    INSTALL_DESTINATION ${FABRIC_TOPOLOGY_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/FabricTopologyConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/FabricTopologyConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/FabricTopologyConfigVersion.cmake
    DESTINATION ${FABRIC_TOPOLOGY_CMAKE_INSTALL_DIR})

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/FabricTopology)

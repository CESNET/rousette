cmake_minimum_required(VERSION 3.12)
project(rousette LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
include(GNUInstallDirs)

# Set a default build type if none was specified. This was shamelessly stolen
# from VTK's cmake setup because these guys produce both CMake and a project that
# manipulates this variable, and the web is full of posts where people say that
# it is apparently evil to just set the build type in a way an earlier version of
# this patch did. Oh, and the location of this check/update matters, apparently.
#
# Yes, this is just plain crazy.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'RelWithDebInfo' as none was specified.")
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build." FORCE)
    # Set the possible values of build type for cmake-gui
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# -Werror is not a default for sanity reasons (one cannot know what warnings a future compiler
# might bring along), but it's a default in debug mode. The idea is that developers should care
# about a warning-free build, and that this is easier than messing with yet another configure option.
set(CMAKE_CXX_FLAGS_DEBUG "-Werror ${CMAKE_CXX_FLAGS_DEBUG}")

# I don't want to duplicate the compiler's optimizations
set(CMAKE_CXX_FLAGS "-O2 ${CMAKE_CXX_FLAGS}")

# Build warnings are useful tools (and this project should be warning-free anyway), enable them on all
# configurations. They are warnings, not errors.
set(CMAKE_CXX_FLAGS "-Wall -Wextra -pedantic -Woverloaded-virtual ${CMAKE_CXX_FLAGS}")

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "-Wsuggest-override ${CMAKE_CXX_FLAGS}")
endif()

set(ANONYMOUS_USER "yangnobody" CACHE STRING "Configures system user used for anonymous access to sysrepo")
set(ANONYMOUS_USER_GROUP "yangnobody" CACHE STRING "Configures system user group used for anonymous access to sysrepo")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")

add_custom_target(rousette-version-cmake-ide
    cmake/ProjectGitVersion.cmake
    cmake/ProjectGitVersionRunner.cmake
    )
include(cmake/ProjectGitVersion.cmake)
prepare_git_version(ROUSETTE_VERSION "0.0")

find_package(Doxygen)
option(WITH_DOCS "Create and install internal documentation (needs Doxygen)" ${DOXYGEN_FOUND})

if(WITH_DOCS)
    set(doxyfile_in ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in)
    set(doxyfile ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(${doxyfile_in} ${doxyfile} @ONLY)
    add_custom_target(doc
        COMMAND ${DOXYGEN_EXECUTABLE} ${doxyfile}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
        SOURCES ${doxyfile_in}
        )
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

find_package(spdlog REQUIRED)
find_package(PkgConfig)
pkg_check_modules(nghttp2 REQUIRED IMPORTED_TARGET libnghttp2_asio>=0.0.90 libnghttp2)
find_package(Boost REQUIRED COMPONENTS system thread)

pkg_check_modules(SYSREPO-CPP REQUIRED IMPORTED_TARGET sysrepo-cpp>=1.1.0)
pkg_check_modules(LIBYANG-CPP REQUIRED IMPORTED_TARGET libyang-cpp>=1.1.0)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(PAM REQUIRED IMPORTED_TARGET pam)

add_library(rousette-http STATIC
    src/http/EventStream.cpp
    src/http/utils.cpp
)
target_link_libraries(rousette-http PUBLIC spdlog::spdlog PkgConfig::nghttp2 ssl crypto)

add_library(rousette-sysrepo STATIC
    src/sr/AllEvents.cpp
    src/sr/OpticalEvents.cpp
)
target_link_libraries(rousette-sysrepo PUBLIC spdlog::spdlog PkgConfig::SYSREPO-CPP PkgConfig::LIBYANG-CPP)

add_library(rousette-auth STATIC
    src/restconf/Nacm.cpp
    src/restconf/PAM.cpp
)
target_link_libraries(rousette-auth PUBLIC spdlog::spdlog PkgConfig::SYSREPO-CPP PkgConfig::PAM)

add_library(rousette-restconf STATIC
    src/restconf/Server.cpp
    src/restconf/uri.cpp
    src/restconf/utils.cpp
)
target_link_libraries(rousette-restconf PUBLIC rousette-http rousette-sysrepo rousette-auth Boost::system Threads::Threads)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/restconf/NacmIdentities.h.in ${CMAKE_CURRENT_BINARY_DIR}/NacmIdentities.h @ONLY)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_executable(clock-demo src/clock.cpp)
target_link_libraries(clock-demo rousette-http Boost::system Threads::Threads)

add_executable(watch-operational-ds
    src/events.cpp
)
target_link_libraries(watch-operational-ds PUBLIC rousette-sysrepo)

add_executable(rousette src/restconf/main.cpp)
target_link_libraries(rousette PUBLIC rousette-restconf PkgConfig::SYSTEMD)

install(TARGETS
    # clock-demo
    # watch-operational-ds
    rousette
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/)
install(FILES ${CMAKE_SOURCE_DIR}/yang/ietf-restconf@2017-01-26.yang DESTINATION ${CMAKE_INSTALL_PREFIX}/share/velia/yang)

include(CTest)
if(BUILD_TESTING)
    find_package(trompeloeil 45 REQUIRED)
    find_package(doctest 2.4.11 REQUIRED)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/configure.cmake.h.in ${CMAKE_CURRENT_BINARY_DIR}/tests/configure.cmake.h)

    add_library(DoctestIntegration STATIC
        tests/doctest_integration.cpp
        tests/trompeloeil_doctest.h
        )

    target_include_directories(DoctestIntegration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/ ${CMAKE_CURRENT_SOURCE_DIR}/src/)
    target_link_libraries(DoctestIntegration doctest::doctest spdlog::spdlog)
    target_compile_definitions(DoctestIntegration PUBLIC DOCTEST_CONFIG_SUPER_FAST_ASSERTS)

    find_program(SYSREPOCTL sysrepoctl)
    find_program(SYSREPOCFG sysrepocfg)
    pkg_check_modules(pam_wrapper REQUIRED IMPORTED_TARGET pam_wrapper)
    pkg_get_variable(PAM_WRAPPER_MODULES_DIR pam_wrapper modules)
    set(TEST_PAM_WRAPPER_DB ${CMAKE_CURRENT_SOURCE_DIR}/tests/pam/users)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/pam/other.in ${CMAKE_CURRENT_BINARY_DIR}/tests/pam/other @ONLY)

    function(wrap_test_pam TEST_NAME)
        set_property(TEST test-${TEST_NAME} APPEND PROPERTY ENVIRONMENT
            "LD_PRELOAD=${pam_wrapper_LDFLAGS}"
            "PAM_WRAPPER_SERVICE_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/pam"
            "PAM_WRAPPER=1"
            "UID_WRAPPER_DISABLE_DEEPBIND=1"
            )
    endfunction()

    function(rousette_test)
        cmake_parse_arguments(TEST "" "NAME;FIXTURE;RESOURCE_LOCK;WRAP_PAM" "LIBRARIES;COMMAND" ${ARGN})

        add_executable(test-${TEST_NAME} ${CMAKE_SOURCE_DIR}/tests/${TEST_NAME}.cpp)
        target_link_libraries(test-${TEST_NAME} DoctestIntegration ${TEST_LIBRARIES})
        target_include_directories(test-${TEST_NAME}
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}
            PRIVATE
                ${CMAKE_BINARY_DIR}
        )

        if(NOT CMAKE_CROSSCOMPILING)
            if(NOT(TEST_COMMAND))
                add_test(test-${TEST_NAME} test-${TEST_NAME})
            else()
                add_test(NAME test-${TEST_NAME} COMMAND ${TEST_COMMAND})
            endif()
        endif()

        set(test_name_init sysrepo:prep:${TEST_NAME})
        set(test_name_cleanup sysrepo:clean:${TEST_NAME})
        set(fixture_name sysrepo:env:${TEST_NAME})

        if(TEST_RESOURCE_LOCK)
            set_tests_properties(test-${TEST_NAME} PROPERTIES RESOURCE_LOCK "${TEST_RESOURCE_LOCK}")
        endif()

        # How to do this:
        # take the args for manage module from variable "fixture{fixture_name}"
        # and then add set env vars for sysrepo
        # set them ALSO for the init/cleanup tests
        if(TEST_FIXTURE)
            add_test(NAME ${test_name_init} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/sysrepoctl-manage-module.sh ${SYSREPOCTL} ${SYSREPOCFG} prepare ${${TEST_FIXTURE}})
            set_tests_properties(${test_name_init} PROPERTIES FIXTURES_SETUP ${fixture_name})
            add_test(NAME ${test_name_cleanup} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/sysrepoctl-manage-module.sh ${SYSREPOCTL} ${SYSREPOCFG} uninstall ${${TEST_FIXTURE}})
            set_tests_properties(${test_name_cleanup} PROPERTIES FIXTURES_CLEANUP ${fixture_name})

            set_tests_properties(test-${TEST_NAME} PROPERTIES FIXTURES_REQUIRED ${fixture_name})
            set_property(TEST test-${TEST_NAME} ${test_name_init} ${test_name_cleanup} APPEND PROPERTY ENVIRONMENT
              "SYSREPO_REPOSITORY_PATH=${CMAKE_CURRENT_BINARY_DIR}/test_repositories/test_${TEST_NAME}"
              "SYSREPO_SHM_PREFIX=rousette_test_${TEST_NAME}"
            )
        endif()

	if(TEST_WRAP_PAM)
            wrap_test_pam(${TEST_NAME})
	endif()
    endfunction()

    rousette_test(NAME http-utils LIBRARIES rousette-http)
    rousette_test(NAME uri-parser LIBRARIES rousette-restconf)
    rousette_test(NAME pam LIBRARIES rousette-auth)
    wrap_test_pam(pam)

    set(common-models
        YANG ${CMAKE_CURRENT_SOURCE_DIR}/tests/yang/ietf-system@2014-08-06.yang FEATURE radius
        YANG ${CMAKE_CURRENT_SOURCE_DIR}/yang/ietf-restconf@2017-01-26.yang
        YANG ${CMAKE_CURRENT_SOURCE_DIR}/tests/yang/example.yang FEATURE f1)
    rousette_test(NAME http-communication LIBRARIES rousette-restconf FIXTURE common-models WRAP_PAM ON)
endif()

project(rousette LANGUAGES CXX)
cmake_minimum_required(VERSION 3.12)
set(CMAKE_CXX_STANDARD 17)
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
pkg_check_modules(nghttp2 REQUIRED IMPORTED_TARGET libnghttp2_asio>=1.41.0 libnghttp2)
find_package(Boost REQUIRED COMPONENTS system thread)

pkg_check_modules(SYSREPO REQUIRED sysrepo-cpp>=1.4.103 IMPORTED_TARGET sysrepo)
pkg_check_modules(LIBYANG REQUIRED libyang-cpp>=1.0.215 IMPORTED_TARGET libyang)

add_library(rousette-http STATIC
    src/http/EventStream.cpp
    src/http/utils.cpp
)
target_link_libraries(rousette-http PRIVATE spdlog::spdlog PkgConfig::nghttp2 ssl crypto)

add_library(rousette-sysrepo STATIC
    src/sr/AllEvents.cpp
    src/sr/OpticalEvents.cpp
)
target_link_libraries(rousette-sysrepo PRIVATE spdlog::spdlog PkgConfig::SYSREPO PkgConfig::LIBYANG)

add_library(rousette-restconf STATIC
    src/restconf/Server.cpp
)
target_link_libraries(rousette-restconf PRIVATE rousette-http spdlog::spdlog PkgConfig::SYSREPO PkgConfig::nghttp2 Boost::system Threads::Threads)

add_executable(clock-demo src/clock.cpp)
target_link_libraries(clock-demo rousette-http spdlog::spdlog PkgConfig::nghttp2 Boost::system Threads::Threads)

add_executable(watch-operational-ds
    src/events.cpp
)
target_link_libraries(watch-operational-ds PRIVATE rousette-sysrepo PkgConfig::SYSREPO spdlog::spdlog)

add_executable(rousette src/restconf/main.cpp)
target_link_libraries(rousette PRIVATE rousette-restconf rousette-sysrepo spdlog::spdlog PkgConfig::SYSREPO)

install(TARGETS
    # clock-demo
    # watch-operational-ds
    rousette
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/)

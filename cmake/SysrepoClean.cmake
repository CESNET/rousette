foreach(var THIS_BINARY_DIR TEST_NAME SYSREPO_SHM_PREFIX)
    if(NOT ${var})
        message(FATAL_ERROR "${var} not specified")
    endif()
endforeach()

set(shm_files_pattern "/dev/shm/${SYSREPO_SHM_PREFIX}_*")

file(GLOB shm_files ${shm_files_pattern})

message(STATUS "Removing ${shm_files_pattern}")
file(REMOVE ${shm_files})
message(STATUS "Removing ${THIS_BINARY_DIR}/test_repositories/test_${TEST_NAME}")
file(REMOVE_RECURSE "${CMAKE_CURRENT_BINARY_DIR}/test_repositories/test_${TEST_NAME}")

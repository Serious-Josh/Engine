# Third-party notices and app-local MSVC redistributables for EngineRuntime. Build Game copies both from the
# runtime directory into every packaged game, so they are produced beside EngineRuntime.exe itself.
FetchContent_GetProperties(box3d)

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)

if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    message(FATAL_ERROR "Cannot locate Release MSVC redistributable libraries; install the MSVC Redist build components.")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateRuntimeNotices.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/GenerateRuntimeNotices.cmake" @ONLY)

add_custom_command(TARGET EngineRuntime POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DRUNTIME_DIR=$<TARGET_FILE_DIR:EngineRuntime>" "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_BINARY_DIR}/GenerateRuntimeNotices.cmake"
    COMMENT "Writing EngineRuntime third-party notices (and Release MSVC redistributables)"
    VERBATIM
)

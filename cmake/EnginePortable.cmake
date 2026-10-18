# The portable target assembles existing Release products; normal deployment stays unchanged.
set(ENGINE_PORTABLE_DXC_DIR "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64"
    CACHE PATH "Windows SDK redistributable x64 DXC directory")
set(ENGINE_PORTABLE_SDK_LICENSE_DIR
    "C:/Program Files (x86)/Windows Kits/10/Licenses/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    CACHE PATH "License directory for the installed Windows SDK")

set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)
FetchContent_GetProperties(box3d)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssembleEnginePortable.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AssembleEnginePortable.configured.cmake" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/AssembleEnginePortable-$<CONFIG>.cmake"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/AssembleEnginePortable.configured.cmake")

add_custom_target(EnginePortable
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/AssembleEnginePortable-$<CONFIG>.cmake"
    DEPENDS Engine EngineRuntime
    COMMENT "Assembling Release portable Editor (diagnostics excluded)"
    VERBATIM
)

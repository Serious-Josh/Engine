# Microsoft DirectX 12 Agility SDK - pinned D3D12 runtime and headers.
# The runtime is deployed beside each D3D12 executable in D3D12/; D3D12SDKPath names that folder.
set(ENGINE_AGILITY_SDK_VERSION 619)
set(ENGINE_AGILITY_SDK_PACKAGE_VERSION 1.619.6)

FetchContent_Declare(
    agility_sdk
    URL https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${ENGINE_AGILITY_SDK_PACKAGE_VERSION}/microsoft.direct3d.d3d12.${ENGINE_AGILITY_SDK_PACKAGE_VERSION}.nupkg
    URL_HASH SHA256=08f0489281401aa430fc37322d6c3fc98a8025175aacd714c10d562f4963f1e9
    DOWNLOAD_NAME microsoft.direct3d.d3d12.${ENGINE_AGILITY_SDK_PACKAGE_VERSION}.zip
)

FetchContent_MakeAvailable(agility_sdk)

set(ENGINE_AGILITY_SDK_INCLUDE_DIR "${agility_sdk_SOURCE_DIR}/build/native/include")
set(ENGINE_AGILITY_SDK_BIN_DIR "${agility_sdk_SOURCE_DIR}/build/native/bin/x64")
set(ENGINE_AGILITY_SDK_LICENSE "${agility_sdk_SOURCE_DIR}/LICENSE.txt")

file(STRINGS "${ENGINE_AGILITY_SDK_INCLUDE_DIR}/d3d12.h" agility_header_version
    REGEX "^#define[ \t]+D3D12_SDK_VERSION[ \t]+\\([ \t]*[0-9]+[ \t]*\\)")
string(REGEX REPLACE ".*\\([ \t]*([0-9]+)[ \t]*\\).*" "\\1" agility_header_version "${agility_header_version}")

if(NOT agility_header_version STREQUAL ENGINE_AGILITY_SDK_VERSION)
    message(FATAL_ERROR "Agility SDK headers declare D3D12_SDK_VERSION '${agility_header_version}', "
        "expected ${ENGINE_AGILITY_SDK_VERSION} (package ${ENGINE_AGILITY_SDK_PACKAGE_VERSION}).")
endif()

# Exports D3D12SDKVersion/D3D12SDKPath from the executable and deploys the redistributable runtime.
# Debug adds the matching SDK layers for the debug layer and DRED; other configurations ship D3D12Core only.
function(engine_use_agility target)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/Platform/Windows/AgilitySDKExports.cpp")
    target_compile_definitions(${target} PRIVATE ENGINE_AGILITY_SDK_VERSION=${ENGINE_AGILITY_SDK_VERSION})

    set(runtime_files "${ENGINE_AGILITY_SDK_BIN_DIR}/D3D12Core.dll"
        "$<$<CONFIG:Debug>:${ENGINE_AGILITY_SDK_BIN_DIR}/d3d12SDKLayers.dll>")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime_files}" "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND_EXPAND_LISTS
        COMMENT "Deploying Agility SDK ${ENGINE_AGILITY_SDK_PACKAGE_VERSION} runtime for ${target}"
        VERBATIM
    )
endfunction()

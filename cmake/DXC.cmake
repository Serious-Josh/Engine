# Microsoft DirectX Shader Compiler - pinned compiler, headers and import library.
# Every Engine target compiles and links against this release, and every deployment copies its DLLs; the
# Windows SDK copy is never used. Debug and Release share the one pinned package.
set(ENGINE_DXC_VERSION 1.9.2602.24)
# The identity the loaded dxcompiler.dll must report at runtime (IDxcVersionInfo/2/3); see ShaderCompiler.
set(ENGINE_DXC_VERSION_MAJOR 1)
set(ENGINE_DXC_VERSION_MINOR 9)
set(ENGINE_DXC_COMMIT_COUNT 5191)
set(ENGINE_DXC_COMMIT_HASH d355aa83)

FetchContent_Declare(
    dxc
    URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${ENGINE_DXC_VERSION}/dxc_2026_05_27.zip
    URL_HASH SHA256=cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4
)

FetchContent_MakeAvailable(dxc)

set(ENGINE_DXC_INCLUDE_DIR "${dxc_SOURCE_DIR}/inc")
set(ENGINE_DXC_BIN_DIR "${dxc_SOURCE_DIR}/bin/x64")
set(ENGINE_DXC_IMPORT_LIBRARY "${dxc_SOURCE_DIR}/lib/x64/dxcompiler.lib")

# The redistributed binaries and their notices. The release notes' license table and the matching NuGet
# package name LICENSE-LLVM.txt for the binaries; LICENSE-MS.txt also ships in the package, so all three
# texts accompany every deployment.
set(ENGINE_DXC_RUNTIME_FILES "${ENGINE_DXC_BIN_DIR}/dxcompiler.dll" "${ENGINE_DXC_BIN_DIR}/dxil.dll")
set(ENGINE_DXC_LICENSE_NAMES LICENSE-MS.txt LICENSE-MIT.txt LICENSE-LLVM.txt)

# Deployed license copies carry a DXC- prefix so they cannot collide with other components' notices.
set(ENGINE_DXC_LICENSE_FILES)
set(ENGINE_DXC_DEPLOYED_LICENSE_NAMES)

foreach(license_name IN LISTS ENGINE_DXC_LICENSE_NAMES)
    list(APPEND ENGINE_DXC_LICENSE_FILES "${dxc_SOURCE_DIR}/${license_name}")
    list(APPEND ENGINE_DXC_DEPLOYED_LICENSE_NAMES "DXC-${license_name}")
endforeach()

# The archive hash pins the package; the per-file hashes also reject a replaced or corrupted file in an
# existing download, so the DLLs are identified by content rather than by name.
set(dxc_expected_hashes
    "dxcompiler.dll" 9b5e10ed756c461b4ec2c83a99f1d6ace20e97826e9c0b0e966b7b1cd6f2aec6
    "dxil.dll" cbcfe883a09fd0ca1f98abdf3a9553b560895e3283a136da82a8381253a169df
)

while(dxc_expected_hashes)
    list(POP_FRONT dxc_expected_hashes dxc_file_name dxc_expected_hash)
    set(dxc_file "${ENGINE_DXC_BIN_DIR}/${dxc_file_name}")

    if(NOT EXISTS "${dxc_file}")
        message(FATAL_ERROR "DXC ${ENGINE_DXC_VERSION} package is missing ${dxc_file}.")
    endif()

    file(SHA256 "${dxc_file}" dxc_actual_hash)

    if(NOT dxc_actual_hash STREQUAL dxc_expected_hash)
        message(FATAL_ERROR "${dxc_file} has SHA-256 ${dxc_actual_hash}; "
            "expected ${dxc_expected_hash} (DXC ${ENGINE_DXC_VERSION}). Delete the dxc download and reconfigure.")
    endif()
endwhile()

foreach(dxc_file IN ITEMS "${ENGINE_DXC_INCLUDE_DIR}/dxcapi.h" "${ENGINE_DXC_IMPORT_LIBRARY}"
    ${ENGINE_DXC_LICENSE_FILES})
    if(NOT EXISTS "${dxc_file}")
        message(FATAL_ERROR "DXC ${ENGINE_DXC_VERSION} package is missing ${dxc_file}.")
    endif()
endforeach()

# Headers take precedence over the Windows SDK's dxcapi.h, and the import library replaces its dxcompiler.lib.
add_library(EngineDXC INTERFACE IMPORTED)
target_include_directories(EngineDXC BEFORE INTERFACE "${ENGINE_DXC_INCLUDE_DIR}")
target_link_libraries(EngineDXC INTERFACE "${ENGINE_DXC_IMPORT_LIBRARY}")
target_compile_definitions(EngineDXC INTERFACE
    ENGINE_DXC_VERSION="${ENGINE_DXC_VERSION}"
    ENGINE_DXC_VERSION_MAJOR=${ENGINE_DXC_VERSION_MAJOR}
    ENGINE_DXC_VERSION_MINOR=${ENGINE_DXC_VERSION_MINOR}
    ENGINE_DXC_COMMIT_COUNT=${ENGINE_DXC_COMMIT_COUNT}
    ENGINE_DXC_COMMIT_HASH="${ENGINE_DXC_COMMIT_HASH}"
)

# Copies the matching compiler DLLs and the DXC license texts beside a target's executable.
function(engine_deploy_dxc target)
    set(deploy_commands)

    foreach(license_file deployed_name IN ZIP_LISTS ENGINE_DXC_LICENSE_FILES ENGINE_DXC_DEPLOYED_LICENSE_NAMES)
        list(APPEND deploy_commands COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${license_file}" "$<TARGET_FILE_DIR:${target}>/${deployed_name}")
    endforeach()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ENGINE_DXC_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
        ${deploy_commands}
        COMMENT "Deploying DXC ${ENGINE_DXC_VERSION} compiler for ${target}"
        VERBATIM
    )
endfunction()

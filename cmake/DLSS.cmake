# NVIDIA DLSS Super Resolution (UPSCALING.3A), optional.
#
# AUTO enables the optional backend when the complete SDK is installed; OFF disables it;
# ON requires it. Existing cached OFF values stay OFF (migrate once with -DENGINE_ENABLE_DLSS=AUTO).
set(ENGINE_ENABLE_DLSS "AUTO" CACHE STRING "DLSS build policy: AUTO, ON or OFF")
set_property(CACHE ENGINE_ENABLE_DLSS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${ENGINE_ENABLE_DLSS}" ENGINE_DLSS_POLICY)
# Accept legacy BOOL cache values without treating AUTO as a CMake boolean.
if(ENGINE_DLSS_POLICY STREQUAL "TRUE" OR ENGINE_DLSS_POLICY STREQUAL "1")
    set(ENGINE_DLSS_POLICY ON)
elseif(ENGINE_DLSS_POLICY STREQUAL "FALSE" OR ENGINE_DLSS_POLICY STREQUAL "0")
    set(ENGINE_DLSS_POLICY OFF)
endif()
if(NOT ENGINE_DLSS_POLICY MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ENGINE_ENABLE_DLSS must be AUTO, ON or OFF")
endif()
set(ENGINE_DLSS_AVAILABLE OFF)

set(ENGINE_DLSS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../DLSS"
    CACHE PATH "Path to the NVIDIA DLSS SDK (a checkout of github.com/NVIDIA/DLSS)"
)

# Engine's NGX project ID (NVSDK_NGX_D3D12_Init_with_ProjectID, engine type CUSTOM). Generated once on 2026-10-24 for
# this project; it identifies Engine to the NVIDIA driver, so it must stay the same. This file is its record; the cache
# entry below only defaults to it.
set(ENGINE_DLSS_PROJECT_ID_DEFAULT "668b21b6-913b-49b6-bb73-a9a5d90e255f")
set(ENGINE_DLSS_PROJECT_ID "${ENGINE_DLSS_PROJECT_ID_DEFAULT}"
    CACHE STRING "GUID-like NGX project ID passed to NVSDK_NGX_D3D12_Init_with_ProjectID"
)

set(ENGINE_DLSS_INCLUDE_DIR "${ENGINE_DLSS_ROOT}/include")
set(ENGINE_DLSS_LIBRARY_DIR "${ENGINE_DLSS_ROOT}/lib/Windows_x86_64/x64")
# The production (non-watermarked) runtime. The development DLL in lib/Windows_x86_64/dev must never be deployed.
set(ENGINE_DLSS_RUNTIME "${ENGINE_DLSS_ROOT}/lib/Windows_x86_64/rel/nvngx_dlss.dll")

if(NOT ENGINE_DLSS_POLICY STREQUAL "OFF")
    # Engine builds with the dynamic CRT (/MD, /MDd in Debug) and the default iterator debug level.
    set(ENGINE_DLSS_REQUIRED_FILES
        "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx.h"
        "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers.h"
        "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_defs.h"
        "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_params.h"
        "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_d3d.h"
        "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_cuda.h"
        "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_loader.h"
        "${ENGINE_DLSS_LIBRARY_DIR}/nvsdk_ngx_d.lib"
        "${ENGINE_DLSS_LIBRARY_DIR}/nvsdk_ngx_d_dbg.lib"
        "${ENGINE_DLSS_RUNTIME}"
    )

    set(ENGINE_DLSS_MISSING "")
    foreach(required IN LISTS ENGINE_DLSS_REQUIRED_FILES)
        if(NOT EXISTS "${required}")
            list(APPEND ENGINE_DLSS_MISSING "${required}")
        endif()
    endforeach()
    if(ENGINE_DLSS_MISSING)
        if(ENGINE_DLSS_POLICY STREQUAL "ON")
            message(FATAL_ERROR "DLSS explicitly required, missing dependencies: ${ENGINE_DLSS_MISSING}. Set ENGINE_DLSS_ROOT or use AUTO/OFF.")
        endif()
        message(STATUS "DLSS AUTO: backend disabled; missing dependencies: ${ENGINE_DLSS_MISSING}")
    else()
        set(ENGINE_DLSS_AVAILABLE ON)
    endif()
endif()

if(ENGINE_DLSS_AVAILABLE)
    # 8-4-4-4-12 hexadecimal digits (CMake regular expressions have no {n} counts).
    string(REPEAT "[0-9a-fA-F]" 4 hex4)
    string(REPEAT "[0-9a-fA-F]" 8 hex8)
    string(REPEAT "[0-9a-fA-F]" 12 hex12)

    if(NOT ENGINE_DLSS_PROJECT_ID MATCHES "^${hex8}-${hex4}-${hex4}-${hex4}-${hex12}$")
        message(FATAL_ERROR "ENGINE_DLSS_PROJECT_ID '${ENGINE_DLSS_PROJECT_ID}' is not GUID-like.")
    endif()

    message(STATUS "DLSS Super Resolution backend enabled (SDK: ${ENGINE_DLSS_ROOT})")
endif()

# Compiles the backend against the SDK when enabled. The NGX library reaches every executable that links the object
# library through its link interface.
function(engine_configure_dlss target)
    if(NOT ENGINE_DLSS_AVAILABLE)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        ENGINE_HAS_DLSS=1
        ENGINE_DLSS_PROJECT_ID="${ENGINE_DLSS_PROJECT_ID}"
        ENGINE_DLSS_ENGINE_VERSION="${PROJECT_VERSION}"
    )
    target_include_directories(${target} PRIVATE "${ENGINE_DLSS_INCLUDE_DIR}")
    target_link_libraries(${target} INTERFACE
        "$<$<CONFIG:Debug>:${ENGINE_DLSS_LIBRARY_DIR}/nvsdk_ngx_d_dbg.lib>"
        "$<$<NOT:$<CONFIG:Debug>>:${ENGINE_DLSS_LIBRARY_DIR}/nvsdk_ngx_d.lib>"
    )
endfunction()

# Copies the production DLSS runtime next to an executable that initializes DLSS. NGX loads it from the executable's
# directory; without it the backend reports the runtime as missing.
function(engine_deploy_dlss target)
    if(NOT ENGINE_DLSS_AVAILABLE)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ENGINE_DLSS_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying NVIDIA DLSS Super Resolution runtime for ${target}"
        VERBATIM
    )
endfunction()

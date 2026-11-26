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

# RR.1A: DLSS Ray Reconstruction (NGX feature "DLSS-D"), detected separately from Super Resolution in the same SDK checkout.
# It needs the Super Resolution backend (the same NGX instance). AUTO enables it when its headers and production runtime are
# present; OFF disables it; ON requires it. Missing RR files never disable Super Resolution. The runtime is deployed beside
# the Editor only: NVIDIA's RR guide (4.2) and the SDK license disagree about redistributing it, so packaged games never
# receive it (Documentation/Rendering/DLSS_Ray_Reconstruction_RR0_Investigation.md §4.4).
set(ENGINE_ENABLE_DLSS_RR "AUTO" CACHE STRING "DLSS Ray Reconstruction build policy: AUTO, ON or OFF")
set_property(CACHE ENGINE_ENABLE_DLSS_RR PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${ENGINE_ENABLE_DLSS_RR}" ENGINE_DLSS_RR_POLICY)
if(NOT ENGINE_DLSS_RR_POLICY MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ENGINE_ENABLE_DLSS_RR must be AUTO, ON or OFF")
endif()
set(ENGINE_DLSS_RR_AVAILABLE OFF)
# The production runtime. The development DLL in lib/Windows_x86_64/dev (debug overlay) must never be deployed.
set(ENGINE_DLSS_RR_RUNTIME "${ENGINE_DLSS_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")

if(NOT ENGINE_DLSS_RR_POLICY STREQUAL "OFF")
    if(NOT ENGINE_DLSS_AVAILABLE)
        if(ENGINE_DLSS_RR_POLICY STREQUAL "ON")
            message(FATAL_ERROR "DLSS Ray Reconstruction explicitly required, but the DLSS Super Resolution backend it builds "
                "on is disabled. Set ENGINE_ENABLE_DLSS to AUTO/ON with a complete SDK, or use ENGINE_ENABLE_DLSS_RR AUTO/OFF.")
        endif()
        message(STATUS "DLSS Ray Reconstruction AUTO: disabled (the DLSS Super Resolution backend is disabled)")
    else()
        set(ENGINE_DLSS_RR_REQUIRED_FILES
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_defs_dlssd.h"
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_params_dlssd.h"
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssd.h"
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssd_d3d.h"
            "${ENGINE_DLSS_RR_RUNTIME}"
        )

        set(ENGINE_DLSS_RR_MISSING "")
        foreach(required IN LISTS ENGINE_DLSS_RR_REQUIRED_FILES)
            if(NOT EXISTS "${required}")
                list(APPEND ENGINE_DLSS_RR_MISSING "${required}")
            endif()
        endforeach()

        if(ENGINE_DLSS_RR_MISSING)
            if(ENGINE_DLSS_RR_POLICY STREQUAL "ON")
                message(FATAL_ERROR "DLSS Ray Reconstruction explicitly required, missing dependencies: "
                    "${ENGINE_DLSS_RR_MISSING}. Set ENGINE_DLSS_ROOT or use ENGINE_ENABLE_DLSS_RR AUTO/OFF.")
            endif()
            message(STATUS "DLSS Ray Reconstruction AUTO: disabled; missing dependencies: ${ENGINE_DLSS_RR_MISSING}")
        else()
            set(ENGINE_DLSS_RR_AVAILABLE ON)
            message(STATUS "DLSS Ray Reconstruction support enabled (Editor runtime: ${ENGINE_DLSS_RR_RUNTIME})")
        endif()
    endif()
endif()

# FG.1: DLSS Frame Generation (NGX feature "DLSS-G", 2x), detected separately in the same SDK checkout. Like Ray
# Reconstruction it needs the Super Resolution backend (the same NGX instance); AUTO enables it when its headers and production
# runtime are present. The runtime is deployed beside the Editor only for now: packaged games get Frame Generation in a later
# stage (Documentation/Rendering/DLSS_Frame_Generation_FG0_Investigation.md §8), although its guide allows shipping it.
set(ENGINE_ENABLE_DLSS_FG "AUTO" CACHE STRING "DLSS Frame Generation build policy: AUTO, ON or OFF")
set_property(CACHE ENGINE_ENABLE_DLSS_FG PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${ENGINE_ENABLE_DLSS_FG}" ENGINE_DLSS_FG_POLICY)

if(NOT ENGINE_DLSS_FG_POLICY MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ENGINE_ENABLE_DLSS_FG must be AUTO, ON or OFF")
endif()

set(ENGINE_DLSS_FG_AVAILABLE OFF)
# The production runtime. The development DLL in lib/Windows_x86_64/dev (watermarked, debug overlays) must never be deployed.
set(ENGINE_DLSS_FG_RUNTIME "${ENGINE_DLSS_ROOT}/lib/Windows_x86_64/rel/nvngx_dlssg.dll")

if(NOT ENGINE_DLSS_FG_POLICY STREQUAL "OFF")
    if(NOT ENGINE_DLSS_AVAILABLE)
        if(ENGINE_DLSS_FG_POLICY STREQUAL "ON")
            message(FATAL_ERROR "DLSS Frame Generation explicitly required, but the DLSS Super Resolution backend it builds "
                "on is disabled. Set ENGINE_ENABLE_DLSS to AUTO/ON with a complete SDK, or use ENGINE_ENABLE_DLSS_FG AUTO/OFF.")
        endif()

        message(STATUS "DLSS Frame Generation AUTO: disabled (the DLSS Super Resolution backend is disabled)")
    else()
        set(ENGINE_DLSS_FG_REQUIRED_FILES
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_defs_dlssg.h"
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_params_dlssg.h"
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssg.h"
            "${ENGINE_DLSS_INCLUDE_DIR}/nvsdk_ngx_helpers_dlssg_d3d.h"
            "${ENGINE_DLSS_FG_RUNTIME}"
        )

        set(ENGINE_DLSS_FG_MISSING "")

        foreach(required IN LISTS ENGINE_DLSS_FG_REQUIRED_FILES)
            if(NOT EXISTS "${required}")
                list(APPEND ENGINE_DLSS_FG_MISSING "${required}")
            endif()
        endforeach()

        if(ENGINE_DLSS_FG_MISSING)
            if(ENGINE_DLSS_FG_POLICY STREQUAL "ON")
                message(FATAL_ERROR "DLSS Frame Generation explicitly required, missing dependencies: "
                    "${ENGINE_DLSS_FG_MISSING}. Set ENGINE_DLSS_ROOT or use ENGINE_ENABLE_DLSS_FG AUTO/OFF.")
            endif()

            message(STATUS "DLSS Frame Generation AUTO: disabled; missing dependencies: ${ENGINE_DLSS_FG_MISSING}")
        else()
            set(ENGINE_DLSS_FG_AVAILABLE ON)
            message(STATUS "DLSS Frame Generation support enabled (Editor runtime: ${ENGINE_DLSS_FG_RUNTIME})")
        endif()
    endif()
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
    if(ENGINE_DLSS_RR_AVAILABLE)
        target_compile_definitions(${target} PRIVATE ENGINE_HAS_DLSS_RR=1)
    endif()

    if(ENGINE_DLSS_FG_AVAILABLE)
        target_compile_definitions(${target} PRIVATE ENGINE_HAS_DLSS_FG=1)
    endif()
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

# RR.1A: copies the production DLSS Ray Reconstruction runtime next to an executable that initializes DLSS: the Editor and
# (cooked-game port) EngineRuntime, whose directory Build Game deploys games from.
function(engine_deploy_dlss_rr target)
    if(NOT ENGINE_DLSS_RR_AVAILABLE)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ENGINE_DLSS_RR_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying NVIDIA DLSS Ray Reconstruction runtime for ${target}"
        VERBATIM
    )
endfunction()

# FG.1: copies the production DLSS Frame Generation runtime next to an executable that initializes DLSS: the Editor and
# (cooked-game port) EngineRuntime.
function(engine_deploy_dlss_fg target)
    if(NOT ENGINE_DLSS_FG_AVAILABLE)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ENGINE_DLSS_FG_RUNTIME}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying NVIDIA DLSS Frame Generation runtime for ${target}"
        VERBATIM
    )
endfunction()

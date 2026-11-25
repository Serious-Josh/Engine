# NVIDIA Reflex Low Latency through the public NVAPI SDK (github.com/NVIDIA/nvapi, R620), optional.
#
# AUTO enables Reflex when the SDK's header and 64-bit static library are present; OFF disables it; ON requires it.
# Without it Engine builds as before and reports Reflex as not compiled. NVAPI's implementation is part of the NVIDIA
# driver (nvapi64.dll in System32): nothing is deployed beside the executables, and the static library is MIT-licensed,
# so its notice accompanies packaged games (GenerateRuntimeNotices.cmake.in).
set(ENGINE_ENABLE_NVAPI "AUTO" CACHE STRING "NVAPI (NVIDIA Reflex) build policy: AUTO, ON or OFF")
set_property(CACHE ENGINE_ENABLE_NVAPI PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${ENGINE_ENABLE_NVAPI}" ENGINE_NVAPI_POLICY)

if(NOT ENGINE_NVAPI_POLICY MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ENGINE_ENABLE_NVAPI must be AUTO, ON or OFF")
endif()

set(ENGINE_NVAPI_AVAILABLE OFF)

set(ENGINE_NVAPI_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../nvapi"
    CACHE PATH "Path to the NVIDIA NVAPI SDK (a checkout of github.com/NVIDIA/nvapi)"
)

set(ENGINE_NVAPI_LIBRARY "${ENGINE_NVAPI_ROOT}/amd64/nvapi64.lib")
set(ENGINE_NVAPI_LICENSE "${ENGINE_NVAPI_ROOT}/License.txt")

if(NOT ENGINE_NVAPI_POLICY STREQUAL "OFF")
    # nvapi.h includes the nvapi_lite_* headers beside it.
    set(ENGINE_NVAPI_REQUIRED_FILES
        "${ENGINE_NVAPI_ROOT}/nvapi.h"
        "${ENGINE_NVAPI_ROOT}/nvapi_lite_common.h"
        "${ENGINE_NVAPI_ROOT}/nvapi_lite_d3dext.h"
        "${ENGINE_NVAPI_ROOT}/nvapi_lite_salstart.h"
        "${ENGINE_NVAPI_ROOT}/nvapi_lite_salend.h"
        "${ENGINE_NVAPI_ROOT}/nvapi_lite_sli.h"
        "${ENGINE_NVAPI_ROOT}/nvapi_lite_stereo.h"
        "${ENGINE_NVAPI_ROOT}/nvapi_lite_surround.h"
        "${ENGINE_NVAPI_LIBRARY}"
        "${ENGINE_NVAPI_LICENSE}"
    )

    set(ENGINE_NVAPI_MISSING "")

    foreach(required IN LISTS ENGINE_NVAPI_REQUIRED_FILES)
        if(NOT EXISTS "${required}")
            list(APPEND ENGINE_NVAPI_MISSING "${required}")
        endif()
    endforeach()

    if(ENGINE_NVAPI_MISSING)
        if(ENGINE_NVAPI_POLICY STREQUAL "ON")
            message(FATAL_ERROR "NVAPI explicitly required, missing dependencies: ${ENGINE_NVAPI_MISSING}. "
                "Set ENGINE_NVAPI_ROOT or use AUTO/OFF.")
        endif()

        message(STATUS "NVAPI AUTO: NVIDIA Reflex disabled; missing dependencies: ${ENGINE_NVAPI_MISSING}")
    else()
        set(ENGINE_NVAPI_AVAILABLE ON)
        message(STATUS "NVIDIA Reflex enabled (NVAPI SDK: ${ENGINE_NVAPI_ROOT})")
    endif()
endif()

# Compiles ReflexLatency against the SDK when enabled. The static library reaches every executable that links the object
# library through its link interface, as the DLSS library does.
function(engine_configure_nvapi target)
    if(NOT ENGINE_NVAPI_AVAILABLE)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE ENGINE_HAS_NVAPI=1)
    target_include_directories(${target} PRIVATE "${ENGINE_NVAPI_ROOT}")
    target_link_libraries(${target} INTERFACE "${ENGINE_NVAPI_LIBRARY}")
endfunction()

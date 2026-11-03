# FMOD Engine (Studio + Core API) audio backend (Audio.1A), optional.
#
# AUTO enables the backend when the complete SDK is installed; OFF builds Engine without FMOD (audio reports
# itself disabled); ON requires the SDK and fails configuration when any required file is missing.
set(ENGINE_ENABLE_FMOD "AUTO" CACHE STRING "FMOD audio build policy: AUTO, ON or OFF")
set_property(CACHE ENGINE_ENABLE_FMOD PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${ENGINE_ENABLE_FMOD}" ENGINE_FMOD_POLICY)

if(NOT ENGINE_FMOD_POLICY MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ENGINE_ENABLE_FMOD must be AUTO, ON or OFF")
endif()

set(ENGINE_FMOD_AVAILABLE OFF)

# The FMOD Engine installer's default location. The SDK stays outside the Engine repository under Firelight's EULA.
set(ENGINE_FMOD_ROOT
    "C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows"
    CACHE PATH "Path to the FMOD Engine SDK for Windows (contains api/core and api/studio)"
)

# Banks built by FMOD Studio load only in a runtime of the same product and major version (2.04.xx), so the
# SDK version is pinned here together with the FMOD Studio version the project uses.
set(ENGINE_FMOD_REQUIRED_VERSION_PREFIX "0x000204")

set(ENGINE_FMOD_CORE_INCLUDE_DIR "${ENGINE_FMOD_ROOT}/api/core/inc")
set(ENGINE_FMOD_STUDIO_INCLUDE_DIR "${ENGINE_FMOD_ROOT}/api/studio/inc")
set(ENGINE_FMOD_CORE_LIBRARY_DIR "${ENGINE_FMOD_ROOT}/api/core/lib/x64")
set(ENGINE_FMOD_STUDIO_LIBRARY_DIR "${ENGINE_FMOD_ROOT}/api/studio/lib/x64")

if(NOT ENGINE_FMOD_POLICY STREQUAL "OFF")
    # Debug links the logging libraries (fmodL, fmodstudioL), which report FMOD's own warnings; every other
    # configuration links the production libraries.
    set(ENGINE_FMOD_REQUIRED_FILES
        "${ENGINE_FMOD_CORE_INCLUDE_DIR}/fmod.hpp"
        "${ENGINE_FMOD_CORE_INCLUDE_DIR}/fmod_common.h"
        "${ENGINE_FMOD_CORE_INCLUDE_DIR}/fmod_errors.h"
        "${ENGINE_FMOD_STUDIO_INCLUDE_DIR}/fmod_studio.hpp"
        "${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmod_vc.lib"
        "${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmod.dll"
        "${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmodL_vc.lib"
        "${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmodL.dll"
        "${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudio_vc.lib"
        "${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudio.dll"
        "${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudioL_vc.lib"
        "${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudioL.dll"
    )

    set(ENGINE_FMOD_PROBLEMS "")

    foreach(required IN LISTS ENGINE_FMOD_REQUIRED_FILES)
        if(NOT EXISTS "${required}")
            list(APPEND ENGINE_FMOD_PROBLEMS "missing ${required}")
        endif()
    endforeach()

    if(NOT ENGINE_FMOD_PROBLEMS)
        file(STRINGS "${ENGINE_FMOD_CORE_INCLUDE_DIR}/fmod_common.h" ENGINE_FMOD_VERSION_LINE
            REGEX "^#define[ \t]+FMOD_VERSION[ \t]+0x[0-9a-fA-F]+")
        string(REGEX MATCH "0x[0-9a-fA-F]+" ENGINE_FMOD_VERSION "${ENGINE_FMOD_VERSION_LINE}")

        if(NOT ENGINE_FMOD_VERSION MATCHES "^${ENGINE_FMOD_REQUIRED_VERSION_PREFIX}")
            list(APPEND ENGINE_FMOD_PROBLEMS
                "FMOD_VERSION is '${ENGINE_FMOD_VERSION}', expected ${ENGINE_FMOD_REQUIRED_VERSION_PREFIX}xx (FMOD 2.04)")
        endif()
    endif()

    if(ENGINE_FMOD_PROBLEMS)
        if(ENGINE_FMOD_POLICY STREQUAL "ON")
            message(FATAL_ERROR "FMOD explicitly required, but the SDK at '${ENGINE_FMOD_ROOT}' is unusable: "
                "${ENGINE_FMOD_PROBLEMS}. Install the FMOD Engine 2.04 SDK for Windows, set ENGINE_FMOD_ROOT to its "
                "'FMOD Studio API Windows' directory, or use -DENGINE_ENABLE_FMOD=AUTO/OFF.")
        endif()

        message(STATUS "FMOD AUTO: audio backend disabled; ${ENGINE_FMOD_PROBLEMS}")
    else()
        set(ENGINE_FMOD_AVAILABLE ON)
        message(STATUS "FMOD audio backend enabled (version ${ENGINE_FMOD_VERSION}, SDK: ${ENGINE_FMOD_ROOT})")
    endif()
endif()

# Audio.1D: which FMOD DLLs an executable uses. An executable with the target property ENGINE_FMOD_LOGGING set links
# and deploys FMOD's logging DLLs (fmodL, fmodstudioL) in Debug, so FMOD reports its own warnings while developing
# (the Editor and the audio tests). Every other executable, above all EngineRuntime, whose directory Build Game
# deploys games from, uses the production DLLs (fmod, fmodstudio) in every configuration.
#
# The choice is made per consuming executable: the generator expression below is part of EngineAudio's link
# interface, and $<TARGET_PROPERTY:ENGINE_FMOD_LOGGING> there is evaluated on the executable being linked.
set(ENGINE_FMOD_LOGGING_DLLS "$<AND:$<CONFIG:Debug>,$<BOOL:$<TARGET_PROPERTY:ENGINE_FMOD_LOGGING>>>")

# Compiles a target's audio sources against the SDK when enabled. The import libraries and delay-load options reach
# every executable linking the target through its link interface; the headers stay private to the target.
#
# The DLLs are delay-loaded: an executable whose directory lacks them still starts, and AudioSystem reports audio
# unavailable after checking for them before any FMOD call (it reads their names from the delay-import table).
function(engine_configure_fmod target)
    if(NOT ENGINE_FMOD_AVAILABLE)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE ENGINE_HAS_FMOD=1)
    target_include_directories(${target} PRIVATE "${ENGINE_FMOD_CORE_INCLUDE_DIR}" "${ENGINE_FMOD_STUDIO_INCLUDE_DIR}")
    target_link_libraries(${target} PUBLIC
        "$<IF:${ENGINE_FMOD_LOGGING_DLLS},${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmodL_vc.lib,${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmod_vc.lib>"
        "$<IF:${ENGINE_FMOD_LOGGING_DLLS},${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudioL_vc.lib,${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudio_vc.lib>"
        delayimp
    )
    target_link_options(${target} INTERFACE
        "$<IF:${ENGINE_FMOD_LOGGING_DLLS},/DELAYLOAD:fmodL.dll,/DELAYLOAD:fmod.dll>"
        "$<IF:${ENGINE_FMOD_LOGGING_DLLS},/DELAYLOAD:fmodstudioL.dll,/DELAYLOAD:fmodstudio.dll>"
    )
endfunction()

# Copies the FMOD DLLs `target` links (see ENGINE_FMOD_LOGGING above) next to it.
function(engine_deploy_fmod target)
    if(NOT ENGINE_FMOD_AVAILABLE)
        return()
    endif()

    set(logging "$<AND:$<CONFIG:Debug>,$<BOOL:$<TARGET_PROPERTY:${target},ENGINE_FMOD_LOGGING>>>")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:${logging},${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmodL.dll,${ENGINE_FMOD_CORE_LIBRARY_DIR}/fmod.dll>"
            "$<IF:${logging},${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudioL.dll,${ENGINE_FMOD_STUDIO_LIBRARY_DIR}/fmodstudio.dll>"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying FMOD runtime DLLs for ${target}"
        VERBATIM
    )
endfunction()

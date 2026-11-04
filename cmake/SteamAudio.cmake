# Steam Audio (Valve) spatial audio for the FMOD backend (SA.1), optional.
#
# Steam Audio never plays audio itself: FMOD stays Engine's audio engine, and Steam Audio's FMOD plugin
# (phonon_fmod.dll) runs inside FMOD's mixer. Engine owns the Steam Audio context and HRTF that the plugin uses,
# through the C API (phonon.dll). See src/Audio/SteamAudioRuntime.h for the runtime contract.
#
# AUTO enables Steam Audio when FMOD is enabled and the complete SDK is present; OFF builds Engine without it (every
# project runs FMOD-only); ON requires both and fails configuration otherwise.
set(ENGINE_ENABLE_STEAMAUDIO "AUTO" CACHE STRING "Steam Audio build policy: AUTO, ON or OFF")
set_property(CACHE ENGINE_ENABLE_STEAMAUDIO PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${ENGINE_ENABLE_STEAMAUDIO}" ENGINE_STEAMAUDIO_POLICY)

if(NOT ENGINE_STEAMAUDIO_POLICY MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ENGINE_ENABLE_STEAMAUDIO must be AUTO, ON or OFF")
endif()

set(ENGINE_STEAMAUDIO_AVAILABLE OFF)

# The two official release archives (steamaudio_<version>.zip and steamaudio_fmod_<version>.zip), extracted side by
# side and unmodified: <root>/steamaudio holds the C API, <root>/steamaudio_fmod the FMOD plugin. The SDK stays outside
# the Engine repository, like FMOD's.
set(ENGINE_STEAMAUDIO_ROOT "${CMAKE_SOURCE_DIR}/../SDKs/SteamAudio/4.8.1"
    CACHE PATH "Directory containing the extracted Steam Audio 'steamaudio' and 'steamaudio_fmod' release folders")

# Exact pin. Engine validates the plugin's descriptor layout (legacy FMOD plugin SDK 110, as shipped in 4.8.1) at run
# time, so a different Steam Audio release needs re-verification before the pin moves.
set(ENGINE_STEAMAUDIO_REQUIRED_VERSION "4.8.1")

set(ENGINE_STEAMAUDIO_INCLUDE_DIR "${ENGINE_STEAMAUDIO_ROOT}/steamaudio/include")
set(ENGINE_STEAMAUDIO_IMPORT_LIBRARY "${ENGINE_STEAMAUDIO_ROOT}/steamaudio/lib/windows-x64/phonon.lib")
# The FMOD release's phonon.dll is the build the plugin ships with; Engine and the plugin share this one module.
set(ENGINE_STEAMAUDIO_RUNTIME_DIR "${ENGINE_STEAMAUDIO_ROOT}/steamaudio_fmod/lib/windows-x64")

if(NOT ENGINE_STEAMAUDIO_POLICY STREQUAL "OFF")
    set(ENGINE_STEAMAUDIO_PROBLEMS "")

    if(NOT ENGINE_FMOD_AVAILABLE)
        list(APPEND ENGINE_STEAMAUDIO_PROBLEMS "the FMOD backend is disabled")
    endif()

    foreach(required IN ITEMS
        "${ENGINE_STEAMAUDIO_INCLUDE_DIR}/phonon.h"
        "${ENGINE_STEAMAUDIO_INCLUDE_DIR}/phonon_version.h"
        "${ENGINE_STEAMAUDIO_IMPORT_LIBRARY}"
        "${ENGINE_STEAMAUDIO_RUNTIME_DIR}/phonon.dll"
        "${ENGINE_STEAMAUDIO_RUNTIME_DIR}/phonon_fmod.dll"
        "${ENGINE_STEAMAUDIO_ROOT}/steamaudio_fmod/THIRDPARTY.md")
        if(NOT EXISTS "${required}")
            list(APPEND ENGINE_STEAMAUDIO_PROBLEMS "missing ${required}")
        endif()
    endforeach()

    if(EXISTS "${ENGINE_STEAMAUDIO_INCLUDE_DIR}/phonon_version.h")
        set(ENGINE_STEAMAUDIO_VERSION_PARTS "")

        foreach(part IN ITEMS MAJOR MINOR PATCH)
            file(STRINGS "${ENGINE_STEAMAUDIO_INCLUDE_DIR}/phonon_version.h" line
                REGEX "^#define[ \t]+STEAMAUDIO_VERSION_${part}[ \t]+[0-9]+")
            string(REGEX MATCH "[0-9]+$" value "${line}")
            list(APPEND ENGINE_STEAMAUDIO_VERSION_PARTS "${value}")
        endforeach()

        list(JOIN ENGINE_STEAMAUDIO_VERSION_PARTS "." ENGINE_STEAMAUDIO_VERSION)

        if(NOT ENGINE_STEAMAUDIO_VERSION STREQUAL ENGINE_STEAMAUDIO_REQUIRED_VERSION)
            list(APPEND ENGINE_STEAMAUDIO_PROBLEMS
                "Steam Audio is ${ENGINE_STEAMAUDIO_VERSION}, expected ${ENGINE_STEAMAUDIO_REQUIRED_VERSION}")
        endif()
    endif()

    if(ENGINE_STEAMAUDIO_PROBLEMS)
        if(ENGINE_STEAMAUDIO_POLICY STREQUAL "ON")
            message(FATAL_ERROR "Steam Audio explicitly required, but it is unusable: ${ENGINE_STEAMAUDIO_PROBLEMS}. "
                "Extract steamaudio_${ENGINE_STEAMAUDIO_REQUIRED_VERSION}.zip and "
                "steamaudio_fmod_${ENGINE_STEAMAUDIO_REQUIRED_VERSION}.zip into ENGINE_STEAMAUDIO_ROOT, or use "
                "-DENGINE_ENABLE_STEAMAUDIO=AUTO/OFF.")
        endif()

        message(STATUS "Steam Audio AUTO: disabled; ${ENGINE_STEAMAUDIO_PROBLEMS}")
    else()
        set(ENGINE_STEAMAUDIO_AVAILABLE ON)
        message(STATUS "Steam Audio enabled (version ${ENGINE_STEAMAUDIO_VERSION}, SDK: ${ENGINE_STEAMAUDIO_ROOT})")
    endif()
endif()

# Compiles a target's Steam Audio sources against the SDK when enabled. phonon.dll is delay-loaded: AudioSystem
# loads it by full path from the executable's directory before any Steam Audio call, so an executable without it
# still starts and runs FMOD-only.
function(engine_configure_steamaudio target)
    if(NOT ENGINE_STEAMAUDIO_AVAILABLE)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE ENGINE_HAS_STEAMAUDIO=1)
    target_include_directories(${target} PRIVATE "${ENGINE_STEAMAUDIO_INCLUDE_DIR}")
    # bcrypt: the SHA-256 of the compatibility gate (SteamAudioSnapshotSource.cpp).
    target_link_libraries(${target} PUBLIC "${ENGINE_STEAMAUDIO_IMPORT_LIBRARY}" delayimp bcrypt)
    target_link_options(${target} INTERFACE "/DELAYLOAD:phonon.dll")
endfunction()

# Copies phonon.dll and phonon_fmod.dll next to `target`. Both must sit in the same directory: the plugin resolves
# phonon.dll beside itself, and Engine loads the same file, so the process holds a single Steam Audio module.
# GPUUtilities.dll and TrueAudioNext.dll are not deployed: phonon.dll delay-loads them for GPU features Engine
# does not use.
function(engine_deploy_steamaudio target)
    if(NOT ENGINE_STEAMAUDIO_AVAILABLE)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ENGINE_STEAMAUDIO_RUNTIME_DIR}/phonon.dll"
            "${ENGINE_STEAMAUDIO_RUNTIME_DIR}/phonon_fmod.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Deploying Steam Audio runtime DLLs for ${target}"
        VERBATIM
    )
endfunction()

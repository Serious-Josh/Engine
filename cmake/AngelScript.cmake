include(FetchContent)

set(ENGINE_ANGELSCRIPT_SOURCE_DIR "" CACHE PATH "Offline AngelScript 2.38.0 SDK root (contains angelscript and add_on)")
set(ENGINE_ANGELSCRIPT_ARCHIVE "" CACHE FILEPATH "Offline official AngelScript 2.38.0 zip (hash checked)")

function(engine_add_angelscript)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_SHARED_LIBS OFF)
    set(AS_DISABLE_INSTALL ON)
    set(AS_NO_EXCEPTIONS OFF)

    if(ENGINE_ANGELSCRIPT_SOURCE_DIR)
        set(sdk "${ENGINE_ANGELSCRIPT_SOURCE_DIR}")
    else()
        set(archive "https://www.angelcode.com/angelscript/sdk/files/angelscript_2.38.0.zip")
        if(ENGINE_ANGELSCRIPT_ARCHIVE)
            set(archive "${ENGINE_ANGELSCRIPT_ARCHIVE}")
        endif()

        FetchContent_Declare(engine_angelscript
            URL "${archive}"
            URL_HASH SHA256=b33b5dbcda10317ef67d628353d83246984ce6fcac102d4dc2aed121eba52e6f
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_SUBDIR angelscript/projects/cmake
        )
        FetchContent_MakeAvailable(engine_angelscript)
        set(sdk "${engine_angelscript_SOURCE_DIR}")
    endif()

    file(STRINGS "${sdk}/angelscript/include/angelscript.h" version
        REGEX "^#define ANGELSCRIPT_VERSION_STRING ")
    if(NOT version STREQUAL "#define ANGELSCRIPT_VERSION_STRING \"2.38.0\"")
        message(FATAL_ERROR "Engine requires the approved AngelScript SDK 2.38.0, found: ${version}")
    endif()

    if(NOT TARGET angelscript)
        add_subdirectory("${sdk}/angelscript/projects/cmake" "${CMAKE_BINARY_DIR}/angelscript")
    endif()

    get_target_property(library_type angelscript TYPE)
    if(NOT library_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "AngelScript must be a static library; refusing a runtime DLL dependency.")
    endif()

    add_library(EngineScriptString STATIC "${sdk}/add_on/scriptstdstring/scriptstdstring.cpp")
    target_include_directories(EngineScriptString SYSTEM PUBLIC "${sdk}/add_on/scriptstdstring")
    target_link_libraries(EngineScriptString PUBLIC angelscript)
    add_library(EngineScriptBuilder STATIC "${sdk}/add_on/scriptbuilder/scriptbuilder.cpp")
    target_include_directories(EngineScriptBuilder SYSTEM PUBLIC "${sdk}/add_on/scriptbuilder")
    target_link_libraries(EngineScriptBuilder PUBLIC angelscript)

    foreach(dependency IN ITEMS angelscript EngineScriptString EngineScriptBuilder)
        set_target_properties(${dependency} PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
        if(MSVC)
            if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
                set_property(TARGET ${dependency} PROPERTY MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
            else()
                set_property(TARGET ${dependency} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            endif()
            target_compile_options(${dependency} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W0>)
        endif()
    endforeach()

    # Preserve the exact source license, including the selected add-on's SDK attribution.
    file(READ "${sdk}/angelscript/include/angelscript.h" header)
    string(FIND "${header}" "*/" license_end)
    math(EXPR license_length "${license_end} + 2")
    string(SUBSTRING "${header}" 0 ${license_length} license)
    file(WRITE "${CMAKE_BINARY_DIR}/AngelScriptNotices.txt"
        "AngelScript SDK 2.38.0, scriptstdstring and scriptbuilder add-ons\nhttps://www.angelcode.com/angelscript/\n\n${license}\n")
endfunction()

engine_add_angelscript()

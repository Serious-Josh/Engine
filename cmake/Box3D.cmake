include(FetchContent)

# Scope BUILD_SHARED_LIBS to Box3D; do not change other dependency configurations.
function(engine_add_box3d)
    set(BUILD_SHARED_LIBS OFF)
    set(BOX3D_SAMPLES OFF CACHE BOOL "" FORCE)
    set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BOX3D_DOCS OFF CACHE BOOL "" FORCE)
    set(BOX3D_DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
    # Box3D v0.1.0; keep the full commit pin when updating this dependency.
    FetchContent_Declare(
        box3d
        GIT_REPOSITORY https://github.com/erincatto/box3d.git
        GIT_TAG 8441b4a06d6d09dcfb0b0f704df4d847d1437b92
    )
    FetchContent_MakeAvailable(box3d)

    # Upstream selects /MT in its subdirectory. Match the host CRT instead.
    if(MSVC)
        if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
            set_property(TARGET box3d PROPERTY MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
        else()
            set_property(TARGET box3d PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endif()
endfunction()

engine_add_box3d()

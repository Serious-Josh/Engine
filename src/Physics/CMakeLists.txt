add_library(EnginePhysics STATIC
    PhysicsWorld.cpp
    PhysicsComponent.cpp
    TriggerColliderComponent.cpp
    CharacterControllerComponent.cpp
    CharacterControllerMover.cpp
    ScenePhysicsSession.cpp
    RuntimePoseSnapshot.cpp
)
target_include_directories(EnginePhysics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(EnginePhysics PUBLIC cxx_std_23)
target_link_libraries(EnginePhysics PRIVATE box3d::box3d)

if(MSVC)
    target_compile_options(EnginePhysics PRIVATE /EHsc)
endif()

if(ENGINE_BUILD_DIAGNOSTICS)
    target_compile_definitions(EnginePhysics PUBLIC $<$<CONFIG:Debug>:ENGINE_HAS_VERIFICATION=1>)
endif()

# Standalone native regressions are available only with explicit diagnostics opt-in.
if(NOT ENGINE_BUILD_DIAGNOSTICS)
    return()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeSensorDiagnostic.cpp")
    add_executable(NativeSensorDiagnostic tests/NativeSensorDiagnostic.cpp
        PhysicsComponent.cpp TriggerColliderComponent.cpp CharacterControllerComponent.cpp CharacterControllerMover.cpp RuntimePoseSnapshot.cpp)
    target_include_directories(NativeSensorDiagnostic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_compile_features(NativeSensorDiagnostic PRIVATE cxx_std_23)
    target_link_libraries(NativeSensorDiagnostic PRIVATE box3d::box3d)
    if(MSVC)
        target_compile_options(NativeSensorDiagnostic PRIVATE /EHsc)
    endif()
endif()

# Test sources are local-only and Git-ignored; a clone without them still builds EnginePhysics.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CharacterControllerNativeDiagnostic.cpp")
    add_executable(CharacterControllerNativeDiagnostic tests/CharacterControllerNativeDiagnostic.cpp
        PhysicsComponent.cpp TriggerColliderComponent.cpp CharacterControllerComponent.cpp CharacterControllerMover.cpp RuntimePoseSnapshot.cpp)
    target_include_directories(CharacterControllerNativeDiagnostic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_compile_features(CharacterControllerNativeDiagnostic PRIVATE cxx_std_23)
    target_link_libraries(CharacterControllerNativeDiagnostic PRIVATE box3d::box3d)
    if(MSVC)
        target_compile_options(CharacterControllerNativeDiagnostic PRIVATE /EHsc)
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicsDiagnostic.cpp")
    add_executable(PhysicsDiagnostic tests/PhysicsDiagnostic.cpp)
    target_link_libraries(PhysicsDiagnostic PRIVATE EnginePhysics)

    add_executable(ScenePhysicsRegistrationDiagnostic tests/ScenePhysicsRegistrationDiagnostic.cpp)
    # Only the diagnostic observes Box3D's global allocation counter; no IDs escape.
    target_link_libraries(ScenePhysicsRegistrationDiagnostic PRIVATE EnginePhysics box3d::box3d)

    add_executable(RuntimePhysicsPoseDiagnostic tests/RuntimePhysicsPoseDiagnostic.cpp)
    target_link_libraries(RuntimePhysicsPoseDiagnostic PRIVATE EnginePhysics)

    add_executable(ContactEventDiagnostic tests/ContactEventDiagnostic.cpp)
    # Only the diagnostic observes Box3D's global allocation counter; no IDs escape.
    target_link_libraries(ContactEventDiagnostic PRIVATE EnginePhysics box3d::box3d)

    if(MSVC)
        target_compile_options(PhysicsDiagnostic PRIVATE /EHsc)
        target_compile_options(ScenePhysicsRegistrationDiagnostic PRIVATE /EHsc)
        target_compile_options(RuntimePhysicsPoseDiagnostic PRIVATE /EHsc)
        target_compile_options(ContactEventDiagnostic PRIVATE /EHsc)
    endif()
endif()

# CM.6 local-only controller runtime diagnostic over the production EnginePhysics library.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CharacterControllerRuntimeDiagnostic.cpp")
    add_executable(CharacterControllerRuntimeDiagnostic tests/CharacterControllerRuntimeDiagnostic.cpp)
    target_link_libraries(CharacterControllerRuntimeDiagnostic PRIVATE EnginePhysics)
    if(MSVC)
        target_compile_options(CharacterControllerRuntimeDiagnostic PRIVATE /EHsc)
    endif()
endif()

# CM.8 local-only interaction diagnostic; compiles World/Session in its own TU like NativeSensorDiagnostic.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CharacterControllerInteractionDiagnostic.cpp")
    add_executable(CharacterControllerInteractionDiagnostic tests/CharacterControllerInteractionDiagnostic.cpp
        PhysicsComponent.cpp TriggerColliderComponent.cpp CharacterControllerComponent.cpp CharacterControllerMover.cpp
        RuntimePoseSnapshot.cpp)
    target_include_directories(CharacterControllerInteractionDiagnostic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_compile_features(CharacterControllerInteractionDiagnostic PRIVATE cxx_std_23)
    target_link_libraries(CharacterControllerInteractionDiagnostic PRIVATE box3d::box3d)
    if(MSVC)
        target_compile_options(CharacterControllerInteractionDiagnostic PRIVATE /EHsc)
    endif()
endif()

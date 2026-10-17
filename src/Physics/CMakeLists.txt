add_library(EnginePhysics STATIC
    PhysicsWorld.cpp
    PhysicsComponent.cpp
    ScenePhysicsSession.cpp
    RuntimePoseSnapshot.cpp
)
target_include_directories(EnginePhysics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(EnginePhysics PUBLIC cxx_std_23)
target_link_libraries(EnginePhysics PRIVATE box3d::box3d)

if(MSVC)
    target_compile_options(EnginePhysics PRIVATE /EHsc)
endif()

# Test sources are local-only and Git-ignored; a clone without them still builds EnginePhysics.
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

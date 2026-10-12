#include "Scene/SceneDocumentAdapter.h"
#include "Scene/Scene.h"

#include <cassert>
#include <iostream>
#include <string>

int main()
{
    Scene scene;

    SceneDocument document;
    document.id = "adapter-test";
    document.name = "Adapter Test";

    document.camera.position = { 2.0f, 3.0f, -8.0f };
    document.camera.target = { 0.0f, 1.0f, 0.0f };
    document.camera.verticalFov = 0.9f;
    document.camera.aspectRatio = 1.5f;
    document.camera.nearPlane = 0.25f;
    document.camera.farPlane = 500.0f;

    document.directionalLight.intensity = 2.5f;
    document.directionalLight.ambientStrength = 0.15f;

    std::string error;

    assert(ApplySceneDocumentSettings(document, scene, &error));

    const Camera& camera = scene.GetCamera();

    assert(camera.GetPosition().x == 2.0f);
    assert(camera.GetPosition().y == 3.0f);
    assert(camera.GetPosition().z == -8.0f);

    assert(camera.GetTarget().y == 1.0f);

    assert(camera.GetVerticalFov() == 0.9f);
    assert(camera.GetAspectRatio() == 1.5f);
    assert(camera.GetNearPlane() == 0.25f);
    assert(camera.GetFarPlane() == 500.0f);

    assert(scene.GetDirectionalLight().intensity == 2.5f);
    assert(scene.GetDirectionalLight().ambientStrength == 0.15f);

    assert(scene.GetRenderObjects().empty());

    // Invalid input must not modify the runtime scene.
    SceneDocument invalid = document;
    invalid.camera.nearPlane = -1.0f;

    assert(!ApplySceneDocumentSettings(invalid, scene, &error));

    assert(scene.GetCamera().GetNearPlane() == 0.25f);
    assert(scene.GetDirectionalLight().intensity == 2.5f);
    assert(scene.GetRenderObjects().empty());

    std::cout << "Scene document adapter tests passed.\n";
    return 0;
}
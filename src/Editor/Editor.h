#pragma once

class IDemoScene;
class SphereGridScene;
class DemoSceneManager;

class Editor
{
    public:
        void Draw(IDemoScene& demoScene, DemoSceneManager& sceneManager, SphereGridScene* sphereGridScene = nullptr);

    private:
        void DrawSphereGridControls(SphereGridScene& demoScene);
        void DrawDirectionalLight(IDemoScene& demoScene);
        void DrawCamera(IDemoScene& demoScene);
};
#pragma once

class DemoScene;

class Editor
{
    public:
        void Draw(DemoScene& demoScene);

    private:
        void DrawDemoControls(DemoScene& demoScene);
        void DrawDirectionalLight(DemoScene& demoScene);
        void DrawCamera(DemoScene& demoScene);
};
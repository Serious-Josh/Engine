#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class IDemoScene;

using DemoSceneFactory = std::unique_ptr<IDemoScene>(*)();

struct DemoSceneEntry
{
    std::string name;
    DemoSceneFactory create = nullptr;
};

class SceneRegistry
{
public:
    static SceneRegistry& Instance();

    bool Register(std::string_view name, DemoSceneFactory factory);

    bool Finalize();

    std::span<const DemoSceneEntry> GetScenes() const;

    const DemoSceneEntry* Find(
        std::string_view name) const;

private:
    std::vector<DemoSceneEntry> m_scenes;

    bool m_finalized = false;
    bool m_registrationError = false;
};

#define ENGINE_CONCAT_IMPL(a, b) a##b
#define ENGINE_CONCAT(a, b) ENGINE_CONCAT_IMPL(a, b)

#define REGISTER_DEMO_SCENE(displayName, SceneType)                    \
    namespace                                                          \
    {                                                                  \
        const bool ENGINE_CONCAT(g_demoSceneRegistered_, __LINE__) =   \
            SceneRegistry::Instance().Register(                        \
                displayName,                                           \
                +[]() -> std::unique_ptr<IDemoScene>                   \
                {                                                      \
                    return std::make_unique<SceneType>();              \
                });                                                    \
    }
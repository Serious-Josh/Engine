#include "DemoContent/SceneRegistry.h"

#include <algorithm>


SceneRegistry& SceneRegistry::Instance()
{
    static SceneRegistry registry;
    return registry;
}


bool SceneRegistry::Register(std::string_view name, DemoSceneFactory factory)
{
    if (m_finalized || name.empty() || factory == nullptr)
    {
        m_registrationError = true;
        return false;
    }

    const auto existing =
        std::find_if(
            m_scenes.begin(),
            m_scenes.end(),
            [name](const DemoSceneEntry& entry)
            {
                return entry.name == name;
            });

    if (existing != m_scenes.end())
    {
        m_registrationError = true;
        return false;
    }

    DemoSceneEntry entry{};
    entry.name = name;
    entry.create = factory;

    m_scenes.push_back(std::move(entry));

    return true;
}


bool SceneRegistry::Finalize()
{
    if (m_finalized)
    {
        return !m_registrationError;
    }

    std::sort(
        m_scenes.begin(),
        m_scenes.end(),
        [](const DemoSceneEntry& a,
           const DemoSceneEntry& b)
        {
            return a.name < b.name;
        });

    m_finalized = true;

    return !m_registrationError;
}


std::span<const DemoSceneEntry>SceneRegistry::GetScenes() const
{
    return m_scenes;
}


const DemoSceneEntry* SceneRegistry::Find(std::string_view name) const
{
    const auto result = std::find_if(m_scenes.begin(), m_scenes.end(), [name](const DemoSceneEntry& entry){return entry.name == name;});

    if (result == m_scenes.end())
    {
        return nullptr;
    }

    return &(*result);
}
#pragma once

#include "Scene/FileScene.h"
#include "Scene/Scene.h"
#include "Scene/SceneCommit.h"
#include "Scene/PreparedScene.h"

#include <optional>
#include <string>



class FileSceneRuntime
{
public:
    FileSceneRuntime() = default;

    FileSceneRuntime(const FileSceneRuntime&) = delete;
    FileSceneRuntime& operator=(const FileSceneRuntime&) = delete;

    const FileScene& GetFileScene() noexcept
    {
        return m_fileScene;
    }

    const FileScene& GetFileScene() const noexcept
    {
        return m_fileScene;
    }

    Scene& GetScene() noexcept
    {
        return m_scene;
    }

    const Scene& GetScene() const noexcept
    {
        return m_scene;
    }

    const SceneObjectMapping& GetObjectMapping() const noexcept
    {
        return m_objectMapping;
    }

    bool Prepare(
        SceneAssetResolver& resolver,
        std::string* errorMessage = nullptr);

    bool Commit(std::string* errorMessage = nullptr);

    bool HasPreparedScene() const noexcept
    {
        return m_preparedScene.has_value();
    }

    void DiscardPreparedScene() noexcept
    {
        m_preparedScene.reset();
    }

    bool New(
        std::string sceneID,
        std::string sceneName = "Untitled Scene");

    bool Open(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

    SceneDocument& EditDocument() noexcept;

    // Returns false when live state cannot be represented safely.
    // On failure, dirty is unchanged.
    bool CheckUnsavedChanges(
        bool& dirty,
        std::string* errorMessage = nullptr) const;

    bool Save(std::string* errorMessage = nullptr);

    bool SaveAs(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

private:
    FileScene m_fileScene;
    Scene m_scene;
    SceneObjectMapping m_objectMapping;
    std::optional<PreparedScene> m_preparedScene;
};
#include "Scene/FileSceneRuntime.h"

#include "Scene/SceneAssetResolver.h"
#include "Scene/SceneDocumentAdapter.h"

#include <utility>

bool FileSceneRuntime::Prepare(
    SceneAssetResolver& resolver,
    std::string* errorMessage)
{
    if (!m_fileScene.HasDocument())
    {
        if (errorMessage)
        {
            *errorMessage = "No file-backed scene document is loaded.";
        }

        return false;
    }

    PreparedScene prepared;

    if (!PrepareScene(
            m_fileScene.GetDocument(),
            resolver,
            prepared,
            errorMessage))
    {
        return false;
    }

    m_preparedScene = std::move(prepared);
    return true;
}

bool FileSceneRuntime::Commit(std::string* errorMessage)
{
    if (!m_preparedScene)
    {
        if (errorMessage)
        {
            *errorMessage = "No prepared scene is available.";
        }

        return false;
    }

    if (!CommitPreparedScene(
            *m_preparedScene,
            m_scene,
            m_objectMapping,
            errorMessage))
    {
        return false;
    }

    m_preparedScene.reset();
    return true;
}

bool FileSceneRuntime::New(
    std::string sceneID,
    std::string sceneName)
{
    if (!m_fileScene.New(
            std::move(sceneID),
            std::move(sceneName)))
    {
        return false;
    }

    m_preparedScene.reset();
    m_objectMapping = {};
    return true;
}

bool FileSceneRuntime::Open(
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    if (!m_fileScene.Open(path, errorMessage))
    {
        return false;
    }

    m_preparedScene.reset();
    m_objectMapping = {};
    return true;
}

SceneDocument& FileSceneRuntime::EditDocument() noexcept
{
    m_preparedScene.reset();
    return m_fileScene.EditDocument();
}

bool FileSceneRuntime::Save(std::string* errorMessage)
{
    return SaveAs(m_fileScene.GetPath(), errorMessage);
}

bool FileSceneRuntime::SaveAs(
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    SceneDocument candidate;

    if (!m_fileScene.HasDocument() ||
        !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, candidate, errorMessage))
    {
        return false;
    }

    return m_fileScene.SaveCaptured(std::move(candidate), path, errorMessage);
}
bool FileSceneRuntime::CheckUnsavedChanges(
    bool& dirty,
    std::string* errorMessage) const
{
    SceneDocument candidate;

    if (!m_fileScene.HasDocument() ||
        !CaptureSceneDocument(m_fileScene.GetDocument(), m_scene, m_objectMapping, candidate, errorMessage))
    {
        return false;
    }

    dirty = m_fileScene.IsDirty() || !SceneDocumentsEqual(m_fileScene.GetDocument(), candidate);
    return true;
}

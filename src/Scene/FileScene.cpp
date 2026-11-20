#include "Scene/FileScene.h"

#include "Scene/SceneDocumentCache.h"
#include "Scene/SceneSerializer.h"

#include <utility>

namespace
{
    bool Fail(
        std::string* errorMessage,
        const std::string& message)
    {
        if (errorMessage)
        {
            *errorMessage = message;
        }

        return false;
    }
}

bool FileScene::New(
    std::string sceneID,
    std::string sceneName)
{
    SceneDocument document;
    document.id = std::move(sceneID);
    document.name = std::move(sceneName);

    if (!ValidateSceneDocument(document))
    {
        return false;
    }

    m_document = std::move(document);
    m_path.clear();
    // M3.5: a replaced document is new content for every revision-keyed cache.
    ++m_editRevision;

    m_hasDocument = true;
    m_dirty = true;

    return true;
}

bool FileScene::Open(
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    // W6.0: parsed once per file content (the open request has usually parsed the same bytes); the scene edits its own copy.
    SceneDocumentCache::Parsed loaded;

    if (!SceneDocumentCache::Load(path, loaded, errorMessage, "FileScene open") ||
        !ValidateSceneKindOpenable(*loaded.document, errorMessage))
    {
        return false;
    }

    m_document = SceneDocumentCache::Copy(*loaded.document);
    m_path = path;
    ++m_editRevision;

    m_hasDocument = true;
    m_dirty = false;

    return true;
}

bool FileScene::LoadDocument(SceneDocument document, std::string* error)
{
    if(!ValidateSceneDocument(document,error) || !ValidateSceneKindOpenable(document,error)) return false;
    m_document = std::move(document);
    m_path.clear();
    ++m_editRevision;
    m_hasDocument = true;
    m_dirty = false;
    return true;
}

bool FileScene::Save(std::string* errorMessage)
{
    if (!m_hasDocument)
    {
        return Fail(errorMessage, "No scene document is open.");
    }

    if (m_path.empty())
    {
        return Fail(
            errorMessage,
            "Scene has no file path. Use SaveAs first.");
    }

    if (!SaveSceneDocument(m_document, m_path, errorMessage))
    {
        return false;
    }

    m_dirty = false;
    return true;
}

bool FileScene::SaveAs(
    const std::filesystem::path& path,
    std::string* errorMessage)
{
    if (!m_hasDocument)
    {
        return Fail(errorMessage, "No scene document is open.");
    }

    if (!SaveSceneDocument(m_document, path, errorMessage))
    {
        return false;
    }

    m_path = path;
    m_dirty = false;

    return true;
}

const SceneDocument& FileScene::GetDocument() const noexcept
{
    return m_document;
}

SceneDocument& FileScene::EditDocument() noexcept
{
    m_dirty = true;
    ++m_editRevision;
    return m_document;
}

const std::filesystem::path& FileScene::GetPath() const noexcept
{
    return m_path;
}

bool FileScene::HasDocument() const noexcept
{
    return m_hasDocument;
}

bool FileScene::IsDirty() const noexcept
{
    return m_dirty;
}
bool FileScene::SaveCaptured(
    SceneDocument candidate,
    const std::filesystem::path& path,
    std::string* errorMessage,
    bool replaceExisting)
{
    if (!m_hasDocument || path.empty())
    {
        return Fail(errorMessage, "An open document and scene file path are required.");
    }

    // Allocate the new path before replacing the file.
    std::filesystem::path savedPath = path;

    const bool written = replaceExisting ? SaveSceneDocument(candidate, savedPath, errorMessage) :
        CreateSceneDocument(candidate, savedPath, errorMessage) == CreateSceneDocumentResult::Created;

    if(!written)
    {
        return false;
    }

    m_document = std::move(candidate);
    m_path = std::move(savedPath);
    ++m_editRevision;
    m_dirty = false;
    return true;
}

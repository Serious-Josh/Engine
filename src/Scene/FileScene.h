#pragma once

#include <cstdint>

#include "Scene/SceneDocument.h"

#include <filesystem>
#include <string>

class FileScene
{
public:
    bool New(
        std::string sceneID,
        std::string sceneName = "Untitled Scene");

    bool Open(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

    // In-memory source: no disk read and no fabricated path.
    bool LoadDocument(SceneDocument document, std::string* errorMessage = nullptr);
    bool Save(std::string* errorMessage = nullptr);

    // Publishes the candidate in memory only after transactional disk replacement.
    bool SaveCaptured(
        SceneDocument candidate,
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr,
        bool replaceExisting = true);

    bool SaveAs(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

    const SceneDocument& GetDocument() const noexcept;
    SceneDocument& EditDocument() noexcept;
    std::uint64_t GetEditRevision() const noexcept { return m_editRevision; }

    const std::filesystem::path& GetPath() const noexcept;

    bool HasDocument() const noexcept;
    bool IsDirty() const noexcept;

private:
    SceneDocument m_document;
    std::filesystem::path m_path;

    bool m_hasDocument = false;
    bool m_dirty = false;
    // Incremented by every EditDocument call (the only writable access to the document) and, since M3.5, by every document
    // replacement (New, Open, LoadDocument, Save As): the content half of the persistent validated object index's and the
    // lighting snapshot cache's invalidation (FileSceneRuntime, revised S0.3).
    std::uint64_t m_editRevision = 0;
};

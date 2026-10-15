#pragma once

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
        std::string* errorMessage = nullptr);

    bool SaveAs(
        const std::filesystem::path& path,
        std::string* errorMessage = nullptr);

    const SceneDocument& GetDocument() const noexcept;
    SceneDocument& EditDocument() noexcept;

    const std::filesystem::path& GetPath() const noexcept;

    bool HasDocument() const noexcept;
    bool IsDirty() const noexcept;

private:
    SceneDocument m_document;
    std::filesystem::path m_path;

    bool m_hasDocument = false;
    bool m_dirty = false;
};
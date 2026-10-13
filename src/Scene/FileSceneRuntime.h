#pragma once

#include "Scene/FileScene.h"
#include "Scene/Scene.h"
#include "Scene/SceneCommit.h"
#include "Scene/PreparedScene.h"

#include <optional>
#include <string>

// Copied document metadata. Identity fields are internal selection checks, not UI labels.
struct SceneObjectMetadata
{
    std::string name;
    std::string meshReference;
    std::string materialReference;
    std::string objectID;
    std::string sceneID;

    RenderObjectID runtimeIndex = 0;

    std::uint64_t runtimeIdentity = 0;
    std::uint64_t structureRevision = 0;
};

struct MaterialAssignmentToken
{
    std::uint64_t runtimeIdentity = 0;
    std::uint64_t transaction = 0;

    bool noOp = false;
    bool operator==(const MaterialAssignmentToken&) const = default;
};

class FileSceneRuntime
{
public:
    FileSceneRuntime();
    bool HasMaterialAssignment() const noexcept
    {
        return m_materialAssignment.has_value();
    }

    std::uint64_t GetRuntimeIdentity() const noexcept
    {
        return m_instanceIdentity;
    }

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

    std::optional<SceneObjectMetadata> GetObjectMetadata(
        RenderObjectID runtimeIndex,
        std::string* errorMessage = nullptr) const;

    // CPU preparation only; one outstanding assignment per runtime.
    std::optional<MaterialAssignmentToken> PrepareMaterialAssignment(
        const SceneObjectMetadata& selection, std::string materialReference,
        std::string* errorMessage = nullptr);
    // Caller must first release Renderer references and prove GPU/recording safety.
    // Caller owns and retains BOTH materials through cleanup, rollback and GPU completion.
    bool CommitMaterialAssignment(const MaterialAssignmentToken& token, Material& retainedSceneMaterial,
        std::string* errorMessage = nullptr);
    // Same Renderer/GPU safety precondition as commit; never destroys either material.
    // Rejects intervening edits rather than overwriting them. Finish only after Renderer success.
    bool RollbackMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool FinishMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);
    bool CancelMaterialAssignment(const MaterialAssignmentToken& token, std::string* errorMessage = nullptr);

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
    struct MaterialAssignment
    {
        MaterialAssignmentToken token;

        SceneObjectMetadata selection;

        FileScene original;
        FileScene candidate;

        SceneDocument liveBefore;

        Material* previousMaterial = nullptr;
        Material* appliedMaterial = nullptr;

        bool applied = false;
    };
    std::optional<MaterialAssignment> m_materialAssignment;

    std::uint64_t m_nextMaterialAssignment = 1;
    const std::uint64_t m_instanceIdentity;
    FileScene m_fileScene;

    Scene m_scene;

    SceneObjectMapping m_objectMapping;

    std::optional<PreparedScene> m_preparedScene;
};

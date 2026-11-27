#pragma once

#include "Scene/SceneDocument.h"
#include "Scene/SceneEnvironmentResolution.h"
#include "assets/AssetID.h"

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ProjectPaths;
class ProjectAssetRegistry;

// W2: the one path that changes World/Local membership. Every operation reads the affected documents, checks their identities
// and the project-wide membership invariants, prepares every changed document (and the registry for new scenes), checks the
// result in both directions, and commits all of it in one AssetReconciliationTransaction with the bytes it read as expected
// originals. Nothing is repaired silently: a disagreement between a World and a Local Level is reported and the operation refused.
//
// Invariants (checked across documents):
//   - every World member names an existing Local Level whose owningWorld is that World, exactly once;
//   - every assigned Local Level is listed by its owning World, and by no other World;
//   - a World always has at least one Local Level; no scene lists itself.
// Placement belongs to the World's membership entry; no operation rewrites a Local Level's entity transforms.
//
// Paths are project-relative ".scene" paths. `registry` is replaced only after a successful commit.
namespace ProjectSceneMembership
{
    enum class EnvironmentSource
    {
        // The World starts from the initial Local Level's scene environment and simulation settings (copied; the Local Level
        // keeps its own).
        Import,
        // The World starts from the Engine's default scene settings.
        Default
    };

    struct NewScene
    {
        std::filesystem::path relativePath;
        std::string name;
    };

    struct CreateWorldRequest
    {
        NewScene world;
        // Exactly one of: an existing unassigned Local Level, or a new Local Level to create.
        std::optional<AssetID> existingLocal;
        std::optional<NewScene> newLocal;
        EnvironmentSource environment = EnvironmentSource::Default;
        ScenePlacement placement;
    };

    struct Result
    {
        std::optional<AssetID> world;
        std::optional<AssetID> local;
        std::vector<std::filesystem::path> written;
        // W3: the effective environment groups of the Local Level that the operation changed (empty when none).
        std::string environmentChanges;
    };

    bool CreateWorld(const ProjectPaths& paths, ProjectAssetRegistry& registry, const CreateWorldRequest& request,
        Result& result, std::string& error);

    bool AssignLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& world, const AssetID& local,
        const ScenePlacement& placement, Result& result, std::string& error);

    // W3: replaces the World's environment fields with `environment` (World-owned values only). Refused when the World no longer
    // holds `expected` (changed on disk since it was shown). Local Level documents are never rewritten: they inherit by resolution.
    bool UpdateWorldEnvironment(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& world,
        const EffectiveSceneEnvironment& expected, const EffectiveSceneEnvironment& environment, Result& result, std::string& error);

    bool CreateAndAssignLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& world,
        const NewScene& local, const ScenePlacement& placement, Result& result, std::string& error);

    // Removes the Local Level from its World and clears its owner; its content and identity are unchanged. Refused when it is
    // the World's last Local Level.
    bool UnassignLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& local, Result& result,
        std::string& error);

    // W5: writes edited scene documents (a World and/or its Local Levels) in one transaction: all are committed or none. Each
    // file must still hold `expectedOriginal` (the bytes it was read from), so a document changed on disk is never overwritten.
    struct DocumentSave
    {
        std::filesystem::path relativePath;
        SceneDocument document;
        std::string expectedOriginal;
    };

    bool SaveSceneDocuments(const ProjectPaths& paths, ProjectAssetRegistry& registry, const std::vector<DocumentSave>& saves,
        Result& result, std::string& error);

    // Changes only the World's membership entry.
    bool UpdatePlacement(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& world, const AssetID& local,
        const ScenePlacement& placement, Result& result, std::string& error);

    // A new, unassigned Local Level: new scene identity, new entity identities with hierarchy references remapped, every other
    // field (including asset references) unchanged. Refused for Worlds. The source is not modified.
    bool DuplicateLocal(const ProjectPaths& paths, ProjectAssetRegistry& registry, const AssetID& local, const NewScene& duplicate,
        Result& result, std::string& error);

    // Save As of an open Local Level: `snapshot` is the in-memory document (with unsaved edits). Writes it to `destination` as a
    // new, independent, unassigned Local Level (the same copy as DuplicateLocal) and registers it; the source file is not touched.
    // Refused for Worlds, for assigned Local Levels and for an existing destination.
    bool SaveLocalAsNew(const ProjectPaths& paths, ProjectAssetRegistry& registry, const SceneDocument& snapshot,
        const std::filesystem::path& destination, Result& result, std::string& error);

    // Read-only, project-wide check of the invariants; one message per problem.
    struct SceneInfo
    {
        AssetID id;
        std::filesystem::path relativePath;
        SceneKind kind = SceneKind::Local;
        std::string name;
        std::string owningWorld;
        std::vector<SceneMembership> members;
        // The scene's own environment values (for a World: its World-owned environment).
        EffectiveSceneEnvironment environment;
    };

    // Startup work (4.3): cancel, when given and set, stops the read before the next scene (a reader on another thread).
    bool ReadScenes(const ProjectPaths& paths, const ProjectAssetRegistry& registry, std::vector<SceneInfo>& scenes,
        std::vector<std::string>& problems, const std::atomic<bool>* cancel = nullptr);
    std::vector<std::string> ValidateProject(const std::vector<SceneInfo>& scenes);
}

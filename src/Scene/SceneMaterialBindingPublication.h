#pragma once

#include "Scene/FileSceneRuntime.h"

#include <map>
#include <memory>
#include <string>
#include <thread>

class Renderer;
class FileSceneAssetCatalog;

// Main Engine thread, between frames. Preparation never mutates the scene or
// submits commands. The renderer and descriptor heap must outlive this handle;
// cancel it before renderer shutdown. No GPU references exist before acceptance.
class SceneMaterialBindingPublication
{
public:
    enum class Status { Ready, Retry, Stale, Unsupported, Failed };
    struct Result
    {
        Status status = Status::Failed;
        std::string message;
        bool published = false;
    };

    SceneMaterialBindingPublication();
    ~SceneMaterialBindingPublication();
    SceneMaterialBindingPublication(const SceneMaterialBindingPublication&) = delete;
    SceneMaterialBindingPublication& operator=(const SceneMaterialBindingPublication&) = delete;

    // Complete authored override map, not a patch. Missing entries inherit the
    // unchanged object fallback. Use every authoritative slot ID for an all-slot
    // replacement. References must be registered materials already admitted to
    // this renderer's fixed material table. Never loads assets or grows the table.
    Result Prepare(Renderer& renderer, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const SceneObjectMetadata& expected,
        const std::map<std::string, std::string>& overrides);
    Result PrepareAllSlots(Renderer& renderer, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog, const SceneObjectMetadata& expected, const AssetID& material);
    Result PublishAtBoundary(Renderer& renderer, FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog);
    void Cancel() noexcept;

private:
    struct Prepared;
    std::unique_ptr<Prepared> m_prepared;
    const std::thread::id m_thread;
    Result Validate(Renderer& renderer, const FileSceneRuntime& runtime,
        FileSceneAssetCatalog& catalog) const;
};

#pragma once
#include "Scene/SceneDocument.h"
#include <memory>
#include <optional>
#include <string>
#include <thread>
class Renderer;
class FileSceneRuntime;
class FileSceneAssetCatalog;
struct SceneObjectMetadata;

// Exclusive main-thread transaction. Caller retains the catalog through renderer
// retirement/shutdown. No editor command or general runtime spawning API.
class SceneMembershipPublication
{
public:
    enum class Status { Ready, Retry, Stale, Unsupported, Failed };
    struct Result { Status status; std::string message; };
    SceneMembershipPublication();
    ~SceneMembershipPublication();
    SceneMembershipPublication(const SceneMembershipPublication&)=delete;
    SceneMembershipPublication& operator=(const SceneMembershipPublication&)=delete;
    Result PrepareInsertion(Renderer&,const FileSceneRuntime&,FileSceneAssetCatalog&,
        const SceneObjectDocument&,std::size_t index,unsigned int frameSlot, std::string_view bindingSourceID = {});
    Result PrepareRemoval(Renderer&,const FileSceneRuntime&,FileSceneAssetCatalog&,
        std::string_view objectID,unsigned int frameSlot);
    // Swaps authoritative runtime and GPU membership together. A retained stable
    // selection ID maps through the new runtime; a removed selection becomes empty.
    Result PublishAtBoundary(Renderer&,std::unique_ptr<FileSceneRuntime>&,
        FileSceneAssetCatalog&,std::optional<std::string_view> selectionID,
        std::optional<std::size_t>& selectionIndex);
    std::optional<SceneObjectMetadata> GetPreparedSelection(std::string_view id) const;
    void Cancel() noexcept;
private:
    struct Prepared;
    std::unique_ptr<Prepared> m_prepared;
    std::thread::id m_thread;
    Result Prepare(Renderer&,const FileSceneRuntime&,FileSceneAssetCatalog&,
        const SceneObjectDocument* insertion,std::string_view removal,std::size_t index,unsigned int frameSlot,std::string_view bindingSourceID = {});
};

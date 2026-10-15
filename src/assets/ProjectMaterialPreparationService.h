#pragma once
#include "assets/MaterialAssetData.h"
#include "assets/ProjectTexturePreparationService.h"
#include <array>
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
#include <functional>
#endif

class MaterialSourceLease;

// CPU-only, main-thread coordinator. The supplied texture service must outlive it.
// Session identifies the project/renderer epoch; invalidate before replacing either.
class ProjectMaterialPreparationService
{
public:
    enum class Status { Pending, Ready, Stale, Unsupported, Failed, Retry };
    struct Candidate
    {
        std::string assetID;
        std::filesystem::path project, path;
        std::string digest;
        std::uint64_t session = 0;
        MaterialAssetData parameters;
        std::array<std::optional<ProjectTexturePreparationService::Completion>, 7> textures;
        // Revocation observation only; never substitutes for Validate/ownership.
        bool HasPinnedSources() const noexcept;
    private:
        friend class ProjectMaterialPreparationService;
        std::shared_ptr<MaterialSourceLease> source;
    };
    struct Result
    {
        Status status = Status::Pending;
        std::shared_ptr<const Candidate> candidate;
        std::string message;
    };
    explicit ProjectMaterialPreparationService(ProjectTexturePreparationService& textures);
    ~ProjectMaterialPreparationService();
    ProjectMaterialPreparationService(const ProjectMaterialPreparationService&) = delete;
    ProjectMaterialPreparationService& operator=(const ProjectMaterialPreparationService&) = delete;
    Status Request(const ProjectPaths&, const ProjectAssetRegistry&, const AssetID&,
        std::uint64_t session, std::uint64_t& ticket, std::string& error);
    Result Poll(std::uint64_t ticket, const ProjectPaths&, const ProjectAssetRegistry&, std::uint64_t session);
    bool Validate(const Candidate&, const ProjectPaths&, const ProjectAssetRegistry&, std::uint64_t session,
        std::string& error) const;
    void Cancel(std::uint64_t ticket);
    void Invalidate();
    void Shutdown();
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
    void SetPinnedTestHook(std::function<void()> hook);
#endif
    static constexpr size_t Capacity = 4;
    static constexpr size_t MaterialByteLimit = 1024 * 1024;
private:
    struct State;
    std::unique_ptr<State> m_state;
};

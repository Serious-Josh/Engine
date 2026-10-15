#pragma once

#include "Renderer/ProjectTextureLoader.h"
#include "assets/ProjectAssetResolver.h"

#include <atomic>
#include <memory>
#include <optional>
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
#include <functional>
#endif

class TextureSourceEvidence;

// Main-thread request/poll/cancel API. The single worker owns CPU inputs only.
class ProjectTexturePreparationService
{
public:
    struct Limits
    {
        size_t requests = 16; // Includes subscribers, active and completed requests.
        size_t completed = 4;
        size_t sourceBytes = 64 * 1024 * 1024;
        size_t preparationBytes = 256 * 1024 * 1024;
        size_t retainedBytes = 512 * 1024 * 1024;
    };

    struct Options
    {
        unsigned int policyRevision = 1; // Existing PrepareProjectTexture policy.
        bool operator==(const Options&) const = default;
    };

    struct Identity
    {
        std::filesystem::path project;
        std::filesystem::path path;
        std::string assetID;
        std::string digest;
        ProjectTextureInterpretation interpretation = ProjectTextureInterpretation::LinearData;
        Options options;
        bool operator==(const Identity&) const = default;
    };

    struct Completion
    {
        Identity identity;
        std::uint64_t ticket = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<const PreparedProjectTexture> prepared;
        std::string error;
        std::shared_ptr<const std::atomic_bool> current;
        // Worker-owned read/delete exclusion, retained through GPU acceptance.
        // Opaque and revocable; callers must validate it, not inspect timestamps.
        std::shared_ptr<TextureSourceEvidence> source;
        std::uintmax_t sourceBytes = 0;
        std::filesystem::file_time_type sourceModified;
    };

    ProjectTexturePreparationService();
    ~ProjectTexturePreparationService();
    ProjectTexturePreparationService(const ProjectTexturePreparationService&) = delete;
    ProjectTexturePreparationService& operator=(const ProjectTexturePreparationService&) = delete;

    bool Start(const Limits& limits, std::string& error);
    void Shutdown();
    std::uint64_t Generation() const;
    void Invalidate(); // Cancels all subscribers; retained CPU results remain valid.
    void Cancel(std::uint64_t ticket);
    bool Request(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const AssetID& asset,
        ProjectTextureInterpretation interpretation, Options options, std::uint64_t generation,
        std::uint64_t& ticket, std::string& error);
    std::optional<Completion> Poll(std::uint64_t ticket, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry);
    // Must be checked again at future publication; a completion is never authorization by itself.
    bool IsCurrent(const Completion& completion) const;
    // Main thread, immediately before consumption/publication. Bounded registry,
    // path and file-ID checks only. Completion must live through acceptance.
    bool ValidateSource(const Completion& completion, const ProjectPaths& paths,
        const ProjectAssetRegistry& registry, std::string& error) const;
    size_t RetainedBytes() const; // Includes active reservations and consumer-held leases.
    struct Statistics
    {
        std::uint64_t workerHashes = 0;
        std::uint64_t decodes = 0;
        std::uint64_t cacheHits = 0;
        std::uint64_t deduplicated = 0;
    };
    Statistics GetStatistics() const;
#ifdef ENGINE_TEXTURE_PREPARATION_TESTS
    enum class TestStage { Pinned, Hashed, Prepared };
    void SetTestHook(std::function<void(TestStage)> hook);
#endif

private:
    struct State;
    std::unique_ptr<State> m_state;
};

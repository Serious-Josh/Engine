#pragma once

#include "Renderer/ProjectTextureLoader.h"
#include "assets/ProjectAssetResolver.h"

#include <atomic>
#include <memory>
#include <optional>

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
    size_t RetainedBytes() const; // Includes active reservations and consumer-held leases.

private:
    struct State;
    std::unique_ptr<State> m_state;
};

#pragma once

#include "assets/ProjectAssetResolver.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <vector>

// Editor-side memos for filesystem facts the UI shows every frame. Each filesystem round trip (a canonicalization or a status
// read) costs tens of microseconds, so the panels must not repeat them per frame for unchanged inputs. Display only: every
// operation that acts on a path or an asset still resolves and validates it itself.

// The canonical form of one path, recomputed only when the path's text changes. A directory link retargeted under an unchanged
// path is noticed once the path changes. Failures are not kept, so a failed canonicalization is retried on the next call.
class CanonicalPathCache
{
public:
    const std::filesystem::path& Resolve(const std::filesystem::path& path, std::error_code& error)
    {
        error.clear();

        if(m_resolved && m_source.native() == path.native())
        {
            return m_canonical;
        }

        m_canonical = std::filesystem::canonical(path, error);
        m_resolved = !error;
        m_source = path;
        return m_canonical;
    }

private:
    std::filesystem::path m_source;
    std::filesystem::path m_canonical;
    bool m_resolved = false;
};

// Availability of registered assets for Editor labels and previews (ProjectAssetResolver::Resolve: three canonicalizations and
// a status read, about 0.1-0.2 ms). An entry stays valid while the project root and the asset's registry record (type and path)
// are unchanged, so a rename, move or unregistration through the registry shows at once. A file changed outside the Editor
// shows within RevalidationInterval. Expired entries are resolved again at most RevalidationsPerFrame per frame, so entries
// created together do not all expire in the same frame.
class AssetAvailabilityCache
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::milliseconds RevalidationInterval{1000};
    static constexpr std::size_t RevalidationsPerFrame = 1;
    static constexpr std::size_t Capacity = 256;

    // Once per Editor frame; resets the revalidation budget.
    void BeginFrame() noexcept
    {
        m_revalidations = 0;
    }

    void Clear() noexcept
    {
        m_entries.clear();
    }

    AssetAvailability Find(const ProjectPaths& paths, const ProjectAssetRegistry& registry, const AssetID& id)
    {
        const AssetRecord* record = registry.Find(id);

        // Resolve answers an unregistered asset from the registry alone, with no filesystem access.
        if(!record)
        {
            return AssetAvailability::NotRegistered;
        }

        const auto now = Clock::now();
        auto entry = std::find_if(m_entries.begin(), m_entries.end(), [&id](const Entry& candidate)
        {
            return candidate.id == id;
        });

        const bool sameRecord = entry != m_entries.end() && entry->root.native() == paths.GetRoot().native() &&
            entry->type == record->type && entry->relativePath.native() == record->relativePath.native();

        if(sameRecord)
        {
            const bool expired = now - entry->resolvedAt >= RevalidationInterval;

            if(!expired || m_revalidations >= RevalidationsPerFrame)
            {
                return entry->availability;
            }

            ++m_revalidations;
        }

        const AssetAvailability availability = ProjectAssetResolver::Resolve(paths, registry, id).status;

        if(entry == m_entries.end())
        {
            if(m_entries.size() >= Capacity)
            {
                m_entries.clear();
            }

            m_entries.push_back({id});
            entry = m_entries.end() - 1;
        }

        entry->root = paths.GetRoot();
        entry->type = record->type;
        entry->relativePath = record->relativePath;
        entry->availability = availability;
        entry->resolvedAt = now;
        return availability;
    }

private:
    struct Entry
    {
        AssetID id;
        std::filesystem::path root;
        AssetType type = AssetType::Material;
        std::filesystem::path relativePath;
        AssetAvailability availability = AssetAvailability::NotRegistered;
        Clock::time_point resolvedAt{};
    };

    std::vector<Entry> m_entries;
    std::size_t m_revalidations = 0;
};

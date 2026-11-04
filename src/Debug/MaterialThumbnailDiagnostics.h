#pragma once

#include "Debug/MeshThumbnailDiagnostics.h"

#include <atomic>
#include <cstdint>
#include <string>

// Diagnostics only: material thumbnail work counters, the material counterpart of
// MeshThumbnailDiagnostics. Reported with the frame-hitch diagnostics and read by the thumbnail
// scheduling verification. Counting and timing compile to nothing outside diagnostics builds.
namespace MaterialThumbnailDiagnostics
{
    using MeshThumbnailDiagnostics::Add;
    using MeshThumbnailDiagnostics::Timer;

    struct Counters
    {
        // Asset Browser, main thread.
        std::atomic<std::uint64_t> browserRequests{0};
        std::atomic<std::uint64_t> browserRequestMicroseconds{0};
        std::atomic<std::uint64_t> imageLookups{0};
        std::atomic<std::uint64_t> imageLookupMicroseconds{0};
        std::atomic<std::uint64_t> admissionReleases{0};

        // Resident tiles displayed through the O(1) cache-handle fast path.
        std::atomic<std::uint64_t> residentFastLookups{0};
        std::atomic<std::uint64_t> residentFastMicroseconds{0};

        // A visible tile that already displayed its thumbnail drew a status label instead.
        std::atomic<std::uint64_t> displayDropouts{0};

        // Renderer request processing, main thread before recording.
        std::atomic<std::uint64_t> validations{0};
        std::atomic<std::uint64_t> validationMicroseconds{0};
        std::atomic<std::uint64_t> validationsUnchanged{0};
        std::atomic<std::uint64_t> generations{0};
        std::atomic<std::uint64_t> diskCacheHits{0};
        std::atomic<std::uint64_t> fullRenders{0};

        // Display images.
        std::atomic<std::uint64_t> imagesCreated{0};
        std::atomic<std::uint64_t> imagesReused{0};
    };

    inline Counters& Get()
    {
        static Counters counters;
        return counters;
    }

    struct Snapshot
    {
        std::uint64_t browserRequests = 0;
        std::uint64_t browserRequestMicroseconds = 0;
        std::uint64_t imageLookups = 0;
        std::uint64_t imageLookupMicroseconds = 0;
        std::uint64_t admissionReleases = 0;
        std::uint64_t residentFastLookups = 0;
        std::uint64_t residentFastMicroseconds = 0;
        std::uint64_t displayDropouts = 0;
        std::uint64_t validations = 0;
        std::uint64_t validationMicroseconds = 0;
        std::uint64_t validationsUnchanged = 0;
        std::uint64_t generations = 0;
        std::uint64_t diskCacheHits = 0;
        std::uint64_t fullRenders = 0;
        std::uint64_t imagesCreated = 0;
        std::uint64_t imagesReused = 0;
    };

    inline Snapshot Capture()
    {
        const auto& counters = Get();
        const auto read = [](const std::atomic<std::uint64_t>& value)
        {
            return value.load(std::memory_order_relaxed);
        };

        Snapshot snapshot;
        snapshot.browserRequests = read(counters.browserRequests);
        snapshot.browserRequestMicroseconds = read(counters.browserRequestMicroseconds);
        snapshot.imageLookups = read(counters.imageLookups);
        snapshot.imageLookupMicroseconds = read(counters.imageLookupMicroseconds);
        snapshot.admissionReleases = read(counters.admissionReleases);
        snapshot.residentFastLookups = read(counters.residentFastLookups);
        snapshot.residentFastMicroseconds = read(counters.residentFastMicroseconds);
        snapshot.displayDropouts = read(counters.displayDropouts);
        snapshot.validations = read(counters.validations);
        snapshot.validationMicroseconds = read(counters.validationMicroseconds);
        snapshot.validationsUnchanged = read(counters.validationsUnchanged);
        snapshot.generations = read(counters.generations);
        snapshot.diskCacheHits = read(counters.diskCacheHits);
        snapshot.fullRenders = read(counters.fullRenders);
        snapshot.imagesCreated = read(counters.imagesCreated);
        snapshot.imagesReused = read(counters.imagesReused);
        return snapshot;
    }

    inline std::string Describe(const Snapshot& from, const Snapshot& to)
    {
        const auto delta = [](std::uint64_t before, std::uint64_t after)
        {
            return std::to_string(after - before);
        };
        const auto milliseconds = [](std::uint64_t before, std::uint64_t after)
        {
            return std::to_string(static_cast<double>(after - before) / 1000.0);
        };

        return "browserRequests=" + delta(from.browserRequests, to.browserRequests) +
            " (" + milliseconds(from.browserRequestMicroseconds, to.browserRequestMicroseconds) + " ms)" +
            ", imageLookups=" + delta(from.imageLookups, to.imageLookups) +
            " (" + milliseconds(from.imageLookupMicroseconds, to.imageLookupMicroseconds) + " ms)" +
            ", residentFastLookups=" + delta(from.residentFastLookups, to.residentFastLookups) +
            " (" + milliseconds(from.residentFastMicroseconds, to.residentFastMicroseconds) + " ms)" +
            ", admissionReleases=" + delta(from.admissionReleases, to.admissionReleases) +
            ", displayDropouts=" + delta(from.displayDropouts, to.displayDropouts) +
            ", validations=" + delta(from.validations, to.validations) +
            " (" + milliseconds(from.validationMicroseconds, to.validationMicroseconds) + " ms)" +
            ", validationsUnchanged=" + delta(from.validationsUnchanged, to.validationsUnchanged) +
            ", generations=" + delta(from.generations, to.generations) +
            ", diskCacheHits=" + delta(from.diskCacheHits, to.diskCacheHits) +
            ", fullRenders=" + delta(from.fullRenders, to.fullRenders) +
            ", imagesCreated=" + delta(from.imagesCreated, to.imagesCreated) +
            ", imagesReused=" + delta(from.imagesReused, to.imagesReused);
    }
}

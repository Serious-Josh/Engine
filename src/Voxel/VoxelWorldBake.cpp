#include "Voxel/VoxelWorldBake.h"

#include "Voxel/VoxelBinary.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

namespace Voxel
{
namespace
{
    using Clock = std::chrono::steady_clock;

    double SecondsSince(Clock::time_point start)
    {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    class Hasher
    {
    public:
        void U64(std::uint64_t value) noexcept
        {
            for(int index = 0; index < 8; ++index)
            {
                m_hash = (m_hash ^ ((value >> (8 * index)) & 0xFF)) * 1099511628211ull;
            }
        }

        // -0.0 and 0.0 are one value, as in the snapshot's welding.
        void Double(double value) noexcept
        {
            U64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
        }

        std::uint64_t Get() const noexcept
        {
            return m_hash;
        }

    private:
        std::uint64_t m_hash = 14695981039346656037ull;
    };

    bool ValidSettings(const BakeSettings& settings) noexcept
    {
        return RegionBricksLog2(settings.regionBricks) && IsValidBaseExponent(settings.baseExponent) &&
            settings.narrowBand >= RegionFormat::MinimumNarrowBand && settings.narrowBand <= RegionFormat::MaximumNarrowBand &&
            settings.coarseClamp >= RegionFormat::MinimumCoarseClamp && settings.coarseClamp <= RegionFormat::MaximumCoarseClamp;
    }

    bool BoxesOverlap(const Point& minimumA, const Point& maximumA, const WorldBox& box) noexcept
    {
        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            if(maximumA[axis] < box.minimum[axis] || minimumA[axis] > box.maximum[axis])
            {
                return false;
            }
        }
        return true;
    }

    // Per-triangle and per-closed-piece content hashes, and a tree over closed piece bounds. Built once per plan or bake.
    struct SnapshotIndex
    {
        std::vector<std::uint64_t> triangleHashes;
        std::vector<std::uint32_t> closedPieces;
        std::vector<std::uint64_t> closedPieceHashes;
        BoxTree closedTree;

        explicit SnapshotIndex(const GeometrySnapshot& snapshot)
        {
            const auto triangles = snapshot.GetTriangles();
            const auto pieces = snapshot.GetPieces();
            triangleHashes.reserve(triangles.size());

            for(const SnapshotTriangle& triangle : triangles)
            {
                Hasher hasher;
                for(const Point& corner : triangle.corners)
                {
                    for(const double value : corner)
                    {
                        hasher.Double(value);
                    }
                }
                hasher.U64(pieces[triangle.piece].closed ? 1 : 0);
                triangleHashes.push_back(hasher.Get());
            }

            std::vector<Point> minimum;
            std::vector<Point> maximum;

            for(std::uint32_t index = 0; index < pieces.size(); ++index)
            {
                const SnapshotPiece& piece = pieces[index];
                if(!piece.closed)
                {
                    continue;
                }

                // Order-sensitive: the exact fallback sums the piece's triangles in this order.
                Hasher hasher;
                hasher.U64(piece.triangleCount);
                hasher.U64(static_cast<std::uint64_t>(static_cast<std::int64_t>(piece.windingSign)));
                for(std::uint32_t triangle = piece.firstTriangle; triangle < piece.firstTriangle + piece.triangleCount; ++triangle)
                {
                    hasher.U64(triangleHashes[triangle]);
                }

                closedPieces.push_back(index);
                closedPieceHashes.push_back(hasher.Get());
                minimum.push_back(piece.minimum);
                maximum.push_back(piece.maximum);
            }

            closedTree = BoxTree(std::move(minimum), std::move(maximum));
        }
    };

    // Regions overlapping a world box grown by the halo, with an exact test per region. Fails beyond the lattice range or the limit.
    template<typename Accept>
    std::expected<void, std::string> CollectRegions(const Point& minimum, const Point& maximum, const BakeSettings& settings, std::size_t limit,
        std::vector<RegionKey>& keys, Accept accept)
    {
        const double regionVoxels = static_cast<double>(settings.regionBricks * BrickVoxels);
        const double halo = static_cast<double>(RegionHaloBricks(settings) * BrickVoxels + 2);
        std::int64_t first[3];
        std::int64_t last[3];
        std::uint64_t count = 1;

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            const double low = std::floor((std::ldexp(minimum[axis], -settings.baseExponent) - halo) / regionVoxels) - 1.0;
            const double high = std::floor((std::ldexp(maximum[axis], -settings.baseExponent) + halo) / regionVoxels) + 1.0;

            if(!(low >= -static_cast<double>(RegionCoordinateLimit)) || !(high < static_cast<double>(RegionCoordinateLimit)))
            {
                return std::unexpected("Geometry lies outside the voxel lattice range (+/-2^20 regions).");
            }

            first[axis] = static_cast<std::int64_t>(low);
            last[axis] = static_cast<std::int64_t>(high);
            count *= static_cast<std::uint64_t>(last[axis] - first[axis] + 1);

            if(count > static_cast<std::uint64_t>(limit) * 8)
            {
                return std::unexpected("Geometry spans more than the region limit of " + std::to_string(limit) + " regions.");
            }
        }

        for(std::int64_t z = first[2]; z <= last[2]; ++z)
        {
            for(std::int64_t y = first[1]; y <= last[1]; ++y)
            {
                for(std::int64_t x = first[0]; x <= last[0]; ++x)
                {
                    const RegionKey key{0, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)};
                    if(accept(key))
                    {
                        keys.push_back(key);
                    }
                }
            }
        }

        // Keep the working list bounded while collecting.
        if(keys.size() > limit * 2)
        {
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            if(keys.size() > limit)
            {
                return std::unexpected("Geometry spans more than the region limit of " + std::to_string(limit) + " regions.");
            }
        }

        return {};
    }

    std::expected<WorldPlan, std::string> Plan(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, const SnapshotIndex& index,
        const BakeSettings& settings, std::size_t maximumRegions)
    {
        if(!ValidSettings(settings))
        {
            return std::unexpected("Voxel bake settings are out of range.");
        }

        std::vector<RegionKey> keys;
        const auto triangles = snapshot.GetTriangles();
        const auto pieces = snapshot.GetPieces();

        for(const SnapshotTriangle& triangle : triangles)
        {
            Point minimum;
            Point maximum;
            for(std::size_t axis = 0; axis < 3; ++axis)
            {
                const auto& c = triangle.corners;
                minimum[axis] = (std::min)({c[0][axis], c[1][axis], c[2][axis]});
                maximum[axis] = (std::max)({c[0][axis], c[1][axis], c[2][axis]});
            }

            auto collected = CollectRegions(minimum, maximum, settings, maximumRegions, keys,
                [&](const RegionKey& key) { return BoxesOverlap(minimum, maximum, RegionGeometryBounds(key, settings)); });
            if(!collected)
            {
                return std::unexpected(collected.error());
            }
        }

        for(const std::uint32_t pieceIndex : index.closedPieces)
        {
            const SnapshotPiece& piece = pieces[pieceIndex];
            auto collected = CollectRegions(piece.minimum, piece.maximum, settings, maximumRegions, keys,
                [&](const RegionKey& key) { return PieceOverlaps(piece, RegionClassificationBounds(key, settings)); });
            if(!collected)
            {
                return std::unexpected(collected.error());
            }
        }

        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

        if(keys.size() > maximumRegions)
        {
            return std::unexpected("Geometry spans " + std::to_string(keys.size()) + " regions, above the limit of " + std::to_string(maximumRegions) + ".");
        }

        WorldPlan plan;
        plan.regions.reserve(keys.size());
        const std::uint64_t settingsHash = HashBakeSettings(settings);
        std::vector<std::uint32_t> found;
        std::vector<std::uint64_t> hashes;

        for(const RegionKey& key : keys)
        {
            Hasher hasher;
            hasher.U64(RegionFormat::FormatVersion);
            hasher.U64(settingsHash);
            hasher.U64(key.level);
            hasher.U64(*PackRegionCoordinates(key));

            // Triangles BakeRegion reads, as a set.
            const WorldBox geometry = RegionGeometryBounds(key, settings);
            acceleration.triangles.Query(geometry.minimum, geometry.maximum, found);
            hashes.clear();
            for(const std::uint32_t triangle : found)
            {
                hashes.push_back(index.triangleHashes[triangle]);
            }
            std::sort(hashes.begin(), hashes.end());
            hasher.U64(hashes.size());
            for(const std::uint64_t hash : hashes)
            {
                hasher.U64(hash);
            }
            const bool hasGeometry = !found.empty();

            // Whole closed pieces that classify it, as a set.
            const WorldBox classification = RegionClassificationBounds(key, settings);
            index.closedTree.Query(classification.minimum, classification.maximum, found);
            hashes.clear();
            for(const std::uint32_t slot : found)
            {
                hashes.push_back(index.closedPieceHashes[slot]);
            }
            std::sort(hashes.begin(), hashes.end());
            hasher.U64(hashes.size());
            for(const std::uint64_t hash : hashes)
            {
                hasher.U64(hash);
            }

            plan.regions.push_back({key, hasher.Get(), hasGeometry});
        }

        if(!plan.regions.empty())
        {
            plan.minimumRegion = {plan.regions.front().key.x, plan.regions.front().key.y, plan.regions.front().key.z};
            plan.maximumRegion = plan.minimumRegion;
            for(const RegionPlan& region : plan.regions)
            {
                const std::int32_t coordinates[3] = {region.key.x, region.key.y, region.key.z};
                for(std::size_t axis = 0; axis < 3; ++axis)
                {
                    plan.minimumRegion[axis] = (std::min)(plan.minimumRegion[axis], coordinates[axis]);
                    plan.maximumRegion[axis] = (std::max)(plan.maximumRegion[axis], coordinates[axis]);
                }
            }
        }

        return plan;
    }

    // A region with no triangle in its geometry box has no surface: BakeRegion would find one surface-free component and classify
    // it at the centre of brick (0, 0, 0). This does exactly that, without building the brick grid.
    bool UniformRegionInside(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, const SnapshotIndex& index,
        const RegionKey& key, const BakeSettings& settings, std::vector<std::uint32_t>& scratch)
    {
        const int log2 = *RegionBricksLog2(settings.regionBricks);
        const int sizeExponent = settings.baseExponent + key.level;
        const Coordinate origin = RegionOriginVoxel(key, log2);
        const Point point{std::ldexp(static_cast<double>(origin.x + 4), sizeExponent), std::ldexp(static_cast<double>(origin.y + 4), sizeExponent),
            std::ldexp(static_cast<double>(origin.z + 4), sizeExponent)};
        const WorldBox classification = RegionClassificationBounds(key, settings);
        index.closedTree.Query(classification.minimum, classification.maximum, scratch);
        const auto pieces = snapshot.GetPieces();
        WindingCounters counters;
        long long total = 0;

        for(const std::uint32_t slot : scratch)
        {
            const std::uint32_t pieceIndex = index.closedPieces[slot];
            const SnapshotPiece& piece = pieces[pieceIndex];
            if(point[0] < piece.minimum[0] || point[0] > piece.maximum[0] || point[1] < piece.minimum[1] || point[1] > piece.maximum[1] ||
                point[2] < piece.minimum[2] || point[2] > piece.maximum[2])
            {
                continue;
            }

            total += settings.classification == ClassificationMode::Exact ? ExactPieceWinding(snapshot, piece, point)
                                                                         : acceleration.winding.PieceWinding(pieceIndex, point, counters);
        }

        return total >= 1;
    }

    struct JobResult
    {
        RegionBakeKind kind = RegionBakeKind::UniformOutside;
        std::string reference;
        std::uint32_t brickCount = 0;
        std::uint64_t byteSize = 0;
        bool reused = false;
        bool rejected = false;
        std::string note;
        // FNV-1a of the region asset's bytes; zero for uniform regions.
        std::uint64_t contentHash = 0;
    };

    // Whether a previous region asset can stand for this region unchanged; why not otherwise.
    std::string CheckReusable(AssetStore& store, const ManifestEntry& entry, const RegionPlan& plan, const BakeSettings& settings,
        std::uint64_t& contentHash)
    {
        const auto bytes = store.Read(entry.reference);
        if(!bytes)
        {
            return "missing (" + bytes.error() + ")";
        }

        if(bytes->size() != entry.byteSize)
        {
            return "size differs from the manifest";
        }

        RegionData data;
        const DecodeResult decoded = DecodeRegion(*bytes, data);
        if(!decoded)
        {
            return "does not decode (" + decoded.message + ")";
        }

        if(data.key != plan.key || data.inputHash != plan.inputHash || data.settingsHash != HashBakeSettings(settings) ||
            data.GetBrickCount() != entry.brickCount || data.baseExponent != settings.baseExponent || data.regionBricks != settings.regionBricks ||
            data.narrowBand != settings.narrowBand || data.coarseClamp != settings.coarseClamp)
        {
            return "records a different region, input or settings";
        }

        contentHash = Binary::Fnv1a(*bytes);
        return {};
    }
}

std::uint64_t PlanParticipationHash(const WorldPlan& plan) noexcept
{
    Hasher hasher;
    for(const RegionPlan& region : plan.regions)
    {
        hasher.U64(*PackRegionCoordinates(region.key));
        hasher.U64(region.inputHash);
    }
    return hasher.Get();
}

std::uint64_t ManifestContentHash(const ManifestData& manifest, std::span<const std::uint64_t> assetHashes) noexcept
{
    Hasher hasher;
    hasher.U64(manifest.settingsHash);
    for(const auto* bound : {&manifest.minimumRegion, &manifest.maximumRegion})
    {
        for(const std::int32_t value : *bound)
        {
            hasher.U64(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
        }
    }
    hasher.U64(manifest.entries.size());

    for(std::size_t index = 0; index < manifest.entries.size(); ++index)
    {
        const ManifestEntry& entry = manifest.entries[index];
        hasher.U64(entry.key.level);
        hasher.U64(*PackRegionCoordinates(entry.key));
        hasher.U64(static_cast<std::uint64_t>(entry.kind));
        hasher.U64(entry.inputHash);
        hasher.U64(index < assetHashes.size() ? assetHashes[index] : 0);
    }

    return hasher.Get();
}

std::expected<WorldPlan, std::string> PlanWorld(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, const BakeSettings& settings,
    std::size_t maximumRegions)
{
    const SnapshotIndex index(snapshot);
    return Plan(snapshot, acceleration, index, settings, maximumRegions);
}

std::size_t EstimateRegionJobBytes(const BakeSettings& settings) noexcept
{
    const std::uint64_t cells = std::uint64_t{settings.regionBricks} * settings.regionBricks * settings.regionBricks;
    const std::uint64_t worstRegion = RegionFormat::EncodedBytes(settings.regionBricks, static_cast<std::uint32_t>(cells));
    const std::uint64_t grid = static_cast<std::uint64_t>(settings.regionBricks + 2 * RegionHaloBricks(settings));
    // Region data and its encoding (or, when validating reuse, the read bytes and the decoded data), plus the baker's per-brick
    // bins and masks over the grid, plus a fixed allowance for candidates and scratch.
    return static_cast<std::size_t>(2 * worstRegion + grid * grid * grid * 256 + (std::uint64_t{8} << 20));
}

std::expected<PreparedWorldBake, WorldBakeError> PrepareWorldBake(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration,
    AssetStore& store, const WorldBakeOptions& options)
{
    const BakeSettings& settings = options.settings;
    WorldBakeResult result;
    WorldBakeStatistics& statistics = result.statistics;
    const auto note = [&statistics](std::string text) {
        if(statistics.notes.size() < 32)
        {
            statistics.notes.push_back(std::move(text));
        }
    };

    // Plan.
    const auto planStart = Clock::now();
    const SnapshotIndex index(snapshot);
    auto plan = Plan(snapshot, acceleration, index, settings, options.maximumRegions);
    if(!plan)
    {
        return std::unexpected(WorldBakeError{false, plan.error()});
    }
    const std::vector<RegionPlan>& regions = plan->regions;
    statistics.regionsPlanned = regions.size();

    if(options.regionsTotal != nullptr)
    {
        options.regionsTotal->store(regions.size());
    }

    // The previous bake, usable only when baked with the same settings.
    std::map<RegionKey, ManifestEntry> previous;
    if(options.reuse)
    {
        if(const auto current = store.GetCurrentManifest())
        {
            ManifestData manifest;
            const auto bytes = store.Read(*current);
            const DecodeResult decoded = bytes ? DecodeManifest(*bytes, manifest) : DecodeResult{DecodeError::NotVoxelFormat, bytes.error()};

            if(!decoded)
            {
                note("Previous manifest unusable, rebaking everything: " + decoded.message);
            }
            else if(manifest.settingsHash != HashBakeSettings(settings) || manifest.baseExponent != settings.baseExponent ||
                manifest.regionBricks != settings.regionBricks || manifest.narrowBand != settings.narrowBand || manifest.coarseClamp != settings.coarseClamp)
            {
                note("Previous manifest was baked with different settings, rebaking everything.");
            }
            else
            {
                for(ManifestEntry& entry : manifest.entries)
                {
                    previous.emplace(entry.key, std::move(entry));
                }
            }
        }
    }
    statistics.planSeconds = SecondsSince(planStart);

    // Bake or reuse each region on bounded workers.
    const auto bakeStart = Clock::now();
    const std::size_t memoryJobs = (std::max)(std::size_t{1}, options.memoryBudget / EstimateRegionJobBytes(settings));
    const unsigned jobs = static_cast<unsigned>((std::max)(std::size_t{1},
        (std::min)({static_cast<std::size_t>((std::max)(1u, options.threads)), memoryJobs, (std::max)(std::size_t{1}, regions.size())})));
    statistics.concurrentJobs = jobs;

    std::vector<JobResult> results(regions.size());
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    std::string failure;
    std::vector<std::string> staged;

    const auto cancelled = [&options] { return options.cancel != nullptr && options.cancel->load(); };
    const auto fail = [&](std::string message) {
        const std::lock_guard lock(mutex);
        if(!failed.exchange(true))
        {
            failure = std::move(message);
        }
    };

    const auto worker = [&] {
        std::vector<std::uint32_t> scratch;

        while(!failed.load() && !cancelled())
        {
            const std::size_t at = next.fetch_add(1);
            if(at >= regions.size())
            {
                return;
            }

            const RegionPlan& region = regions[at];
            JobResult& job = results[at];

            // Counts the region as done however this iteration ends.
            struct Done
            {
                std::atomic<std::size_t>* counter;
                ~Done()
                {
                    if(counter != nullptr)
                    {
                        counter->fetch_add(1);
                    }
                }
            } done{options.regionsDone};

            try
            {
                const auto entry = previous.find(region.key);
                if(entry != previous.end() && entry->second.inputHash == region.inputHash)
                {
                    if(entry->second.kind == RegionKind::UniformInside)
                    {
                        job.kind = RegionBakeKind::UniformInside;
                        job.reused = true;
                        continue;
                    }

                    if(const std::string why = CheckReusable(store, entry->second, region, settings, job.contentHash); why.empty())
                    {
                        job.kind = RegionBakeKind::Populated;
                        job.reference = entry->second.reference;
                        job.brickCount = entry->second.brickCount;
                        job.byteSize = entry->second.byteSize;
                        job.reused = true;
                        continue;
                    }
                    else
                    {
                        job.rejected = true;
                        job.note = "Region (" + std::to_string(region.key.x) + ", " + std::to_string(region.key.y) + ", " +
                            std::to_string(region.key.z) + ") previous asset " + why + "; rebaked.";
                    }
                }

                if(!region.hasGeometry)
                {
                    job.kind = UniformRegionInside(snapshot, acceleration, index, region.key, settings, scratch) ? RegionBakeKind::UniformInside
                                                                                                                : RegionBakeKind::UniformOutside;
                    continue;
                }

                auto baked = BakeRegion(snapshot, acceleration, region.key, settings);
                if(!baked)
                {
                    fail("Region (" + std::to_string(region.key.x) + ", " + std::to_string(region.key.y) + ", " + std::to_string(region.key.z) +
                        "): " + baked.error());
                    return;
                }

                job.kind = baked->kind;
                if(baked->kind != RegionBakeKind::Populated)
                {
                    continue;
                }

                baked->data.inputHash = region.inputHash;
                const std::vector<std::byte> bytes = EncodeRegion(baked->data);

                if(bytes.size() > AssetStore::MaximumAssetBytes)
                {
                    fail("Region (" + std::to_string(region.key.x) + ", " + std::to_string(region.key.y) + ", " + std::to_string(region.key.z) +
                        ") encodes to " + std::to_string(bytes.size()) + " bytes, above the asset limit.");
                    return;
                }

                auto reference = store.Stage(bytes);
                if(!reference)
                {
                    fail("Staging a region failed: " + reference.error());
                    return;
                }

                {
                    const std::lock_guard lock(mutex);
                    staged.push_back(*reference);
                }

                job.reference = *reference;
                job.brickCount = baked->data.GetBrickCount();
                job.byteSize = bytes.size();
                job.contentHash = Binary::Fnv1a(bytes);
            }
            catch(const std::exception& exception)
            {
                fail(std::string("Region bake threw: ") + exception.what());
                return;
            }
        }
    };

    if(jobs == 1)
    {
        worker();
    }
    else
    {
        std::vector<std::thread> threads;
        for(unsigned thread = 0; thread < jobs; ++thread)
        {
            threads.emplace_back(worker);
        }
        for(std::thread& thread : threads)
        {
            thread.join();
        }
    }
    statistics.bakeSeconds = SecondsSince(bakeStart);

    const auto abandon = [&](bool wasCancelled, std::string message) {
        store.Discard(staged);
        return std::unexpected(WorldBakeError{wasCancelled, std::move(message)});
    };

    if(failed.load())
    {
        return abandon(false, failure);
    }

    if(cancelled())
    {
        return abandon(true, "The voxel bake was cancelled; nothing was published.");
    }

    // Assemble and publish the manifest.
    const auto publishStart = Clock::now();
    ManifestData& manifest = result.manifest;
    manifest.baseExponent = settings.baseExponent;
    manifest.regionBricks = settings.regionBricks;
    manifest.narrowBand = settings.narrowBand;
    manifest.coarseClamp = settings.coarseClamp;
    manifest.settingsHash = HashBakeSettings(settings);
    manifest.minimumRegion = plan->minimumRegion;
    manifest.maximumRegion = plan->maximumRegion;
    std::vector<std::uint64_t> assetHashes;

    for(std::size_t at = 0; at < regions.size(); ++at)
    {
        const RegionPlan& region = regions[at];
        const JobResult& job = results[at];
        statistics.reused += job.reused;
        statistics.cacheRejected += job.rejected;
        statistics.baked += !job.reused && job.kind == RegionBakeKind::Populated;

        if(!job.note.empty())
        {
            note(job.note);
        }

        switch(job.kind)
        {
        case RegionBakeKind::Populated:
            ++statistics.populated;
            statistics.stagedBytes += job.reused ? 0 : job.byteSize;
            manifest.entries.push_back({region.key, RegionKind::Populated, region.inputHash, job.brickCount, job.byteSize, job.reference});
            assetHashes.push_back(job.contentHash);
            break;
        case RegionBakeKind::UniformInside:
            ++statistics.uniformInside;
            manifest.entries.push_back({region.key, RegionKind::UniformInside, region.inputHash, 0, 0, {}});
            assetHashes.push_back(0);
            break;
        case RegionBakeKind::UniformOutside:
            ++statistics.uniformOutside;
            break;
        }
    }

    manifest.participationHash = PlanParticipationHash(*plan);
    result.contentHash = ManifestContentHash(manifest, assetHashes);

    std::vector<std::byte> manifestBytes;
    try
    {
        manifestBytes = EncodeManifest(manifest);
    }
    catch(const std::exception& exception)
    {
        return abandon(false, std::string("Encoding the manifest failed: ") + exception.what());
    }

    auto manifestReference = store.Stage(manifestBytes);
    if(!manifestReference)
    {
        return abandon(false, "Staging the manifest failed: " + manifestReference.error());
    }
    staged.push_back(*manifestReference);

    if(cancelled())
    {
        return abandon(true, "The voxel bake was cancelled; nothing was published.");
    }

    result.manifestReference = *manifestReference;
    statistics.publishSeconds = SecondsSince(publishStart);
    return PreparedWorldBake{std::move(result), std::move(staged)};
}

std::expected<WorldBakeResult, WorldBakeError> PublishWorldBake(AssetStore& store, PreparedWorldBake prepared)
{
    const auto start = Clock::now();
    const std::vector<std::string> regionAssets(prepared.staged.begin(), prepared.staged.end() - 1);

    if(auto published = store.Publish(regionAssets, prepared.result.manifestReference); !published)
    {
        store.Discard(prepared.staged);
        return std::unexpected(WorldBakeError{false, "Publishing failed: " + published.error()});
    }

    prepared.result.statistics.publishSeconds += SecondsSince(start);
    return std::move(prepared.result);
}

void AbandonWorldBake(AssetStore& store, const PreparedWorldBake& prepared) noexcept
{
    store.Discard(prepared.staged);
}

std::expected<WorldBakeResult, WorldBakeError> BakeWorld(const GeometrySnapshot& snapshot, const BakeAcceleration& acceleration, AssetStore& store,
    const WorldBakeOptions& options)
{
    auto prepared = PrepareWorldBake(snapshot, acceleration, store, options);

    if(!prepared)
    {
        return std::unexpected(prepared.error());
    }

    return PublishWorldBake(store, std::move(*prepared));
}
}

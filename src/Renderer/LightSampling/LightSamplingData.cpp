#include "Renderer/LightSampling/LightSamplingData.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

namespace
{
    constexpr double LuminanceRed = 0.2126;
    constexpr double LuminanceGreen = 0.7152;
    constexpr double LuminanceBlue = 0.0722;
    constexpr std::uint32_t CellWords = 3;
    constexpr std::uint32_t AliasWords = 4;
    constexpr double TwoPow32 = 4294967296.0;
    // Cell-list and alias offsets are byte offsets in a raw buffer, read with 32-bit addresses.
    constexpr std::uint64_t MaximumBufferBytes = 0xFFFFFFF0ull;

    using Category = LightSamplingCategory;

    // A finite light's range sphere, widened by the float-precision guard.
    struct Sphere
    {
        std::array<double, 3> center{};
        double radius = 0.0;
    };

    struct Grid
    {
        std::array<double, 3> origin{};
        std::array<std::int64_t, 3> dimensions{};
        double cellSize = 1.0;
        // Padded coverage beyond the nominal box (the lookup-jitter margin).
        double paddedMargin = 0.0;
        std::uint64_t cellCount = 0;
    };

    bool IsFinite(const float* values, int count)
    {
        for(int index = 0; index < count; ++index)
        {
            if(!std::isfinite(values[index]))
            {
                return false;
            }
        }

        return true;
    }

    // The grid cells (inclusive index ranges) whose box widened by `reach` can be closer than the sphere's radius.
    bool CellRange(const Grid& grid, const Sphere& sphere, double reach, std::array<std::int64_t, 3>& low,
        std::array<std::int64_t, 3>& high)
    {
        for(int axis = 0; axis < 3; ++axis)
        {
            const double minimum = (sphere.center[axis] - sphere.radius - reach - grid.origin[axis]) / grid.cellSize;
            const double maximum = (sphere.center[axis] + sphere.radius + reach - grid.origin[axis]) / grid.cellSize;
            low[axis] = (std::max)(std::int64_t{0}, static_cast<std::int64_t>(std::floor(minimum)));
            high[axis] = (std::min)(grid.dimensions[axis] - 1, static_cast<std::int64_t>(std::floor(maximum)));

            if(high[axis] < low[axis])
            {
                return false;
            }
        }

        return true;
    }

    // Per-axis squared distances from a sphere's centre to each cell slab in its range, nominal and padded. The squared
    // distance to a box is the sum of its axes' terms ((x + y) + z), so a cell test is two additions. Sized once for the
    // grid (one per worker) and written through raw pointers: no container operation runs per light, which keeps the
    // parallel passes free of the debug runtime's iterator bookkeeping.
    struct AxisDistances
    {
        explicit AxisDistances(const Grid& grid)
        {
            for(int axis = 0; axis < 3; ++axis)
            {
                const std::size_t count = static_cast<std::size_t>((std::max)(grid.dimensions[axis], std::int64_t{1}));
                nominal[axis].resize(count);
                padded[axis].resize(count);
            }
        }

        std::array<std::vector<double>, 3> nominal;
        std::array<std::vector<double>, 3> padded;
    };

    void ComputeAxisDistances(const Grid& grid, const Sphere& sphere, const std::array<std::int64_t, 3>& low,
        const std::array<std::int64_t, 3>& high, AxisDistances& distances)
    {
        for(int axis = 0; axis < 3; ++axis)
        {
            const std::int64_t count = high[axis] - low[axis] + 1;
            double* const nominalTerms = distances.nominal[axis].data();
            double* const paddedTerms = distances.padded[axis].data();
            const double centre = sphere.center[axis];

            for(std::int64_t index = 0; index < count; ++index)
            {
                const double boxLow = grid.origin[axis] + static_cast<double>(low[axis] + index) * grid.cellSize;
                const double boxHigh = boxLow + grid.cellSize;
                const double nominal = centre - std::clamp(centre, boxLow, boxHigh);
                const double padded = centre - std::clamp(centre, boxLow - grid.paddedMargin, boxHigh + grid.paddedMargin);
                nominalTerms[index] = nominal * nominal;
                paddedTerms[index] = padded * padded;
            }
        }
    }

    // Calls visit(linearCell, nominal) for every cell the sphere reaches through the cell's padded box; `nominal` says
    // whether it also reaches the nominal box. Only cells in z slices [sliceBegin, sliceEnd) are visited (a contiguous
    // range of linear cells). The order is fixed (x fastest), so builds are deterministic.
    template<typename Visit>
    void ForEachReachedCell(const Grid& grid, const Sphere& sphere, AxisDistances& distances, std::int64_t sliceBegin,
        std::int64_t sliceEnd, Visit&& visit)
    {
        std::array<std::int64_t, 3> low{};
        std::array<std::int64_t, 3> high{};

        if(!CellRange(grid, sphere, grid.paddedMargin, low, high))
        {
            return;
        }

        low[2] = (std::max)(low[2], sliceBegin);
        high[2] = (std::min)(high[2], sliceEnd - 1);

        if(high[2] < low[2])
        {
            return;
        }

        ComputeAxisDistances(grid, sphere, low, high, distances);
        const double radiusSquared = sphere.radius * sphere.radius;
        const std::uint64_t rowStride = static_cast<std::uint64_t>(grid.dimensions[0]);
        const std::uint64_t sliceStride = rowStride * static_cast<std::uint64_t>(grid.dimensions[1]);
        const double* xNominal = distances.nominal[0].data();
        const double* yNominal = distances.nominal[1].data();
        const double* zNominal = distances.nominal[2].data();
        const double* xPadded = distances.padded[0].data();
        const double* yPadded = distances.padded[1].data();
        const double* zPadded = distances.padded[2].data();

        for(std::int64_t z = low[2]; z <= high[2]; ++z)
        {
            const std::size_t zIndex = static_cast<std::size_t>(z - low[2]);

            if(!(zPadded[zIndex] < radiusSquared))
            {
                continue;
            }

            for(std::int64_t y = low[1]; y <= high[1]; ++y)
            {
                const std::size_t yIndex = static_cast<std::size_t>(y - low[1]);

                // Adding non-negative terms never decreases a sum, so a row whose y and z terms already reach the radius
                // holds no cell.
                if(!(yPadded[yIndex] + zPadded[zIndex] < radiusSquared))
                {
                    continue;
                }

                const std::uint64_t rowBase = static_cast<std::uint64_t>(y) * rowStride +
                    static_cast<std::uint64_t>(z) * sliceStride;

                for(std::int64_t x = low[0]; x <= high[0]; ++x)
                {
                    const std::size_t xIndex = static_cast<std::size_t>(x - low[0]);
                    const double padded = (xPadded[xIndex] + yPadded[yIndex]) + zPadded[zIndex];

                    if(!(padded < radiusSquared))
                    {
                        continue;
                    }

                    const double nominal = (xNominal[xIndex] + yNominal[yIndex]) + zNominal[zIndex];
                    visit(rowBase + static_cast<std::uint64_t>(x), nominal < radiusSquared);
                }
            }
        }
    }

    // z-slab ranges for parallel traversal: each slab owns its cells (a contiguous range of linear cells), so slabs never
    // write the same counter or list, and every slab visits lights in index order (deterministic lists). A light is
    // visited only by the slabs its z range overlaps.
    std::vector<std::pair<std::int64_t, std::int64_t>> SliceChunks(const Grid& grid)
    {
        const std::int64_t slices = grid.dimensions[2];
        const std::int64_t workers = (std::max)(1u, (std::min)(std::thread::hardware_concurrency(), 16u));
        const std::int64_t chunks = (std::max)(std::int64_t{1}, (std::min)(slices, workers * 2));
        std::vector<std::pair<std::int64_t, std::int64_t>> ranges;

        for(std::int64_t chunk = 0; chunk < chunks; ++chunk)
        {
            ranges.emplace_back(slices * chunk / chunks, slices * (chunk + 1) / chunks);
        }

        return ranges;
    }

    // The grid of `cellSize` over the given bounds. The origin snaps to the cell lattice (a multiple of the cell size), so
    // moving one light does not shift every cell. Values pass through float, exactly as the shader receives them.
    Grid MakeGrid(const std::array<double, 3>& boundsLow, const std::array<double, 3>& boundsHigh, double cellSize,
        double jitterMargin)
    {
        Grid grid;
        grid.cellSize = static_cast<double>(static_cast<float>(cellSize));
        grid.paddedMargin = static_cast<double>(static_cast<float>(jitterMargin * grid.cellSize));
        grid.cellCount = 1;

        for(int axis = 0; axis < 3; ++axis)
        {
            const double snapped = std::floor(boundsLow[axis] / grid.cellSize) * grid.cellSize;
            grid.origin[axis] = static_cast<double>(static_cast<float>(snapped));

            // float rounding of a large origin may move it up; step back until the bounds start inside the grid.
            while(grid.origin[axis] > boundsLow[axis])
            {
                grid.origin[axis] = static_cast<double>(static_cast<float>(grid.origin[axis] - grid.cellSize));
            }

            grid.dimensions[axis] = static_cast<std::int64_t>(std::floor((boundsHigh[axis] - grid.origin[axis]) /
                grid.cellSize)) + 1;
            grid.cellCount *= static_cast<std::uint64_t>(grid.dimensions[axis]);
        }

        return grid;
    }

    // Upper bound of the cells a sphere's padded reach can touch (its axis-aligned box of cells).
    std::uint64_t EstimatedCells(const Grid& grid, const Sphere& sphere)
    {
        std::array<std::int64_t, 3> low{};
        std::array<std::int64_t, 3> high{};

        if(!CellRange(grid, sphere, grid.paddedMargin, low, high))
        {
            return 0;
        }

        return static_cast<std::uint64_t>(high[0] - low[0] + 1) * static_cast<std::uint64_t>(high[1] - low[1] + 1) *
            static_cast<std::uint64_t>(high[2] - low[2] + 1);
    }

    // Vose's alias table over `weights` with a defensive uniform share, stored as {threshold, alias entry, pdf, light}.
    // The stored pdf is the probability the GPU procedure realizes for the entry's light: bin = mulhi(r0, n), then the bin
    // if r1 < threshold, else its alias, for uniform uint32 r0 and r1.
    void WriteAliasTable(const std::vector<std::uint32_t>& lights, const std::vector<double>& weights,
        double defensiveShare, std::uint32_t* destination)
    {
        const std::size_t count = lights.size();

        if(count == 0)
        {
            return;
        }

        const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
        std::vector<double> probability(count);

        for(std::size_t entry = 0; entry < count; ++entry)
        {
            probability[entry] = (total > 0.0 && std::isfinite(total)) ?
                (1.0 - defensiveShare) * weights[entry] / total + defensiveShare / static_cast<double>(count) :
                1.0 / static_cast<double>(count);
        }

        std::vector<double> scaled(count);
        std::vector<double> threshold(count, 1.0);
        std::vector<std::uint32_t> alias(count);
        std::vector<std::size_t> underfull;
        std::vector<std::size_t> overfull;

        for(std::size_t entry = 0; entry < count; ++entry)
        {
            scaled[entry] = probability[entry] * static_cast<double>(count);
            alias[entry] = static_cast<std::uint32_t>(entry);
            (scaled[entry] < 1.0 ? underfull : overfull).push_back(entry);
        }

        while(!underfull.empty() && !overfull.empty())
        {
            const std::size_t low = underfull.back();
            underfull.pop_back();
            const std::size_t high = overfull.back();
            overfull.pop_back();
            threshold[low] = scaled[low];
            alias[low] = static_cast<std::uint32_t>(high);
            scaled[high] = (scaled[high] + scaled[low]) - 1.0;
            (scaled[high] < 1.0 ? underfull : overfull).push_back(high);
        }

        // Leftovers hold (up to rounding) a whole bin of their own.
        for(const std::size_t entry : underfull)
        {
            threshold[entry] = 1.0;
            alias[entry] = static_cast<std::uint32_t>(entry);
        }

        for(const std::size_t entry : overfull)
        {
            threshold[entry] = 1.0;
            alias[entry] = static_cast<std::uint32_t>(entry);
        }

        std::vector<std::uint32_t> thresholdBits(count);
        std::vector<double> realized(count, 0.0);

        for(std::size_t entry = 0; entry < count; ++entry)
        {
            const bool self = alias[entry] == entry;
            const double rounded = std::round(threshold[entry] * TwoPow32);
            thresholdBits[entry] = self ? 0xFFFFFFFFu :
                static_cast<std::uint32_t>(std::clamp(rounded, 0.0, TwoPow32 - 1.0));

            // Bin mass: the uint32 values r0 with mulhi(r0, n) == entry, counted exactly.
            const std::uint64_t first = (static_cast<std::uint64_t>(entry) << 32) / count +
                (((static_cast<std::uint64_t>(entry) << 32) % count) != 0 ? 1 : 0);
            const std::uint64_t next = ((static_cast<std::uint64_t>(entry) + 1) << 32) / count +
                ((((static_cast<std::uint64_t>(entry) + 1) << 32) % count) != 0 ? 1 : 0);
            const double binMass = static_cast<double>(next - first) / TwoPow32;

            if(self)
            {
                realized[entry] += binMass;
                continue;
            }

            const double accept = static_cast<double>(thresholdBits[entry]) / TwoPow32;
            realized[entry] += binMass * accept;
            realized[alias[entry]] += binMass * (1.0 - accept);
        }

        for(std::size_t entry = 0; entry < count; ++entry)
        {
            const float pdf = static_cast<float>(realized[entry]);
            std::uint32_t pdfBits = 0;
            std::memcpy(&pdfBits, &pdf, sizeof(pdfBits));
            destination[entry * AliasWords + 0] = thresholdBits[entry];
            destination[entry * AliasWords + 1] = alias[entry];
            destination[entry * AliasWords + 2] = pdfBits;
            destination[entry * AliasWords + 3] = lights[entry];
        }
    }

    // M4.1.1: the alias table, followed by `reserved` zeroed entries an incremental update may fill.
    void AppendAliasTable(const std::vector<std::uint32_t>& lights, const std::vector<double>& weights,
        double defensiveShare, std::size_t reserved, std::vector<std::uint32_t>& words)
    {
        const std::size_t start = words.size();
        words.resize(start + (lights.size() + reserved) * AliasWords, 0u);
        WriteAliasTable(lights, weights, defensiveShare, words.data() + start);
    }

    Grid GridOf(const LightSamplingIncrementalState& state)
    {
        Grid grid;

        for(int axis = 0; axis < 3; ++axis)
        {
            grid.origin[axis] = state.gridOrigin[axis];
            grid.dimensions[axis] = state.gridDimensions[axis];
        }

        grid.cellSize = state.cellSize;
        grid.paddedMargin = state.paddedMargin;
        grid.cellCount = state.cellCount;
        return grid;
    }

    void Fail(std::string* error, std::string message)
    {
        if(error)
        {
            *error = std::move(message);
        }
    }
}

double LightSamplingEmissionSolidAngle(const GPULight& light) noexcept
{
    static const bool intensityOnly = []
    {
        const char* value = std::getenv("ENGINE_LIGHT_SAMPLING_POWER");
        return value != nullptr && std::string(value) == "intensity";
    }();
    constexpr double Pi = 3.14159265358979323846;

    if(intensityOnly || light.type == 2u)
    {
        return 1.0;
    }

    if(light.type != 1u)
    {
        return 4.0 * Pi;
    }

    // Photometry::SpotEmissionSolidAngle over the uploaded cosines (SceneLights.hlsli's profile: full inside the inner
    // cone, ((cos - cosOuter) / width)^2 in the transition).
    const double cosOuter = light.cosOuterCone;
    const double cosInner = light.cosInnerCone;
    const double width = (std::max)(cosInner - cosOuter, 1.0e-4);
    const double full = (std::max)(1.0 - cosOuter - width, 0.0);
    return 2.0 * Pi * (full + width / 3.0);
}

double LightSamplingPower(const GPULight& light) noexcept
{
    const double power = (LuminanceRed * light.radiance[0] + LuminanceGreen * light.radiance[1] +
        LuminanceBlue * light.radiance[2]) * LightSamplingEmissionSolidAngle(light);
    return std::isfinite(power) && power > 0.0 ? power : 0.0;
}

bool BuildLightSamplingData(const SceneLightTable& table, const LightSamplingSettings& settings, LightSamplingData& data,
    std::string* error)
{
    if(table.lights.size() != table.lightCount)
    {
        Fail(error, "The light table's record count does not match its light count.");
        return false;
    }

    LightSamplingData result;
    LightSamplingStatistics& statistics = result.statistics;
    statistics.tableLights = table.lightCount;
    auto phaseStart = std::chrono::steady_clock::now();
    const auto phase = [&phaseStart](double& milliseconds)
    {
        const auto now = std::chrono::steady_clock::now();
        milliseconds = std::chrono::duration<double, std::milli>(now - phaseStart).count();
        phaseStart = now;
    };
    const std::uint32_t lightCount = table.lightCount;
    std::vector<Category> category(lightCount, Category::Excluded);
    std::vector<Sphere> spheres(lightCount);
    std::vector<double> finiteRanges;

    // Categories. A finite light contributes only strictly inside |range| (EvaluateSceneLightIncidence), so a zero range
    // contributes nothing.
    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        const GPULight& light = table.lights[index];
        const bool valid = LightSamplingPower(light) > 0.0 && IsFinite(light.position, 3) && std::isfinite(light.range);

        if(!valid)
        {
            continue;
        }

        if(light.type == LightSamplingDirectionalType)
        {
            category[index] = Category::Directional;
            continue;
        }

        if(std::fabs(light.range) > 0.0f)
        {
            category[index] = Category::Binned;
            spheres[index].center = {light.position[0], light.position[1], light.position[2]};
            spheres[index].radius = std::fabs(static_cast<double>(light.range));
            finiteRanges.push_back(spheres[index].radius);
        }
    }

    // Wide by range: a light far larger than typical would stretch the grid and every list around it.
    if(!finiteRanges.empty())
    {
        std::vector<double> sorted = finiteRanges;
        const std::size_t middle = sorted.size() / 2;
        std::nth_element(sorted.begin(), sorted.begin() + middle, sorted.end());
        const double limit = static_cast<double>(settings.wideRangeFactor) * sorted[middle];

        for(std::uint32_t index = 0; index < lightCount; ++index)
        {
            if(category[index] == Category::Binned && spheres[index].radius > limit)
            {
                category[index] = Category::Wide;
                ++statistics.wideByRange;
            }
        }
    }

    // Float-precision guard: shaders locate cells and evaluate ranges in float, so every binned sphere is widened by a
    // bound on that rounding (relative to the largest coordinate involved) and the lists stay a superset of the true
    // support near cell boundaries and range edges.
    std::array<double, 3> boundsLow{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max()};
    std::array<double, 3> boundsHigh{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::lowest()};
    double largestCoordinate = 0.0;
    bool anyBinned = false;

    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        if(category[index] != Category::Binned)
        {
            continue;
        }

        anyBinned = true;

        for(int axis = 0; axis < 3; ++axis)
        {
            largestCoordinate = (std::max)(largestCoordinate,
                std::fabs(spheres[index].center[axis]) + spheres[index].radius);
        }
    }

    const double guard = (largestCoordinate + 1.0) * std::ldexp(1.0, -19) + 1.0e-5;
    statistics.precisionGuard = guard;

    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        if(category[index] != Category::Binned)
        {
            continue;
        }

        spheres[index].radius += guard;

        for(int axis = 0; axis < 3; ++axis)
        {
            boundsLow[axis] = (std::min)(boundsLow[axis], spheres[index].center[axis] - spheres[index].radius);
            boundsHigh[axis] = (std::max)(boundsHigh[axis], spheres[index].center[axis] + spheres[index].radius);
        }
    }

    Grid grid;
    std::vector<std::uint64_t> reached(lightCount, 0);

    if(anyBinned)
    {
        std::uint32_t binnedCandidates = 0;

        for(const Category value : category)
        {
            binnedCandidates += value == Category::Binned ? 1u : 0u;
        }

        // Automatic size: the smallest power of two (finer cells give shorter, better-targeted lists) whose cells fit, whose
        // estimated references fit the budget and that leaves few lights over the per-light limit. The estimate counts each
        // light's box of cells, an upper bound of its sphere's cells.
        double cellSize = settings.cellSize;

        if(!(cellSize > 0.0))
        {
            cellSize = 1024.0;

            for(int exponent = -3; exponent <= 10; ++exponent)
            {
                const double candidate = std::ldexp(1.0, exponent);
                const Grid trial = MakeGrid(boundsLow, boundsHigh, candidate, settings.lookupJitterMargin);

                if(trial.cellCount > settings.maximumCells)
                {
                    continue;
                }

                std::uint64_t references = 0;
                std::uint32_t overLimit = 0;

                for(std::uint32_t index = 0; index < lightCount; ++index)
                {
                    if(category[index] != Category::Binned)
                    {
                        continue;
                    }

                    const std::uint64_t cells = EstimatedCells(trial, spheres[index]);

                    if(cells > settings.perLightCellLimit)
                    {
                        ++overLimit;
                    }
                    else
                    {
                        references += cells;
                    }
                }

                if(references <= settings.referenceBudget &&
                    overLimit <= static_cast<std::uint32_t>(settings.maximumWideFraction * binnedCandidates))
                {
                    cellSize = candidate;
                    break;
                }
            }
        }

        grid = MakeGrid(boundsLow, boundsHigh, cellSize, settings.lookupJitterMargin);

        if(grid.cellCount > 0xFFFFFFFFull / CellWords)
        {
            Fail(error, "The light-sampling grid would need " + std::to_string(grid.cellCount) + " cells.");
            return false;
        }

        // The per-light limit applies to the light's box of cells (an upper bound of its sphere's cells), so a huge light is
        // classified without enumerating its cells.
        for(std::uint32_t index = 0; index < lightCount; ++index)
        {
            if(category[index] == Category::Binned && EstimatedCells(grid, spheres[index]) > settings.perLightCellLimit)
            {
                category[index] = Category::Wide;
                ++statistics.wideByCellLimit;
            }
        }
    }

    // Cell lists: count (once), demote over the budget, offsets, then fill (nominal members first, then padded-only
    // members; light order within each).
    phase(statistics.classifyMilliseconds);
    const std::uint64_t cellCount = anyBinned ? grid.cellCount : 0;
    std::vector<std::uint32_t> nominalCount(cellCount, 0);
    std::vector<std::uint32_t> paddedCount(cellCount, 0);
    AxisDistances distances(grid);
    std::uint64_t references = 0;
    const std::vector<std::pair<std::int64_t, std::int64_t>> chunks = anyBinned ? SliceChunks(grid) :
        std::vector<std::pair<std::int64_t, std::int64_t>>{};
    std::uint32_t* const nominalCounts = nominalCount.data();
    std::uint32_t* const paddedCounts = paddedCount.data();
    const Category* const categories = category.data();
    const Sphere* const sphereData = spheres.data();
    std::uint64_t* const reachedData = reached.data();

    std::for_each(std::execution::par, chunks.begin(), chunks.end(),
        [&](const std::pair<std::int64_t, std::int64_t>& chunk)
        {
            AxisDistances local(grid);

            for(std::uint32_t index = 0; index < lightCount; ++index)
            {
                if(categories[index] != Category::Binned)
                {
                    continue;
                }

                std::uint32_t reachedCells = 0;
                ForEachReachedCell(grid, sphereData[index], local, chunk.first, chunk.second,
                    [&](std::uint64_t cell, bool nominal)
                    {
                        ++(nominal ? nominalCounts : paddedCounts)[cell];
                        ++reachedCells;
                    });

                if(reachedCells != 0)
                {
                    std::atomic_ref<std::uint64_t>(reachedData[index]).fetch_add(reachedCells, std::memory_order_relaxed);
                }
            }
        });

    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        references += reached[index];
    }

    // The reference budget: demote the lights reaching the most cells first (lower index first among equals), never
    // truncate a list.
    if(references > settings.referenceBudget)
    {
        std::vector<std::uint32_t> order;

        for(std::uint32_t index = 0; index < lightCount; ++index)
        {
            if(category[index] == Category::Binned)
            {
                order.push_back(index);
            }
        }

        std::stable_sort(order.begin(), order.end(),
            [&](std::uint32_t left, std::uint32_t right) { return reached[left] > reached[right]; });

        for(const std::uint32_t index : order)
        {
            if(references <= settings.referenceBudget)
            {
                break;
            }

            category[index] = Category::Wide;
            references -= reached[index];
            ++statistics.wideByBudget;
            ForEachReachedCell(grid, spheres[index], distances, 0, grid.dimensions[2],
                [&](std::uint64_t cell, bool nominal)
                {
                    --(nominal ? nominalCounts : paddedCounts)[cell];
                });
        }
    }

    phase(statistics.countMilliseconds);
    std::vector<std::uint64_t> listOffset(cellCount, 0);
    std::uint64_t totalReferences = 0;

    for(std::uint64_t cell = 0; cell < cellCount; ++cell)
    {
        listOffset[cell] = totalReferences;
        totalReferences += std::uint64_t{nominalCount[cell]} + paddedCount[cell];
        statistics.nominalReferences += nominalCount[cell];
        statistics.maximumNominalOccupancy = (std::max)(statistics.maximumNominalOccupancy, nominalCount[cell]);
        statistics.maximumPaddedOccupancy = (std::max)(statistics.maximumPaddedOccupancy,
            nominalCount[cell] + paddedCount[cell]);
        statistics.nonEmptyCells += nominalCount[cell] != 0 ? 1u : 0u;
    }

    statistics.paddedReferences = totalReferences;
    statistics.cells = static_cast<std::uint32_t>(cellCount);
    statistics.meanNominalOccupancy = statistics.nonEmptyCells ?
        static_cast<double>(statistics.nominalReferences) / statistics.nonEmptyCells : 0.0;

    std::vector<std::uint32_t> wideLights;
    std::vector<double> wideWeights;
    std::vector<std::uint32_t> globalLights;
    std::vector<double> globalWeights;
    std::vector<std::uint32_t> directionalLights;

    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        switch(category[index])
        {
        case Category::Excluded:
            ++statistics.excludedLights;
            break;
        case Category::Directional:
            directionalLights.push_back(index);
            break;
        case Category::Binned:
            ++statistics.binnedLights;
            globalLights.push_back(index);
            globalWeights.push_back(LightSamplingPower(table.lights[index]));
            break;
        case Category::Wide:
        case Category::Dynamic:
            ++statistics.wideLights;
            wideLights.push_back(index);
            wideWeights.push_back(LightSamplingPower(table.lights[index]));
            globalLights.push_back(index);
            globalWeights.push_back(LightSamplingPower(table.lights[index]));
            break;
        }
    }

    statistics.directionalLights = static_cast<std::uint32_t>(directionalLights.size());
    statistics.budgetForcedDemotion = statistics.wideByBudget > 0;

    // M4.1.1: alias entries reserved for lights an incremental update adds to the wide and global pools, so adding one
    // never moves a section.
    const std::uint64_t reserve = (std::max)(std::uint64_t{settings.minimumDynamicReserve},
        static_cast<std::uint64_t>(std::ceil(settings.dynamicReserveFraction * statistics.binnedLights)));
    const std::uint64_t wideCapacity = wideLights.size() + reserve;
    const std::uint64_t globalCapacity = globalLights.size() + reserve;

    // Layout: cells, lists, wide alias, global alias, directional indices, and (M4.3) the RTXDI local-light translation
    // (all 16-byte aligned).
    const auto alignWords = [](std::uint64_t words) { return (words + 3) & ~std::uint64_t{3}; };
    const std::uint64_t cellsWords = cellCount * CellWords;
    const std::uint64_t listsStart = alignWords(cellsWords);
    const std::uint64_t wideStart = alignWords(listsStart + totalReferences);
    const std::uint64_t globalStart = wideStart + wideCapacity * AliasWords;
    const std::uint64_t directionalStart = globalStart + globalCapacity * AliasWords;
    // M4.3: per table light, its RTXDI local-light index (its rank among the table's non-directional lights, the order
    // BuildRtxdiLightSet adds them in; 0xFFFFFFFF for directional lights), read by the ReGIR presampling pass. It depends
    // only on the light count and types, which every incremental update keeps (it rejects changes to them).
    const std::uint64_t translationStart = alignWords(directionalStart + directionalLights.size());
    const std::uint64_t totalWords = (std::max)(alignWords(translationStart + lightCount), std::uint64_t{4});

    if(totalWords * 4 > MaximumBufferBytes)
    {
        Fail(error, "The light-sampling structures would need " + std::to_string(totalWords * 4) + " bytes.");
        return false;
    }

    std::vector<std::uint32_t>& words = result.words;
    words.assign(static_cast<std::size_t>(listsStart), 0u);

    for(std::uint64_t cell = 0; cell < cellCount; ++cell)
    {
        words[cell * CellWords + 0] = static_cast<std::uint32_t>(listOffset[cell]);
        words[cell * CellWords + 1] = nominalCount[cell];
        words[cell * CellWords + 2] = nominalCount[cell] + paddedCount[cell];
    }

    words.resize(static_cast<std::size_t>(listsStart + totalReferences), 0u);
    std::vector<std::uint64_t> nominalCursor = listOffset;
    std::vector<std::uint64_t> paddedCursor(cellCount);

    for(std::uint64_t cell = 0; cell < cellCount; ++cell)
    {
        paddedCursor[cell] = listOffset[cell] + nominalCount[cell];
    }

    std::uint32_t* const listWords = words.data() + listsStart;
    std::uint64_t* const nominalCursors = nominalCursor.data();
    std::uint64_t* const paddedCursors = paddedCursor.data();

    std::for_each(std::execution::par, chunks.begin(), chunks.end(),
        [&](const std::pair<std::int64_t, std::int64_t>& chunk)
        {
            AxisDistances local(grid);

            for(std::uint32_t index = 0; index < lightCount; ++index)
            {
                if(categories[index] != Category::Binned)
                {
                    continue;
                }

                ForEachReachedCell(grid, sphereData[index], local, chunk.first, chunk.second,
                    [&](std::uint64_t cell, bool nominal)
                    {
                        std::uint64_t& cursor = (nominal ? nominalCursors : paddedCursors)[cell];
                        listWords[cursor] = index;
                        ++cursor;
                    });
            }
        });

    phase(statistics.fillMilliseconds);
    words.resize(static_cast<std::size_t>(wideStart), 0u);
    AppendAliasTable(wideLights, wideWeights, settings.defensiveShare, static_cast<std::size_t>(reserve), words);
    AppendAliasTable(globalLights, globalWeights, settings.defensiveShare, static_cast<std::size_t>(reserve), words);
    words.insert(words.end(), directionalLights.begin(), directionalLights.end());
    words.resize(static_cast<std::size_t>(translationStart), 0u);
    std::uint32_t localIndex = 0;

    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        words.push_back(table.lights[index].type == LightSamplingDirectionalType ? 0xFFFFFFFFu : localIndex++);
    }

    words.resize(static_cast<std::size_t>(totalWords), 0u);

    phase(statistics.distributionMilliseconds);
    statistics.cellBytes = cellsWords * 4;
    statistics.listBytes = totalReferences * 4;
    statistics.distributionBytes = (wideCapacity + globalCapacity) * AliasWords * 4 + directionalLights.size() * 4;
    statistics.totalBytes = totalWords * 4;
    statistics.wideCapacity = static_cast<std::uint32_t>(wideCapacity);
    statistics.globalCapacity = static_cast<std::uint32_t>(globalCapacity);

    // M4.1.1: what an incremental update needs (the grid as traversed, categories, binned spheres, pool members).
    LightSamplingIncrementalState& state = result.incremental;
    state.valid = true;

    for(int axis = 0; axis < 3; ++axis)
    {
        state.gridOrigin[axis] = grid.origin[axis];
        state.gridDimensions[axis] = grid.dimensions[axis];
    }

    state.cellSize = grid.cellSize;
    state.paddedMargin = grid.paddedMargin;
    state.cellCount = cellCount;
    state.listsStartWord = listsStart;
    state.wideStartWord = wideStart;
    state.globalStartWord = globalStart;
    state.wideCapacity = static_cast<std::uint32_t>(wideCapacity);
    state.globalCapacity = static_cast<std::uint32_t>(globalCapacity);
    state.categories = category;
    state.spheres.assign(lightCount, std::array<double, 4>{});

    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        if(category[index] == Category::Binned)
        {
            state.spheres[index] = {spheres[index].center[0], spheres[index].center[1], spheres[index].center[2],
                spheres[index].radius};
        }
    }

    state.wideMembers = wideLights;
    state.globalMembers = globalLights;
    state.updateSerial = 0;
    state.lastMoved.assign(lightCount, 0);

    LightSamplingConstants& constants = result.constants;
    constants = {};

    for(int axis = 0; axis < 3; ++axis)
    {
        constants.gridOrigin[axis] = static_cast<float>(grid.origin[axis]);
        constants.gridDimensions[axis] = anyBinned ? static_cast<std::uint32_t>(grid.dimensions[axis]) : 0u;
    }

    constants.cellSize = static_cast<float>(grid.cellSize);
    constants.inverseCellSize = static_cast<float>(1.0 / grid.cellSize);
    constants.paddedMargin = static_cast<float>(grid.paddedMargin);
    constants.cellCount = static_cast<std::uint32_t>(cellCount);
    constants.cellsOffset = 0;
    constants.listsOffset = static_cast<std::uint32_t>(listsStart * 4);
    constants.wideOffset = static_cast<std::uint32_t>(wideStart * 4);
    constants.wideCount = static_cast<std::uint32_t>(wideLights.size());
    constants.globalOffset = static_cast<std::uint32_t>(globalStart * 4);
    constants.globalCount = static_cast<std::uint32_t>(globalLights.size());
    constants.directionalOffset = static_cast<std::uint32_t>(directionalStart * 4);
    constants.directionalCount = static_cast<std::uint32_t>(directionalLights.size());
    constants.tableLightCount = lightCount;
    constants.padding = static_cast<std::uint32_t>(translationStart * 4);
    data = std::move(result);
    return true;
}

bool UpdateLightSamplingIncremental(const SceneLightTable& table, const SceneLightTable& previousTable,
    const LightSamplingSettings& settings, LightSamplingData& data, std::vector<LightSamplingDirtyRange>& dirty)
{
    dirty.clear();
    LightSamplingIncrementalState& state = data.incremental;
    const std::uint32_t lightCount = table.lightCount;

    if(!state.valid || data.words.empty() || lightCount != previousTable.lightCount || table.lights.size() != lightCount ||
        previousTable.lights.size() != lightCount || state.categories.size() != lightCount ||
        data.constants.tableLightCount != lightCount)
    {
        return false;
    }

    const auto started = std::chrono::steady_clock::now();

    // Stage 1: classify every change without touching the data, so a change that needs a full build leaves it intact.
    std::vector<std::uint32_t> demoted;
    std::vector<std::uint32_t> added;
    std::vector<std::uint32_t> geometryMoved;
    bool weightsChanged = false;

    for(std::uint32_t index = 0; index < lightCount; ++index)
    {
        const GPULight& current = table.lights[index];
        const GPULight& previous = previousTable.lights[index];

        if(std::memcmp(&current, &previous, sizeof(GPULight)) == 0)
        {
            continue;
        }

        if(current.type != previous.type)
        {
            return false;
        }

        const Category category = state.categories[index];
        const bool contributes = LightSamplingPower(current) > 0.0 && IsFinite(current.position, 3) &&
            std::isfinite(current.range);

        // A directional light is enumerated, not sampled from a distribution: only a new contributor changes the list.
        if(current.type == LightSamplingDirectionalType)
        {
            if(category == Category::Excluded && contributes)
            {
                return false;
            }

            continue;
        }

        // A finite light that starts contributing joins the wide and global pools (only one with a range contributes, as
        // the builder's categories).
        if(category == Category::Excluded)
        {
            if(contributes && std::fabs(current.range) > 0.0f)
            {
                added.push_back(index);
                geometryMoved.push_back(index);
            }

            continue;
        }

        const bool radianceChanged = std::memcmp(current.radiance, previous.radiance, sizeof(current.radiance)) != 0;
        const bool geometryChanged = std::memcmp(current.position, previous.position, sizeof(current.position)) != 0 ||
            std::memcmp(&current.range, &previous.range, sizeof(current.range)) != 0;
        weightsChanged = weightsChanged || radianceChanged;

        if(geometryChanged)
        {
            geometryMoved.push_back(index);
        }

        // Lists hold binned lights by their range sphere; a wide or dynamic light is sampled from the wide pool anywhere.
        if(geometryChanged && category == Category::Binned)
        {
            demoted.push_back(index);
        }
    }

    if(state.wideMembers.size() + demoted.size() + added.size() > state.wideCapacity ||
        state.globalMembers.size() + added.size() > state.globalCapacity)
    {
        return false;
    }

    std::vector<LightSamplingDirtyRange> changed;

    // Stage 2: each demoted light leaves every list it was binned into, found by traversing its binned sphere exactly as
    // the build did. A list keeps its nominal members first: removing a nominal member moves the last nominal member into
    // its slot and the last padded member into the freed last nominal slot.
    if(!demoted.empty())
    {
        const Grid grid = GridOf(state);
        AxisDistances distances(grid);
        std::uint32_t* const cells = data.words.data();
        std::uint32_t* const lists = data.words.data() + state.listsStartWord;
        bool consistent = true;
        // Each touched cell once, with its list's length before this update, and each list word written: the touched
        // cells' records and the written words (found by sweeping only the touched lists) are dirty.
        std::vector<std::uint8_t> touched(static_cast<std::size_t>(state.cellCount), 0);
        std::vector<std::pair<std::uint64_t, std::uint32_t>> touchedCells;
        std::vector<std::uint8_t> written(static_cast<std::size_t>(state.wideStartWord - state.listsStartWord), 0);

        for(const std::uint32_t light : demoted)
        {
            Sphere sphere;
            sphere.center = {state.spheres[light][0], state.spheres[light][1], state.spheres[light][2]};
            sphere.radius = state.spheres[light][3];

            ForEachReachedCell(grid, sphere, distances, 0, grid.dimensions[2],
                [&](std::uint64_t cell, bool nominal)
                {
                    std::uint32_t* const record = cells + cell * CellWords;
                    const std::uint64_t offset = record[0];
                    const std::uint32_t nominalCount = record[1];
                    const std::uint32_t totalCount = record[2];

                    if(!touched[static_cast<std::size_t>(cell)])
                    {
                        touched[static_cast<std::size_t>(cell)] = 1;
                        touchedCells.emplace_back(cell, totalCount);
                    }

                    const std::uint64_t begin = nominal ? 0 : nominalCount;
                    const std::uint64_t end = nominal ? nominalCount : totalCount;
                    std::uint64_t position = end;

                    for(std::uint64_t entry = begin; entry < end; ++entry)
                    {
                        if(lists[offset + entry] == light)
                        {
                            position = entry;
                            break;
                        }
                    }

                    if(position == end)
                    {
                        consistent = false;
                        return;
                    }

                    const auto write = [&](std::uint64_t entry, std::uint32_t value)
                    {
                        lists[offset + entry] = value;
                        written[static_cast<std::size_t>(offset + entry)] = 1;
                    };

                    if(nominal)
                    {
                        write(position, lists[offset + nominalCount - 1]);

                        if(totalCount > nominalCount)
                        {
                            write(nominalCount - 1, lists[offset + totalCount - 1]);
                        }

                        record[1] = nominalCount - 1;
                    }
                    else
                    {
                        write(position, lists[offset + totalCount - 1]);
                    }

                    record[2] = totalCount - 1;
                });

            // A binned light missing from a list it reaches means the data does not match its state; only a full build
            // can recover, and the partial edit above must not be published.
            if(!consistent)
            {
                state.valid = false;
                return false;
            }

            state.categories[light] = Category::Dynamic;
        }

        for(const auto& [cell, originalTotal] : touchedCells)
        {
            changed.push_back(LightSamplingDirtyRange{cell * CellWords + 1, 2});
            const std::uint64_t offset = cells[cell * CellWords];

            for(std::uint64_t entry = 0; entry < originalTotal; ++entry)
            {
                if(!written[static_cast<std::size_t>(offset + entry)])
                {
                    continue;
                }

                const std::uint64_t word = state.listsStartWord + offset + entry;

                if(!changed.empty() && changed.back().firstWord + changed.back().wordCount == word)
                {
                    ++changed.back().wordCount;
                }
                else
                {
                    changed.push_back(LightSamplingDirtyRange{word, 1});
                }
            }
        }
    }

    for(const std::uint32_t light : added)
    {
        state.categories[light] = Category::Dynamic;
        state.globalMembers.push_back(light);
    }

    state.wideMembers.insert(state.wideMembers.end(), demoted.begin(), demoted.end());
    state.wideMembers.insert(state.wideMembers.end(), added.begin(), added.end());

    // Stage 3: the wide and global distributions over their (possibly grown) members, rewritten in their reserved regions.
    if(!demoted.empty() || !added.empty() || weightsChanged)
    {
        const auto rewrite = [&](const std::vector<std::uint32_t>& members, std::uint64_t startWord,
            std::uint32_t previousCount, bool global)
        {
            std::vector<double> weights(members.size());
            // World-level noise fix: the global distribution keeps its applied camera weighting.
            const auto& camera = data.cameraWeighting;

            for(std::size_t entry = 0; entry < members.size(); ++entry)
            {
                const std::uint32_t light = members[entry];
                const bool distant = global && camera.applied && light < camera.reach.size() && camera.reach[light] == 0;
                weights[entry] = LightSamplingPower(table.lights[light]) * (distant ? settings.cameraFarFactor : 1.0);
            }

            WriteAliasTable(members, weights, settings.defensiveShare, data.words.data() + startWord);
            const std::uint64_t entries = (std::max)(std::uint64_t{members.size()}, std::uint64_t{previousCount});

            if(entries != 0)
            {
                changed.push_back(LightSamplingDirtyRange{startWord, entries * AliasWords});
            }
        };

        rewrite(state.wideMembers, state.wideStartWord, data.constants.wideCount, false);

        // A demotion moves a light between the cell and wide pools; the global pool changes only with weights or members.
        if(weightsChanged || !added.empty())
        {
            rewrite(state.globalMembers, state.globalStartWord, data.constants.globalCount, true);
        }

        data.constants.wideCount = static_cast<std::uint32_t>(state.wideMembers.size());
        data.constants.globalCount = static_cast<std::uint32_t>(state.globalMembers.size());
    }

    // Statistics follow the categories.
    LightSamplingStatistics& statistics = data.statistics;
    const std::uint32_t moved = static_cast<std::uint32_t>(demoted.size() + added.size());
    statistics.binnedLights -= static_cast<std::uint32_t>(demoted.size());
    statistics.excludedLights -= static_cast<std::uint32_t>(added.size());
    statistics.wideLights += moved;
    statistics.dynamicLights += moved;
    statistics.incrementalDemotions = static_cast<std::uint32_t>(demoted.size());
    statistics.reusedLists = true;

    // M4.2C: which Dynamic lights moved in this update (the rest have settled and a full build would bin them).
    ++state.updateSerial;
    state.lastMoved.resize(lightCount, 0);

    for(const std::uint32_t light : geometryMoved)
    {
        state.lastMoved[light] = state.updateSerial;
    }

    statistics.settledDynamicLights = 0;

    for(const std::uint32_t light : state.wideMembers)
    {
        if(state.categories[light] == Category::Dynamic && state.lastMoved[light] != state.updateSerial)
        {
            ++statistics.settledDynamicLights;
        }
    }

    // Sorted, merged ranges (cell records, list slots and distributions).
    std::sort(changed.begin(), changed.end(), [](const LightSamplingDirtyRange& left, const LightSamplingDirtyRange& right)
    {
        return left.firstWord < right.firstWord;
    });

    for(const LightSamplingDirtyRange& range : changed)
    {
        if(!dirty.empty() && range.firstWord <= dirty.back().firstWord + dirty.back().wordCount)
        {
            const std::uint64_t end = (std::max)(dirty.back().firstWord + dirty.back().wordCount,
                range.firstWord + range.wordCount);
            dirty.back().wordCount = end - dirty.back().firstWord;
            continue;
        }

        dirty.push_back(range);
    }

    statistics.incrementalDirtyWords = 0;

    for(const LightSamplingDirtyRange& range : dirty)
    {
        statistics.incrementalDirtyWords += range.wordCount;
    }

    statistics.incrementalMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
        started).count();
    return true;
}

LightSamplingCameraUpdate UpdateLightSamplingCameraWeighting(const SceneLightTable& table,
    const LightSamplingSettings& settings, const LightSamplingCameraView& view, LightSamplingData& data,
    std::vector<LightSamplingDirtyRange>& dirty)
{
    LightSamplingCameraUpdate update;
    const auto started = std::chrono::steady_clock::now();
    auto& weighting = data.cameraWeighting;
    const LightSamplingIncrementalState& state = data.incremental;

    if(!state.valid || table.lights.size() != table.lightCount || state.categories.size() != table.lightCount)
    {
        return update;
    }

    std::vector<std::uint8_t> reach(table.lightCount, 1);
    bool weighted = settings.cameraWeighting;

    if(weighted)
    {
        const float* camera = view.position;
        const double cellSize = (std::max)(static_cast<double>(settings.cameraCellSize), 1.0e-3);
        std::int64_t cell[3];

        for(int axis = 0; axis < 3; ++axis)
        {
            cell[axis] = static_cast<std::int64_t>(std::floor(static_cast<double>(camera[axis]) / cellSize));
        }

        const double turnCosine = std::cos(static_cast<double>(settings.cameraTurnDegrees) * 3.14159265358979323846 / 180.0);
        const double sameDirection = static_cast<double>(view.forward[0]) * weighting.forward[0] +
            static_cast<double>(view.forward[1]) * weighting.forward[1] +
            static_cast<double>(view.forward[2]) * weighting.forward[2];
        const bool sameView = !settings.cameraFrustum || (sameDirection >= turnCosine &&
            view.verticalFov == weighting.verticalFov && view.aspectRatio == weighting.aspectRatio);

        if(weighting.applied && weighting.cellValid && std::equal(cell, cell + 3, weighting.cell) && sameView)
        {
            return update;
        }

        weighting.cellValid = true;
        std::copy(cell, cell + 3, weighting.cell);
        std::copy(view.forward, view.forward + 3, weighting.forward);
        weighting.verticalFov = view.verticalFov;
        weighting.aspectRatio = view.aspectRatio;

        // The view frustum's side planes (inward unit normals through the camera) and its far plane, from RenderView's
        // basis: a point at view coordinates (x, y, z) is inside when |y| <= z tan(fov / 2), |x| <= z tan(fov / 2) aspect.
        const bool frustum = settings.cameraFrustum && view.verticalFov > 0.0f && view.aspectRatio > 0.0f;
        const double tanVertical = std::tan(0.5 * static_cast<double>(view.verticalFov));
        const double tanHorizontal = tanVertical * static_cast<double>(view.aspectRatio);
        double planes[4][3]{};

        for(int axis = 0; axis < 3; ++axis)
        {
            planes[0][axis] = tanVertical * view.forward[axis] - view.up[axis];
            planes[1][axis] = tanVertical * view.forward[axis] + view.up[axis];
            planes[2][axis] = tanHorizontal * view.forward[axis] - view.right[axis];
            planes[3][axis] = tanHorizontal * view.forward[axis] + view.right[axis];
        }

        const double verticalLength = std::sqrt(1.0 + tanVertical * tanVertical);
        const double horizontalLength = std::sqrt(1.0 + tanHorizontal * tanHorizontal);
        const double planeLength[4] = {verticalLength, verticalLength, horizontalLength, horizontalLength};
        const double farPlane = view.farPlane > 0.0f ? static_cast<double>(view.farPlane) : 1.0e30;

        // Lights without a finite range (range attenuation off: a negative record range, or non-finite) count as reaching.
        for(const std::uint32_t light : state.globalMembers)
        {
            const GPULight& record = table.lights[light];

            if(!(record.range > 0.0f) || !std::isfinite(record.range))
            {
                continue;
            }

            const double offset[3] = {static_cast<double>(record.position[0]) - camera[0],
                static_cast<double>(record.position[1]) - camera[1], static_cast<double>(record.position[2]) - camera[2]};
            const double radius = static_cast<double>(record.range) + settings.cameraReachRadius;
            bool reaches = offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] <= radius * radius;

            // Conservative sphere-frustum test: the widened range sphere is not wholly outside any plane.
            if(!reaches && frustum)
            {
                const double depth = offset[0] * view.forward[0] + offset[1] * view.forward[1] + offset[2] * view.forward[2];
                reaches = depth >= -radius && depth <= farPlane + radius;

                for(int plane = 0; plane < 4 && reaches; ++plane)
                {
                    const double distance = (offset[0] * planes[plane][0] + offset[1] * planes[plane][1] +
                        offset[2] * planes[plane][2]) / planeLength[plane];
                    reaches = distance >= -radius;
                }
            }

            if(!reaches)
            {
                reach[light] = 0;
                ++update.farLights;
            }
        }
    }
    else if(!weighting.applied)
    {
        return update;
    }

    // Unchanged classification: nothing to rewrite (most cell crossings).
    // Without an applied weighting the distribution holds pure power: every light counts as reaching.
    for(std::uint32_t light = 0; light < table.lightCount; ++light)
    {
        const std::uint8_t before = weighting.applied && light < weighting.reach.size() ? weighting.reach[light] : 1;
        update.changedLights += reach[light] != before ? 1u : 0u;
    }

    weighting.applied = weighted;
    weighting.reach = std::move(reach);

    if(update.changedLights == 0)
    {
        update.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return update;
    }

    std::vector<double> weights(state.globalMembers.size());

    for(std::size_t entry = 0; entry < state.globalMembers.size(); ++entry)
    {
        const std::uint32_t light = state.globalMembers[entry];
        weights[entry] = LightSamplingPower(table.lights[light]) *
            (weighted && weighting.reach[light] == 0 ? settings.cameraFarFactor : 1.0);
    }

    WriteAliasTable(state.globalMembers, weights, settings.defensiveShare, data.words.data() + state.globalStartWord);
    const std::uint64_t words = std::uint64_t{state.globalMembers.size()} * AliasWords;

    if(words != 0)
    {
        // Merged into the caller's sorted ranges (an incremental update of this frame may have listed some already).
        dirty.push_back(LightSamplingDirtyRange{state.globalStartWord, words});
        std::sort(dirty.begin(), dirty.end(), [](const LightSamplingDirtyRange& left, const LightSamplingDirtyRange& right)
        {
            return left.firstWord < right.firstWord;
        });
        std::vector<LightSamplingDirtyRange> merged;

        for(const LightSamplingDirtyRange& range : dirty)
        {
            if(!merged.empty() && range.firstWord <= merged.back().firstWord + merged.back().wordCount)
            {
                const std::uint64_t end = (std::max)(merged.back().firstWord + merged.back().wordCount,
                    range.firstWord + range.wordCount);
                merged.back().wordCount = end - merged.back().firstWord;
                continue;
            }

            merged.push_back(range);
        }

        dirty = std::move(merged);
    }

    update.rewritten = true;
    update.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return update;
}

std::string DescribeLightSamplingStatistics(const LightSamplingData& data)
{
    const LightSamplingStatistics& statistics = data.statistics;
    const LightSamplingConstants& constants = data.constants;
    std::ostringstream stream;
    stream << statistics.tableLights << " lights (" << statistics.binnedLights << " binned, " << statistics.wideLights <<
        " wide [range " << statistics.wideByRange << ", cell limit " << statistics.wideByCellLimit << ", budget " <<
        statistics.wideByBudget << "], " << statistics.directionalLights << " directional, " << statistics.excludedLights <<
        " excluded); grid " << constants.gridDimensions[0] << "x" << constants.gridDimensions[1] << "x" <<
        constants.gridDimensions[2] << " cells of " << constants.cellSize << " m (margin " << constants.paddedMargin <<
        " m); references " << statistics.nominalReferences << " nominal / " << statistics.paddedReferences <<
        " padded; occupancy mean " << statistics.meanNominalOccupancy << ", max " << statistics.maximumNominalOccupancy <<
        " nominal / " << statistics.maximumPaddedOccupancy << " padded; " << statistics.totalBytes << " bytes (lists " <<
        statistics.listBytes << ", cells " << statistics.cellBytes << ", distributions " << statistics.distributionBytes <<
        ")" << (statistics.budgetForcedDemotion ? "; BUDGET FORCED DEMOTION" : "");
    return stream.str();
}

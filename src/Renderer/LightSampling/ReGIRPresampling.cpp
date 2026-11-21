#include "Renderer/LightSampling/ReGIRPresampling.h"

#include "Renderer/LightSampling/LightSamplingData.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr std::uint32_t DirectionalType = 2;
    constexpr std::uint32_t CellWords = 3;
    constexpr std::uint32_t AliasWords = 4;

    std::uint32_t MulHi(std::uint32_t a, std::uint32_t b) noexcept
    {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(a) * b) >> 32);
    }

    // LightSamplingUniformProbability: the realized probability that MulHi(random, count) returns index, in float as
    // the shader computes it.
    float UniformProbability(std::uint32_t index, std::uint32_t count) noexcept
    {
        const std::uint64_t scale = std::uint64_t{1} << 32;
        const std::uint64_t first = (std::uint64_t{index} * scale + count - 1) / count;
        const std::uint64_t next = ((std::uint64_t{index} + 1) * scale + count - 1) / count;
        return static_cast<float>(next - first) * (1.0f / 4294967296.0f);
    }

    float AsFloat(std::uint32_t bits) noexcept
    {
        float value;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // LightSamplingDrawAlias: {threshold, alias entry, realized probability, light} entries.
    void DrawAlias(const std::vector<std::uint32_t>& words, std::uint32_t tableOffset, std::uint32_t count,
        std::uint32_t random0, std::uint32_t random1, std::uint32_t& light, float& probability) noexcept
    {
        const std::size_t table = tableOffset / 4;
        std::size_t entry = table + std::size_t{MulHi(random0, count)} * AliasWords;

        if(random1 >= words[entry])
        {
            entry = table + std::size_t{words[entry + 1]} * AliasWords;
        }

        light = words[entry + 3];
        probability = AsFloat(words[entry + 2]);
    }

    float Luminance(const float rgb[3]) noexcept
    {
        return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    }

    // A wide draw for a small wide pool (each alias entry holds one member): among the m members with a positive cell
    // target t (total T), uniformly with the realized probability D = WideDefensiveThreshold / 2^32 (random2 below it),
    // otherwise in proportion to t (random1 scanned over the cumulative targets). A member's probability is the sum of
    // both paths, D U(rank, m) + (1 - D) t / T, so every member that can reach the cell keeps a positive realized
    // probability whatever the float rounding of the scan. False when no member reaches the cell.
    constexpr std::uint32_t WideDefensiveThreshold = 1u << 26;

    bool DrawWideByTarget(const LightSamplingData& data, const SceneLightTable& lights, const ReGIRCellSource& source,
        std::uint32_t random1, std::uint32_t random2, std::uint32_t& light, float& probability)
    {
        const LightSamplingConstants& constants = data.constants;
        const std::size_t entries = constants.wideOffset / 4;
        const auto memberAt = [&](std::uint32_t entry) { return data.words[entries + std::size_t{entry} * AliasWords + 3]; };
        const auto targetOf = [&](std::uint32_t member)
        {
            return member < lights.lightCount ? ReGIRCellTarget(lights.lights[member], source.regionLow, source.regionHigh,
                constants.cellSize) : 0.0f;
        };
        float total = 0.0f;
        std::uint32_t reaching = 0;

        for(std::uint32_t entry = 0; entry < source.wideCount; ++entry)
        {
            const float target = targetOf(memberAt(entry));
            total += target;
            reaching += target > 0.0f ? 1u : 0u;
        }

        if(reaching == 0 || !(total > 0.0f))
        {
            return false;
        }

        const bool uniform = random2 < WideDefensiveThreshold;
        const std::uint32_t uniformRank = MulHi(random1, reaching);
        const float threshold = static_cast<float>(random1 >> 8) * (1.0f / 16777216.0f) * total;
        float cumulative = 0.0f;
        std::uint32_t rank = 0;

        for(std::uint32_t entry = 0; entry < source.wideCount; ++entry)
        {
            const std::uint32_t member = memberAt(entry);
            const float target = targetOf(member);

            if(!(target > 0.0f))
            {
                continue;
            }

            cumulative += target;
            const bool last = rank + 1 == reaching;

            if(uniform ? rank == uniformRank : (threshold < cumulative || last))
            {
                const float defensive = static_cast<float>(WideDefensiveThreshold) * (1.0f / 4294967296.0f);
                light = member;
                probability = defensive * UniformProbability(rank, reaching) + (1.0f - defensive) * (target / total);
                return true;
            }

            ++rank;
        }

        return false;
    }
}

ReGIRWindow MakeReGIRWindow(const LightSamplingData& data, const float camera[3], const ReGIRSettings& settings)
{
    ReGIRWindow window;
    const LightSamplingConstants& constants = data.constants;
    const bool settingsValid = settings.windowCells[0] != 0 && settings.windowCells[1] != 0 &&
        settings.windowCells[2] != 0 && settings.lightsPerCell != 0 && settings.buildSamples != 0 &&
        settings.samplingJitter >= 0.0f && settings.wideFraction > 0.0f && settings.wideFraction < 1.0f &&
        static_cast<double>(settings.samplingJitter) <= 2.0 * constants.paddedMargin / constants.cellSize + 1.0e-6;

    // The CPU build (publication, which sets constants.valid, is the renderer's to check).
    if(data.words.empty() || !(constants.cellSize > 0.0f) || !settingsValid)
    {
        return window;
    }

    window.cellSize = constants.cellSize;
    window.precisionGuard = data.statistics.precisionGuard;
    window.samplingJitter = settings.samplingJitter;

    for(int axis = 0; axis < 3; ++axis)
    {
        window.gridOrigin[axis] = constants.gridOrigin[axis];
        window.gridDimensions[axis] = constants.gridDimensions[axis];
        window.cells[axis] = settings.windowCells[axis];
        const double cameraCell = std::floor((static_cast<double>(camera[axis]) - window.gridOrigin[axis]) / window.cellSize);
        window.originCell[axis] = static_cast<std::int64_t>(cameraCell) - static_cast<std::int64_t>(window.cells[axis] / 2);
        window.center[axis] = static_cast<float>(window.gridOrigin[axis] +
            (static_cast<double>(window.originCell[axis]) + 0.5 * window.cells[axis]) * window.cellSize);
    }

    window.valid = true;
    return window;
}

std::int32_t ReGIRWindowCellAt(const ReGIRWindow& window, const float position[3])
{
    // RTXDI_ReGIR_WorldPosToCellIndex, Grid mode, in float.
    const float cellSize = static_cast<float>(window.cellSize);
    std::int32_t cell[3];

    for(int axis = 0; axis < 3; ++axis)
    {
        const float origin = window.center[axis] - static_cast<float>(window.cells[axis]) * (cellSize * 0.5f);
        cell[axis] = static_cast<std::int32_t>(std::floor((position[axis] - origin) / cellSize));

        if(cell[axis] < 0 || cell[axis] >= static_cast<std::int32_t>(window.cells[axis]))
        {
            return -1;
        }
    }

    return cell[0] + (cell[1] + cell[2] * static_cast<std::int32_t>(window.cells[1])) *
        static_cast<std::int32_t>(window.cells[0]);
}

ReGIRCellSource ReGIRCellSourceAt(const LightSamplingData& data, const ReGIRWindow& window, std::uint32_t windowCell,
    const ReGIRSettings& settings)
{
    ReGIRCellSource source;
    const LightSamplingConstants& constants = data.constants;
    const std::int64_t local[3] = {windowCell % window.cells[0], (windowCell / window.cells[0]) % window.cells[1],
        windowCell / (window.cells[0] * window.cells[1])};
    // In float, exactly as the shader: margin, then base = origin + cell * size, low = base - margin,
    // high = base + size + margin. The precision guard dwarfs float rounding of these bounds, so coverage holds.
    const float cellSize = static_cast<float>(window.cellSize);
    const float margin = 0.5f * window.samplingJitter * cellSize + static_cast<float>(window.precisionGuard);
    bool inGrid = true;
    std::int64_t gridCell[3];

    for(int axis = 0; axis < 3; ++axis)
    {
        gridCell[axis] = window.originCell[axis] + local[axis];
        inGrid = inGrid && gridCell[axis] >= 0 && gridCell[axis] < window.gridDimensions[axis];
        const float base = static_cast<float>(window.gridOrigin[axis]) + static_cast<float>(gridCell[axis]) * cellSize;
        source.regionLow[axis] = base - margin;
        source.regionHigh[axis] = base + cellSize + margin;
    }

    source.wideCount = constants.wideCount;
    source.globalCount = constants.globalCount;

    if(!inGrid)
    {
        return source;
    }

    source.gridCell = gridCell[0] + window.gridDimensions[0] * (gridCell[1] + window.gridDimensions[1] * gridCell[2]);
    const std::size_t record = constants.cellsOffset / 4 + static_cast<std::size_t>(source.gridCell) * CellWords;
    source.listOffset = data.words[record];
    source.paddedCount = data.words[record + 2];
    const double threshold = std::round(static_cast<double>(settings.wideFraction) * 4294967296.0);
    source.wideThreshold = static_cast<std::uint32_t>(std::clamp(threshold, 1.0, 4294967295.0));
    return source;
}

float ReGIRCellTarget(const GPULight& light, const float regionLow[3], const float regionHigh[3], float cellSize)
{
    // ReGIRCellTarget in ReGIRPresample.hlsl, in float.
    const float luminance = Luminance(light.radiance);

    if(light.type == DirectionalType || !(luminance > 0.0f) || !std::isfinite(luminance))
    {
        return 0.0f;
    }

    float offset[3];

    for(int axis = 0; axis < 3; ++axis)
    {
        offset[axis] = light.position[axis] - std::clamp(light.position[axis], regionLow[axis], regionHigh[axis]);
    }

    const float distanceSquared = offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2];
    const float distance = std::sqrt(distanceSquared);
    const float range = std::fabs(light.range);

    if(!(distance < range))
    {
        return 0.0f;
    }

    float window = 1.0f;

    if(light.range > 0.0f)
    {
        const float ratio = distance / range;
        const float fade = std::clamp(1.0f - ratio * ratio * ratio * ratio, 0.0f, 1.0f);
        window = fade * fade;
    }

    const float halfCell = 0.5f * cellSize;
    const float target = luminance * window / (std::max)(distanceSquared, halfCell * halfCell);
    return target > 0.0f ? target : 0.0f;
}

std::uint32_t ReGIRNextRandom(std::uint32_t& state) noexcept
{
    state = state * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

std::uint32_t ReGIRSlotSeed(std::uint32_t slot, std::uint32_t frameSeed) noexcept
{
    std::uint32_t state = frameSeed ^ 0x52654749u;
    ReGIRNextRandom(state);
    state ^= slot * 0x9E3779B9u;
    ReGIRNextRandom(state);
    return state;
}

ReGIRSlot ReGIRPresampleSlot(const LightSamplingData& data, const SceneLightTable& table, const ReGIRCellSource& source,
    const ReGIRSettings& settings, std::uint32_t& random, ReGIRCandidateTrace* trace)
{
    const LightSamplingConstants& constants = data.constants;
    const bool inGrid = source.gridCell >= 0;
    const bool hasCell = inGrid && source.paddedCount != 0;
    const bool hasWide = inGrid && source.wideCount != 0;
    // The realized probability of the wide source: random0 < wideThreshold.
    const float wideShare = !hasWide ? 0.0f : (!hasCell ? 1.0f :
        static_cast<float>(source.wideThreshold) * (1.0f / 4294967296.0f));
    const float candidates = static_cast<float>(settings.buildSamples);
    ReGIRSlot slot;
    float weightSum = 0.0f;
    float selectedTarget = 0.0f;

    for(std::uint32_t sample = 0; sample < settings.buildSamples; ++sample)
    {
        const std::uint32_t random0 = ReGIRNextRandom(random);
        const std::uint32_t random1 = ReGIRNextRandom(random);
        const std::uint32_t random2 = ReGIRNextRandom(random);
        const std::uint32_t random3 = ReGIRNextRandom(random);
        std::uint32_t light = 0xFFFFFFFFu;
        float probability = 0.0f;
        std::uint32_t sourceKind = 0;

        if(!inGrid)
        {
            if(source.globalCount != 0)
            {
                DrawAlias(data.words, constants.globalOffset, source.globalCount, random1, random2, light, probability);
                sourceKind = 3;
            }
        }
        else if(hasWide && (!hasCell || random0 < source.wideThreshold))
        {
            if(source.wideCount <= settings.wideTargetLimit)
            {
                if(!DrawWideByTarget(data, table, source, random1, random2, light, probability))
                {
                    light = 0xFFFFFFFFu;
                    probability = 0.0f;
                }

                probability *= wideShare;
                sourceKind = 2;
            }
            else
            {
                DrawAlias(data.words, constants.wideOffset, source.wideCount, random1, random2, light, probability);
                probability *= wideShare;
                sourceKind = 1;
            }
        }
        else if(hasCell)
        {
            const std::uint32_t index = MulHi(random1, source.paddedCount);
            light = data.words[constants.listsOffset / 4 + source.listOffset + index];
            probability = (1.0f - wideShare) * UniformProbability(index, source.paddedCount);
        }

        const bool usable = light != 0xFFFFFFFFu && probability > 0.0f && light < table.lightCount;
        const float target = usable ? ReGIRCellTarget(table.lights[light], source.regionLow, source.regionHigh,
            constants.cellSize) : 0.0f;

        if(trace)
        {
            trace[sample] = {usable ? light : 0xFFFFFFFFu, sourceKind, probability, target};
        }

        if(!usable)
        {
            continue;
        }

        const float weight = target / (candidates * probability);

        if(!(weight > 0.0f) || !std::isfinite(weight))
        {
            continue;
        }

        weightSum += weight;
        const float selection = static_cast<float>(random3 >> 8) * (1.0f / 16777216.0f);

        if(selection * weightSum < weight)
        {
            slot.light = light;
            selectedTarget = target;
        }
    }

    if(slot.light != 0xFFFFFFFFu && selectedTarget > 0.0f)
    {
        slot.weight = weightSum / selectedTarget;
    }
    else
    {
        slot.light = 0xFFFFFFFFu;
    }

    return slot;
}

void BuildReGIRLocalIndexTranslation(const SceneLightTable& table, std::vector<std::uint32_t>& translation)
{
    translation.assign(table.lightCount, 0xFFFFFFFFu);
    std::uint32_t local = 0;

    for(std::uint32_t index = 0; index < table.lightCount; ++index)
    {
        if(table.lights[index].type != DirectionalType)
        {
            translation[index] = local++;
        }
    }
}

std::uint32_t CountReGIRLocalLights(const SceneLightTable& table)
{
    std::uint32_t local = 0;

    for(std::uint32_t index = 0; index < table.lightCount; ++index)
    {
        local += table.lights[index].type != DirectionalType ? 1u : 0u;
    }

    return local;
}

ReGIRPresampleConstants MakeReGIRPresampleConstants(const ReGIRWindow& window, const ReGIRSettings& settings,
    std::uint32_t sceneLightCount, std::uint32_t frameSeed)
{
    ReGIRPresampleConstants constants;
    constants.sceneLightCount = sceneLightCount;
    constants.lightsPerCell = settings.lightsPerCell;
    constants.buildSamples = settings.buildSamples;
    constants.frameSeed = frameSeed;

    for(int axis = 0; axis < 3; ++axis)
    {
        constants.originCell[axis] = static_cast<std::int32_t>(window.originCell[axis]);
        constants.windowCells[axis] = window.cells[axis];
    }

    const double threshold = std::round(static_cast<double>(settings.wideFraction) * 4294967296.0);
    constants.wideThreshold = static_cast<std::uint32_t>(std::clamp(threshold, 1.0, 4294967295.0));
    constants.wideTargetLimit = settings.wideTargetLimit;
    constants.samplingJitter = window.samplingJitter;
    constants.precisionGuard = static_cast<float>(window.precisionGuard);

    for(int axis = 0; axis < 3; ++axis)
    {
        constants.windowCenter[axis] = window.center[axis];
    }

    return constants;
}

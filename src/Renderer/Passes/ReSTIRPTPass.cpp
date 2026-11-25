#include "Renderer/Passes/ReSTIRPTPass.h"

#include "Core/Log.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <iterator>
#include <random>
#include <system_error>
#include <thread>

namespace
{
    // RestirPtInitialBody.hlsli flag bits.
    constexpr std::uint32_t FlagExtract = 1u << 0;
    constexpr std::uint32_t FlagRawDiffuseHitDistance = 1u << 1;
    constexpr std::uint32_t FlagRis = 1u << 2;
    constexpr std::uint32_t FlagReservoir = 1u << 3;
    constexpr std::uint32_t FlagNormalizationOneOverM = 1u << 4;
    constexpr std::uint32_t FlagDirectOnly = 1u << 5;
    constexpr std::uint32_t FlagIndirectOnly = 1u << 6;
    // M5.1 T-1.8 test only: NaN injected into one x1 NEE candidate, or into a sample's result, at a sparse pixel lattice.
    constexpr std::uint32_t FlagInjectCandidateNaN = 1u << 7;
    constexpr std::uint32_t FlagInjectResultNaN = 1u << 8;
    // M5.1-6 F3 experiment: the output (image and denoiser signals) is the path tree's sum; the reservoir is unchanged.
    constexpr std::uint32_t FlagOutputSum = 1u << 9;
    // M5.1-4 T-2.1 test only: the self-shift identity check.
    constexpr std::uint32_t FlagSelfShift = 1u << 10;
    constexpr std::uint32_t FlagSelfShiftUnquantized = 1u << 11;
    // Residual boiling §5d option 2 (experimental): direct-light reservoir, fresh bounce light (extraction frames only).
    constexpr std::uint32_t FlagDirectReservoir = 1u << 12;
    // T-2.1 round 3 quantization decomposition: one stored field replaced by its in-register value.
    constexpr std::uint32_t FlagShiftExactDirection = 1u << 13;
    constexpr std::uint32_t FlagShiftExactBarycentrics = 1u << 14;
    // M5.2 C0 performance baseline: one reconnection-type visibility ray per sample (bench), plain opaque (plain).
    constexpr std::uint32_t FlagVisibilityBench = 1u << 15;
    constexpr std::uint32_t FlagVisibilityBenchPlain = 1u << 16;
    // M5.2: temporal reuse.
    constexpr std::uint32_t FlagTemporal = 1u << 17;
    constexpr std::uint32_t FlagTemporalRoundTrip = 1u << 18;
    // M5.3: spatial reuse, its colour-noise reduction, and the pre-pass round trip (diagnostics variant).
    constexpr std::uint32_t FlagSpatial = 1u << 19;
    constexpr std::uint32_t FlagSpatialColour = 1u << 20;
    constexpr std::uint32_t FlagSpatialRoundTrip = 1u << 21;
    // M5.4 C1: temporal reuse in its own pass (the initial kernel stores the canonical and leaves the image).
    constexpr std::uint32_t FlagTemporalPass = 1u << 22;
    // M5.4 C4: the published reconnection criteria, and the test-only A' rule for NEE leaving x_k.
    constexpr std::uint32_t FlagFootprintCriteria = 1u << 23;
    constexpr std::uint32_t FlagNeeInverseProxy = 1u << 24;
    // M5.4 C5: replay compaction in the temporal pass.
    constexpr std::uint32_t FlagReplayCompaction = 1u << 25;
    // M5.4 C6: dual motion vectors.
    constexpr std::uint32_t FlagDualMotion = 1u << 26;
    // M5.4 C7: the duplication map computed, and applied to temporal reuse.
    constexpr std::uint32_t FlagDuplicationMap = 1u << 27;
    constexpr std::uint32_t FlagDuplicationApply = 1u << 28;
    constexpr std::uint32_t FlagStagnancy = 1u << 30;
    constexpr std::uint32_t FlagFirefly = 1u << 31;

    // M5.3 pairing texture [P 3.1]: consecutive link indices (raster pairs), n tiled 2x2 random shuffles (every other
    // one offset diagonally by one, wrapping), the two cells of each link joined by their coordinate delta, links longer
    // than half the side wrapped (tileable). Packed as int8 dx | int8 dy << 8.
    std::vector<std::uint32_t> BuildPairingTexture(std::uint32_t size, double sigma, std::uint32_t seed)
    {
        const std::uint32_t texels = size * size;
        // Eq. 3's printed approximation, n = floor(sigma^2 / 2 + 0.5) (127 for sigma 15.96). Its exact form lost its
        // layout in extraction (Q-1); read as sigma^2 / (2 + 1.46 / sigma + ...) + 0.5 it gives 121 shuffles and a
        // measured sigma 2.6-2.8% low, outside the specification's 16 +- 2%, while this form lands within 0.5% (M5.3 S0).
        const int shuffles = static_cast<int>(sigma * sigma / 2.0 + 0.5);
        std::vector<std::uint32_t> links(texels);

        for(std::uint32_t texel = 0; texel < texels; ++texel)
        {
            links[texel] = texel / 2u;
        }

        std::mt19937 random(seed);

        for(int shuffle = 0; shuffle < shuffles; ++shuffle)
        {
            const std::uint32_t offset = static_cast<std::uint32_t>(shuffle & 1);

            for(std::uint32_t blockY = 0; blockY < size / 2u; ++blockY)
            {
                for(std::uint32_t blockX = 0; blockX < size / 2u; ++blockX)
                {
                    const std::uint32_t x0 = (2u * blockX + offset) % size;
                    const std::uint32_t x1 = (2u * blockX + offset + 1u) % size;
                    const std::uint32_t y0 = (2u * blockY + offset) % size;
                    const std::uint32_t y1 = (2u * blockY + offset + 1u) % size;
                    std::uint32_t* cells[4] = {&links[y0 * size + x0], &links[y0 * size + x1], &links[y1 * size + x0],
                        &links[y1 * size + x1]};
                    std::uint32_t values[4] = {*cells[0], *cells[1], *cells[2], *cells[3]};
                    std::shuffle(values, values + 4, random);

                    for(int cell = 0; cell < 4; ++cell)
                    {
                        *cells[cell] = values[cell];
                    }
                }
            }
        }

        const auto wrap = [size](int delta)
        {
            const int half = static_cast<int>(size / 2u);
            return delta > half ? delta - static_cast<int>(size) : delta < -half ? delta + static_cast<int>(size) : delta;
        };
        const auto pack = [](int dx, int dy)
        {
            return (static_cast<std::uint32_t>(dx) & 0xFFu) | ((static_cast<std::uint32_t>(dy) & 0xFFu) << 8u);
        };
        std::vector<std::int64_t> first(texels / 2u, -1);
        std::vector<std::uint32_t> packed(texels, 0u);

        for(std::uint32_t texel = 0; texel < texels; ++texel)
        {
            std::int64_t& partner = first[links[texel]];

            if(partner < 0)
            {
                partner = texel;
                continue;
            }

            const std::uint32_t a = static_cast<std::uint32_t>(partner);
            const int dx = wrap(static_cast<int>(texel % size) - static_cast<int>(a % size));
            const int dy = wrap(static_cast<int>(texel / size) - static_cast<int>(a / size));
            packed[a] = pack(dx, dy);
            packed[texel] = pack(-dx, -dy);
        }

        return packed;
    }

    int PairingDelta(std::uint32_t packed, int channel)
    {
        return static_cast<int>(static_cast<std::int8_t>((packed >> (8u * static_cast<std::uint32_t>(channel))) & 0xFFu));
    }

    std::uint64_t SplitMix(std::uint64_t value)
    {
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

    // The path tracer's static samplers (RayTracingPipeline::CreateGlobalRootSignature): materials wrap at s0;
    // equirectangular environments wrap longitude and clamp latitude at s1.
    D3D12_STATIC_SAMPLER_DESC PathTraceSampler(UINT shaderRegister, bool clampLatitude)
    {
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = clampLatitude ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MipLODBias = 0.0f;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = shaderRegister;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        return sampler;
    }

    RootParameterDescription Parameter(RootParameterType type, unsigned int shaderRegister)
    {
        RootParameterDescription parameter{};
        parameter.type = type;
        parameter.shaderRegister = shaderRegister;
        parameter.visibility = D3D12_SHADER_VISIBILITY_ALL;
        return parameter;
    }

    RootParameterDescription UnorderedAccessTable(unsigned int shaderRegister)
    {
        RootParameterDescription parameter = Parameter(RootParameterType::DescriptorTable, shaderRegister);
        parameter.descriptorRangeType = DescriptorRangeType::UnorderedAccess;
        parameter.descriptorCount = 1;
        return parameter;
    }

    // One pipeline of the pass and the shader it is created from.
    struct PipelineSource
    {
        const char* path;
        const char* entryPoint;
        // The initial-sampling kernels and the reuse passes share the path tracer's root signature; the light-tile and
        // ReGIR light-map passes take one constant buffer.
        bool kernel;
        ComputePipeline ReSTIRPTPreparedPipelines::* pipeline;
    };

    // Every pipeline the pass owns. The unified initial sampling is production; the temporal, diagnostics and visibility
    // bench variants are compiled apart so production never carries their state (M5.2 C0). The spatial passes (M5.3), the
    // temporal reuse pass (M5.4 C1) and the duplication map (M5.4 C7) are entry points of the same shaders (same bindings).
    // Ordered by the driver's measured cost after a shader change, largest first, so the parallel creation ends sooner.
    constexpr PipelineSource PipelineSources[] = {
        {"ReSTIRPT/RestirPtInitialDiagnostics.hlsl", "RestirPtInitialCS", true, &ReSTIRPTPreparedPipelines::diagnostics},
        {"ReSTIRPT/RestirPtInitialTemporal.hlsl", "RestirPtInitialCS", true, &ReSTIRPTPreparedPipelines::temporal},
        // Wall boiling (experimental): the ReGIR kernel with light-traced bounce connections.
        {"ReSTIRPT/RestirPtInitialReGIRLightBounce.hlsl", "RestirPtInitialCS", true,
            &ReSTIRPTPreparedPipelines::initialReGIRLightBounce},
        {"ReSTIRPT/RestirPtInitialDiagnostics.hlsl", "RestirPtSpatialPairsCS", true,
            &ReSTIRPTPreparedPipelines::spatialPairsDiagnostics},
        {"ReSTIRPT/RestirPtInitialReGIR.hlsl", "RestirPtInitialCS", true, &ReSTIRPTPreparedPipelines::initialReGIR},
        {"ReSTIRPT/RestirPtInitialDiagnostics.hlsl", "RestirPtTemporalCS", true,
            &ReSTIRPTPreparedPipelines::temporalPassDiagnostics},
        {"ReSTIRPT/RestirPtInitialVisibilityBench.hlsl", "RestirPtInitialCS", true,
            &ReSTIRPTPreparedPipelines::visibilityBench},
        {"ReSTIRPT/RestirPtInitial.hlsl", "RestirPtInitialCS", true, &ReSTIRPTPreparedPipelines::initial},
        {"ReSTIRPT/RestirPtInitialTemporal.hlsl", "RestirPtTemporalCS", true, &ReSTIRPTPreparedPipelines::temporalPass},
        {"ReSTIRPT/RestirPtInitial.hlsl", "RestirPtSpatialPairsCS", true, &ReSTIRPTPreparedPipelines::spatialPairs},
        {"ReSTIRPT/RestirPtInitial.hlsl", "RestirPtSpatialResampleCS", true, &ReSTIRPTPreparedPipelines::spatialResample},
        {"ReSTIRPT/RestirPtInitial.hlsl", "RestirPtDuplicationCS", true, &ReSTIRPTPreparedPipelines::duplication},
        {"ReSTIRPT/RestirPtLightTiles.hlsl", "main", false, &ReSTIRPTPreparedPipelines::lightTiles},
        // M5.6 O0: the ReGIR light map.
        {"ReSTIRPT/RestirPtReGIRLights.hlsl", "main", false, &ReSTIRPTPreparedPipelines::regirLights},
    };

    // At most this many threads create the pipelines (a quarter of the hardware threads): the driver's cost is a few
    // large kernels, so more threads would not end sooner, and the Editor keeps the rest of the CPU.
    constexpr unsigned int MaximumPipelineThreads = 4;

    // Creates one pipeline. A kernel has the path tracer's root signature (t0 TLAS, u0-u2 accumulation, output and
    // statistics, b0 path tracing constants, b1 scene light constants) plus RestirPtConstants at b2, root parameter 7 (t1,
    // M5.1-4: the per-instance transforms, a root SRV), and its two static samplers. Returns the failure, empty on success.
    std::string CreatePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const PipelineSource& source,
        ComputePipeline& pipeline)
    {
        ShaderDescription shaderDescription{};
        shaderDescription.path = source.path;
        shaderDescription.entryPoint = source.entryPoint;
        shaderDescription.stage = ShaderStage::Compute;
        Shader* shader = shaderLibrary.Load(shaderDescription);

        if(!shader)
        {
            return std::string(source.path) + " (" + source.entryPoint + ") did not compile.";
        }

        ComputePipelineDescription description{};
        description.computeShader = shader;

        if(source.kernel)
        {
            description.rootParameters = {Parameter(RootParameterType::ShaderResource, 0), UnorderedAccessTable(0),
                UnorderedAccessTable(1), UnorderedAccessTable(2), Parameter(RootParameterType::ConstantBuffer, 0),
                Parameter(RootParameterType::ConstantBuffer, 1), Parameter(RootParameterType::ConstantBuffer, 2),
                Parameter(RootParameterType::ShaderResource, 1)};
            description.staticSamplers = {PathTraceSampler(0, false), PathTraceSampler(1, true)};
        }
        else
        {
            description.rootParameters = {Parameter(RootParameterType::ConstantBuffer, 0)};
        }

        if(!pipeline.Initialize(device, description))
        {
            return std::string("the pipeline of ") + source.path + " (" + source.entryPoint + ") could not be created.";
        }

        return {};
    }

    // M5.3: the pairing textures, one per spatial neighbour, each checked to be an involution with the target standard
    // deviation. CPU only. Returns the failure, empty on success.
    std::string BuildPairingTexels(std::vector<std::uint32_t>& texels, std::string& summary)
    {
        // sigma = sqrt(8 / (9 pi)) R for the ReSTIR default R = 30 [P 7]: 16.0.
        const double sigma = std::sqrt(8.0 / (9.0 * 3.14159265358979323846)) * 30.0;
        texels.clear();
        summary.clear();

        for(std::uint32_t t = 0; t < ReSTIRPTPass::SpatialNeighbours; ++t)
        {
            const std::uint32_t size = ReSTIRPTPass::PairingSizes[t];
            const std::vector<std::uint32_t> texture = BuildPairingTexture(size, sigma, 0x52535033u + 7919u * t);
            double squares = 0.0;
            bool involution = true;

            for(std::uint32_t texel = 0; texel < size * size; ++texel)
            {
                const int dx = PairingDelta(texture[texel], 0);
                const int dy = PairingDelta(texture[texel], 1);
                squares += static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
                const std::uint32_t x = static_cast<std::uint32_t>((static_cast<int>(texel % size) + dx +
                    static_cast<int>(size)) % static_cast<int>(size));
                const std::uint32_t y = static_cast<std::uint32_t>((static_cast<int>(texel / size) + dy +
                    static_cast<int>(size)) % static_cast<int>(size));
                const std::uint32_t back = texture[y * size + x];
                involution = involution && (dx != 0 || dy != 0) && PairingDelta(back, 0) == -dx &&
                    PairingDelta(back, 1) == -dy;
            }

            // Standard deviation per axis (the deltas' mean is 0 by construction).
            const double measured = std::sqrt(squares / (2.0 * size * size));

            if(!involution || std::abs(measured - sigma) > 0.02 * sigma)
            {
                return "pairing texture " + std::to_string(size) + " failed its check (involution " +
                    std::to_string(involution) + ", sigma " + std::to_string(measured) + ", target " +
                    std::to_string(sigma) + " +- 2%).";
            }

            summary += " " + std::to_string(size) + ": sigma " + std::to_string(measured);
            texels.insert(texels.end(), texture.begin(), texture.end());
        }

        return {};
    }
}

void ReSTIRPTPreparedPipelines::Shutdown()
{
    for(const PipelineSource& source : PipelineSources)
    {
        (this->*source.pipeline).Shutdown();
    }

    pairingTexels.clear();
    pairingSummary.clear();
}

bool ReSTIRPTPass::PreparePipelines(ID3D12Device* device, const std::filesystem::path& shaderRoot,
    ReSTIRPTPreparedPipelines& prepared)
{
    if(!device || shaderRoot.empty())
    {
        prepared.failure = "no device or no shader sources (ReSTIR PT Enhanced is Editor-only).";
        return false;
    }

    // Each thread takes the next pipeline of the table until none is left, with its own ShaderLibrary (a library is used
    // by one thread at a time), below normal priority so the compiles never take CPU from the render thread. Each slot of
    // the table is written by exactly one thread.
    constexpr std::size_t PipelineCount = std::size(PipelineSources);
    std::vector<std::string> failures(PipelineCount);
    std::atomic<std::size_t> nextPipeline{0};
    std::atomic<bool> threadFailed{false};
    const auto createPipelines = [&]()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        ShaderLibrary shaderLibrary;

        try
        {
            const bool libraryReady = shaderLibrary.Initialize(shaderRoot);

            for(std::size_t index = nextPipeline++; index < PipelineCount; index = nextPipeline++)
            {
                const PipelineSource& source = PipelineSources[index];
                failures[index] = libraryReady ? CreatePipeline(device, shaderLibrary, source, prepared.*source.pipeline) :
                    std::string("the shader compiler could not be initialized for ") + source.path + ".";
            }
        }
        catch(const std::exception&)
        {
            threadFailed = true;
        }

        shaderLibrary.Shutdown();
    };

    const unsigned int threadCount = std::clamp(std::thread::hardware_concurrency() / 4u, 1u, MaximumPipelineThreads);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    try
    {
        for(unsigned int thread = 0; thread < threadCount; ++thread)
        {
            threads.emplace_back(createPipelines);
        }
    }
    catch(const std::system_error&)
    {
        // Fewer threads: the started ones take every pipeline.
    }

    // The pairing texels meanwhile, on this thread (an exception must not leave the joinable threads behind).
    std::string pairingFailure;

    try
    {
        pairingFailure = BuildPairingTexels(prepared.pairingTexels, prepared.pairingSummary);
    }
    catch(const std::exception& exception)
    {
        pairingFailure = std::string("the pairing textures could not be built: ") + exception.what();
    }

    for(std::thread& thread : threads)
    {
        thread.join();
    }

    if(threads.empty())
    {
        prepared.failure = "no thread could be started to create the pipelines.";
        return false;
    }

    if(threadFailed)
    {
        prepared.failure = "a pipeline thread failed with an exception.";
        return false;
    }

    for(const std::string& failure : failures)
    {
        if(!failure.empty())
        {
            prepared.failure = failure;
            return false;
        }
    }

    prepared.failure = pairingFailure;
    return prepared.failure.empty();
}

bool ReSTIRPTPass::Fail(const std::string& failure)
{
    m_failed = true;
    m_failure = failure;
    Log::Error("ReSTIR PT (configuration B) unavailable: " + failure);
    Shutdown();
    m_failed = true;
    m_failure = failure;
    return false;
}

void ReSTIRPTPass::AdoptPipelines(const ReSTIRPTPreparedPipelines& prepared)
{
    m_initialPipeline.CopyFrom(prepared.initial);
    m_temporalPipeline.CopyFrom(prepared.temporal);
    m_initialReGIRPipeline.CopyFrom(prepared.initialReGIR);
    m_initialReGIRLightBouncePipeline.CopyFrom(prepared.initialReGIRLightBounce);
    m_diagnosticsPipeline.CopyFrom(prepared.diagnostics);
    m_visibilityBenchPipeline.CopyFrom(prepared.visibilityBench);
    m_spatialPairsPipeline.CopyFrom(prepared.spatialPairs);
    m_spatialPairsDiagnosticsPipeline.CopyFrom(prepared.spatialPairsDiagnostics);
    m_spatialResamplePipeline.CopyFrom(prepared.spatialResample);
    m_temporalPassPipeline.CopyFrom(prepared.temporalPass);
    m_temporalPassDiagnosticsPipeline.CopyFrom(prepared.temporalPassDiagnostics);
    m_duplicationPipeline.CopyFrom(prepared.duplication);
    m_lightTilePipeline.CopyFrom(prepared.lightTiles);
    m_regirLightsPipeline.CopyFrom(prepared.regirLights);
}

bool ReSTIRPTPass::UploadPairingTextures(ID3D12Device* device, DescriptorHeap& descriptorHeap, ResourceUploader& uploader,
    const std::string& pairingSummary)
{
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    if(!m_pairing.Initialize(device, uploader, m_pairingTexels.data(), m_pairingTexels.size() * sizeof(std::uint32_t),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) || !m_pairing.CreateRawSRV(device, descriptorHeap))
    {
        return Fail("the pairing textures could not be uploaded.");
    }

    Log::Info("ReSTIR PT (configuration B): pairing textures built (involutions verified);" + pairingSummary + ".");
    return true;
}

bool ReSTIRPTPass::EnsureSpatialResources(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!m_initialized || m_width == 0 || m_height == 0 || !m_pairing.GetResource())
    {
        return false;
    }

    if(m_spatialScratch.GetResource() && m_pairs.GetResource())
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);
    const std::uint64_t scratchBytes = std::uint64_t{m_width} * m_height * ReservoirBytes;
    const std::uint64_t pairBytes = std::uint64_t{m_width} * m_height * SpatialNeighbours * PairRecordBytes;

    if(!m_spatialScratch.InitializeGPUWritable(device, static_cast<std::size_t>(scratchBytes)) ||
        !m_spatialScratch.CreateRawUAV(device, descriptorHeap) ||
        !m_pairs.InitializeGPUWritable(device, static_cast<std::size_t>(pairBytes)) ||
        !m_pairs.CreateRawUAV(device, descriptorHeap))
    {
        m_spatialScratch.Shutdown();
        m_pairs.Shutdown();
        Log::Error("ReSTIR PT (configuration B): the spatial buffers could not be allocated.");
        return false;
    }

    Log::Info("ReSTIR PT (configuration B): spatial buffers created at " + std::to_string(m_width) + "x" +
        std::to_string(m_height) + ": scratch reservoirs " + std::to_string(scratchBytes) + " bytes, pair records " +
        std::to_string(pairBytes) + " bytes (" + std::to_string(SpatialNeighbours * PairRecordBytes) + " B/px).");
    return true;
}

bool ReSTIRPTPass::EnsureReplayList(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!m_initialized || m_width == 0 || m_height == 0)
    {
        return false;
    }

    if(m_replayList.GetResource())
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);
    const std::uint64_t bytes = (std::uint64_t{m_width} * m_height + 4u) * sizeof(std::uint32_t);

    if(!m_replayList.InitializeGPUWritable(device, static_cast<std::size_t>(bytes)) ||
        !m_replayList.CreateRawUAV(device, descriptorHeap))
    {
        m_replayList.Shutdown();
        Log::Error("ReSTIR PT (configuration B): the replay list could not be allocated.");
        return false;
    }

    return true;
}

bool ReSTIRPTPass::EnsureDuplicationMaps(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!m_initialized || m_width == 0 || m_height == 0)
    {
        return false;
    }

    if(m_duplication[0].GetResource() && m_duplication[1].GetResource())
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);
    const std::uint64_t bytes = std::uint64_t{m_width} * m_height * sizeof(std::uint32_t);

    for(Buffer& map : m_duplication)
    {
        if(!map.InitializeGPUWritable(device, static_cast<std::size_t>(bytes)) || !map.CreateRawUAV(device, descriptorHeap))
        {
            m_duplication[0].Shutdown();
            m_duplication[1].Shutdown();
            Log::Error("ReSTIR PT (configuration B): the duplication maps could not be allocated.");
            return false;
        }
    }

    return true;
}

bool ReSTIRPTPass::EnsureStagnancyMaps(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!m_initialized || m_width == 0 || m_height == 0)
    {
        return false;
    }

    if(m_stagnancy[0].GetResource() && m_stagnancy[1].GetResource())
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);
    const std::uint64_t bytes = std::uint64_t{m_width} * m_height * sizeof(float);

    for(Buffer& map : m_stagnancy)
    {
        if(!map.InitializeGPUWritable(device, static_cast<std::size_t>(bytes)) || !map.CreateRawUAV(device, descriptorHeap))
        {
            m_stagnancy[0].Shutdown();
            m_stagnancy[1].Shutdown();
            Log::Error("ReSTIR PT (configuration B): the stagnancy maps could not be allocated.");
            return false;
        }
    }

    return true;
}

bool ReSTIRPTPass::EnsureDebugPixels(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!m_initialized || m_width == 0 || m_height == 0)
    {
        return false;
    }

    if(m_debugPixels.GetResource())
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);

    if(!m_debugPixels.InitializeGPUWritable(device, static_cast<std::size_t>(std::uint64_t{m_width} * m_height * 16u)) ||
        !m_debugPixels.CreateRawUAV(device, descriptorHeap))
    {
        m_debugPixels.Shutdown();
        return false;
    }

    return true;
}

bool ReSTIRPTPass::EnsureInstanceHistory(ID3D12Device* device, DescriptorHeap& descriptorHeap)
{
    if(!m_initialized || m_width == 0 || m_height == 0)
    {
        return false;
    }

    if(m_instanceHistory[0].GetResource() && m_instanceHistory[1].GetResource())
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);
    const std::uint64_t bytes = std::uint64_t{m_width} * m_height * sizeof(std::uint32_t);

    for(Buffer& history : m_instanceHistory)
    {
        if(!history.InitializeGPUWritable(device, static_cast<std::size_t>(bytes)) || !history.CreateRawUAV(device, descriptorHeap))
        {
            m_instanceHistory[0].Shutdown();
            m_instanceHistory[1].Shutdown();
            Log::Error("ReSTIR PT (configuration B): the instance history could not be allocated.");
            return false;
        }
    }

    // A new history holds no frame yet; the reservoirs' history decides whether last frame's may be read, and the first
    // frame that writes it is never read as "previous" before it is committed.
    return true;
}

bool ReSTIRPTPass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& descriptorHeap,
    ResourceUploader& uploader, const ReSTIRPTSettings& settings)
{
    if(m_initialized)
    {
        return true;
    }

    if(m_failed)
    {
        return false;
    }

    if(!device || settings.tileCount == 0 || settings.tileSize == 0)
    {
        return Fail("invalid device or light-tile settings.");
    }

    // Without pipelines prepared on a worker (direct Render callers, no pipeline worker): the same parallel preparation,
    // waited for here. A failed set fails the pass below.
    ReSTIRPTPreparedPipelines prepared;
    PreparePipelines(device, shaderLibrary.GetShaderRoot(), prepared);
    const bool initialized = Initialize(device, descriptorHeap, uploader, settings, prepared);
    prepared.Shutdown();
    return initialized;
}

bool ReSTIRPTPass::Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ResourceUploader& uploader,
    const ReSTIRPTSettings& settings, ReSTIRPTPreparedPipelines& prepared)
{
    if(m_initialized)
    {
        return true;
    }

    if(m_failed)
    {
        return false;
    }

    if(!device || settings.tileCount == 0 || settings.tileSize == 0)
    {
        return Fail("invalid device or light-tile settings.");
    }

    if(!prepared.failure.empty())
    {
        return Fail(prepared.failure);
    }

    // The pipelines gain this pass's references; the set keeps its own until its owner releases it.
    AdoptPipelines(prepared);
    m_pairingTexels = std::move(prepared.pairingTexels);

    if(!UploadPairingTextures(device, descriptorHeap, uploader, prepared.pairingSummary))
    {
        return false;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);
    const std::uint64_t tileBytes = std::uint64_t{settings.tileCount} * settings.tileSize * LightTileEntryBytes;

    if(!m_lightTiles.InitializeGPUWritable(device, static_cast<std::size_t>(tileBytes)) ||
        !m_lightTiles.CreateRawUAV(device, descriptorHeap) || !m_lightTiles.CreateRawSRV(device, descriptorHeap))
    {
        return Fail("the light tiles could not be allocated.");
    }

    // 8 bytes per local light (table index, p_1), then 4 bytes per table light (p_1).
    if(!m_regirLights.InitializeGPUWritable(device, std::size_t{ReGIRLightCapacity} * 12u) ||
        !m_regirLights.CreateRawUAV(device, descriptorHeap) || !m_regirLights.CreateRawSRV(device, descriptorHeap))
    {
        return Fail("the ReGIR light map could not be allocated.");
    }

    if(!m_counters.InitializeGPUWritable(device, CounterCount * sizeof(std::uint32_t)) ||
        !m_counters.CreateRawUAV(device, descriptorHeap))
    {
        return Fail("the counters could not be allocated.");
    }

    m_tileCount = settings.tileCount;
    m_tileSize = settings.tileSize;
    m_initialized = true;
    Log::Info("ReSTIR PT (configuration B): pipelines created; light tiles " + std::to_string(m_tileCount) + " x " +
        std::to_string(m_tileSize) + " (" + std::to_string(tileBytes) + " bytes).");
    return true;
}

bool ReSTIRPTPass::EnsureReservoirs(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width,
    unsigned int height)
{
    if(!m_initialized || width == 0 || height == 0)
    {
        return false;
    }

    if(m_reservoirs[0].GetResource() && m_reservoirs[1].GetResource() && m_width == width && m_height == height)
    {
        return true;
    }

    ReleaseTextures();
    const MemoryCategoryScope memoryScope(MemoryCategory::PathTracing);
    const std::uint64_t bytes = std::uint64_t{width} * height * ReservoirBytes;

    for(Buffer& reservoirs : m_reservoirs)
    {
        if(!reservoirs.InitializeGPUWritable(device, static_cast<std::size_t>(bytes)) ||
            !reservoirs.CreateRawUAV(device, descriptorHeap))
        {
            ReleaseTextures();
            Log::Error("ReSTIR PT (configuration B): the reservoirs (2 x " + std::to_string(bytes) +
                " bytes) could not be allocated.");
            return false;
        }
    }

    m_width = width;
    m_height = height;
    return true;
}

void ReSTIRPTPass::ReleaseTextures()
{
    for(Buffer& reservoirs : m_reservoirs)
    {
        reservoirs.Shutdown();
    }

    m_spatialScratch.Shutdown();
    m_pairs.Shutdown();
    m_replayList.Shutdown();
    m_instanceHistory[0].Shutdown();
    m_instanceHistory[1].Shutdown();
    m_duplication[0].Shutdown();
    m_duplication[1].Shutdown();
    m_stagnancy[0].Shutdown();
    m_stagnancy[1].Shutdown();
    m_debugPixels.Shutdown();

    // The reservoirs' contents are gone with them.
    InvalidateHistory();
    m_width = 0;
    m_height = 0;
}

ReSTIRPTFrame ReSTIRPTPass::BeginFrame(const ReSTIRPTHistorySignature& signature, bool previousSurfacesAvailable)
{
    ReSTIRPTFrame frame;
    frame.decision = ReSTIRPTHistoryDecision::Reused;

    if(!m_committed)
    {
        frame.decision = ReSTIRPTHistoryDecision::NoCommittedFrame;
    }
    else if(!previousSurfacesAvailable)
    {
        frame.decision = ReSTIRPTHistoryDecision::PreviousSurfacesUnavailable;
    }
    else if(signature.width != m_committedSignature.width || signature.height != m_committedSignature.height)
    {
        frame.decision = ReSTIRPTHistoryDecision::Extent;
    }
    else if(signature.sceneResourceLifetime != m_committedSignature.sceneResourceLifetime)
    {
        frame.decision = ReSTIRPTHistoryDecision::SceneResources;
    }
    else if(signature.topLevelAddress != m_committedSignature.topLevelAddress)
    {
        frame.decision = ReSTIRPTHistoryDecision::TopLevel;
    }
    else if(signature.topLevelGeometry != m_committedSignature.topLevelGeometry)
    {
        frame.decision = ReSTIRPTHistoryDecision::Geometry;
    }
    else if(signature.materialRevision != m_committedSignature.materialRevision)
    {
        frame.decision = ReSTIRPTHistoryDecision::Material;
    }
    else if(signature.lightingHash != m_committedSignature.lightingHash)
    {
        frame.decision = ReSTIRPTHistoryDecision::Lighting;
    }
    else if(signature.settingsHash != m_committedSignature.settingsHash)
    {
        frame.decision = ReSTIRPTHistoryDecision::Settings;
    }

    frame.historyValid = frame.decision == ReSTIRPTHistoryDecision::Reused;
    frame.previousSet = m_committedSet;
    // Never write the set history reads; without history either set may be written.
    frame.writeSet = m_committed ? 1u - m_committedSet : 0u;
    m_pending = true;
    m_pendingSet = frame.writeSet;
    m_pendingSignature = signature;
    m_lastWriteSet = frame.writeSet;
    return frame;
}

void ReSTIRPTPass::CommitFrame()
{
    if(!m_pending)
    {
        return;
    }

    m_committed = true;
    m_committedSet = m_pendingSet;
    m_committedSignature = m_pendingSignature;
    m_pending = false;
    // M5.5 Stage 1: the committed frame's scene goes with its reservoirs.
    m_committedSceneInstances.swap(m_pendingSceneInstances);
    m_committedSceneIdentity = m_pendingSceneIdentity;
    m_pendingSceneInstances.clear();
    m_pendingSceneIdentity = 0;
}

void ReSTIRPTPass::AbandonFrame()
{
    m_pending = false;
    m_pendingSceneInstances.clear();
    m_pendingSceneIdentity = 0;
}

void ReSTIRPTPass::InvalidateHistory()
{
    m_committed = false;
    m_pending = false;
    m_pendingSceneInstances.clear();
    m_committedSceneInstances.clear();
    m_pendingSceneIdentity = 0;
    m_committedSceneIdentity = 0;
}

void ReSTIRPTPass::SetPendingScene(const std::vector<RayTracingInstanceDescription>& instances, std::uint64_t geometryIdentity)
{
    m_pendingSceneInstances = instances;
    m_pendingSceneIdentity = geometryIdentity;
}

std::uint32_t ReSTIRPTPass::PrepareHistoryScene(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex, DescriptorHeap& descriptorHeap, std::uint64_t geometryIdentity,
    const std::vector<const BottomLevelAccelerationStructure*>& bottomLevels)
{
    // Only while the committed frame used this exact geometry (the same BLAS set): then only instance transforms differ, and
    // the history TLAS references BLASes the current TLAS keeps alive for this frame's work.
    if(!device || !commandList || !m_committed || m_committedSceneInstances.empty() || geometryIdentity == 0 ||
        m_committedSceneIdentity != geometryIdentity || bottomLevels.empty())
    {
        return 0xFFFFFFFFu;
    }

    HistoryScene& slot = m_historyScenes[frameIndex % HistorySceneSlots];

    if(slot.descriptor == 0xFFFFFFFFu)
    {
        slot.descriptor = descriptorHeap.Allocate();

        if(slot.descriptor == UINT_MAX)
        {
            slot.descriptor = 0xFFFFFFFFu;
            return 0xFFFFFFFFu;
        }

        m_historySceneHeap = &descriptorHeap;
    }

    bool recorded = false;

    // This slot was last used by the frame that previously held this frame index, which is complete once the index is
    // acquired; refit when the instance set is unchanged, else rebuild.
    if(slot.initialized && slot.geometryIdentity == geometryIdentity && slot.instanceCount == m_committedSceneInstances.size())
    {
        std::vector<std::size_t> every(m_committedSceneInstances.size());

        for(std::size_t index = 0; index < every.size(); ++index)
        {
            every[index] = index;
        }

        recorded = slot.topLevel.Update(commandList, frameIndex, m_committedSceneInstances, every, bottomLevels);
    }
    else
    {
        slot.topLevel.Shutdown();
        RayTracingTopLevelDescription description;
        description.instances = m_committedSceneInstances;
        recorded = slot.topLevel.Initialize(device, commandList, description, bottomLevels);
        slot.geometryIdentity = geometryIdentity;
        slot.instanceCount = m_committedSceneInstances.size();
    }

    slot.initialized = recorded;

    if(!recorded)
    {
        return 0xFFFFFFFFu;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS address = slot.topLevel.GetGPUAddress();

    if(address != slot.address)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.RaytracingAccelerationStructure.Location = address;
        device->CreateShaderResourceView(nullptr, &view, descriptorHeap.GetCPUHandle(slot.descriptor));
        slot.address = address;
    }

    // The build or refit is read by the ReSTIR PT dispatches that follow.
    D3D12_RESOURCE_BARRIER ordering{};
    ordering.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ordering.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &ordering);
    return slot.descriptor;
}

const char* ReSTIRPTHistoryDecisionName(ReSTIRPTHistoryDecision decision)
{
    switch(decision)
    {
    case ReSTIRPTHistoryDecision::Reused:
        return "reused";
    case ReSTIRPTHistoryDecision::NoCommittedFrame:
        return "no committed frame";
    case ReSTIRPTHistoryDecision::PreviousSurfacesUnavailable:
        return "previous surfaces unavailable";
    case ReSTIRPTHistoryDecision::Extent:
        return "extent";
    case ReSTIRPTHistoryDecision::SceneResources:
        return "scene resources";
    case ReSTIRPTHistoryDecision::TopLevel:
        return "top level";
    case ReSTIRPTHistoryDecision::Geometry:
        return "geometry";
    case ReSTIRPTHistoryDecision::Material:
        return "material";
    case ReSTIRPTHistoryDecision::Lighting:
        return "lighting";
    case ReSTIRPTHistoryDecision::Settings:
        return "settings";
    }

    return "unknown";
}

void ReSTIRPTPass::Shutdown()
{
    // M5.5 Stage 1: the history TLASes and their descriptors (teardown follows GPU completion).
    for(HistoryScene& slot : m_historyScenes)
    {
        slot.topLevel.Shutdown();

        if(slot.descriptor != 0xFFFFFFFFu && m_historySceneHeap)
        {
            m_historySceneHeap->Free(slot.descriptor);
        }

        slot.descriptor = 0xFFFFFFFFu;
        slot.geometryIdentity = 0;
        slot.instanceCount = 0;
        slot.address = 0;
        slot.initialized = false;
    }

    m_pendingSceneInstances.clear();
    m_committedSceneInstances.clear();
    ReleaseTextures();
    m_counters.Shutdown();
    m_lightTiles.Shutdown();
    m_lightTilePipeline.Shutdown();
    m_regirLights.Shutdown();
    m_regirLightsPipeline.Shutdown();
    m_initialPipeline.Shutdown();
    m_initialReGIRPipeline.Shutdown();
    m_initialReGIRLightBouncePipeline.Shutdown();
    m_temporalPipeline.Shutdown();
    m_diagnosticsPipeline.Shutdown();
    m_visibilityBenchPipeline.Shutdown();
    m_spatialPairsPipeline.Shutdown();
    m_spatialPairsDiagnosticsPipeline.Shutdown();
    m_spatialResamplePipeline.Shutdown();
    m_temporalPassPipeline.Shutdown();
    m_temporalPassDiagnosticsPipeline.Shutdown();
    m_duplicationPipeline.Shutdown();
    m_pairing.Shutdown();
    m_pairingTexels.clear();
    m_tileCount = 0;
    m_tileSize = 0;
    m_initialized = false;
}

bool ReSTIRPTPass::RecordLightTiles(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    std::uint32_t lightSamplingIndex, std::uint32_t tableLightCount, std::uint32_t frameSeed)
{
    if(!m_initialized || !commandList)
    {
        return false;
    }

    m_regirThisFrame = false;
    ReSTIRPTLightTileConstants constants{};
    constants.lightSamplingIndex = lightSamplingIndex;
    constants.outputIndex = m_lightTiles.GetUAVDescriptorIndex();
    constants.tileCount = m_tileCount;
    constants.tileSize = m_tileSize;
    constants.frameSeed = frameSeed;
    constants.tableLightCount = tableLightCount;
    const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&constants, sizeof(constants),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(address == 0)
    {
        return false;
    }

    // Every entry is rewritten each frame (an invalid draw stores light 0xFFFFFFFF), so no entry outlives its frame.
    m_lightTiles.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_lightTilePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, address);
    const std::uint32_t entries = m_tileCount * m_tileSize;
    commandList->Dispatch((entries + 255u) / 256u, 1, 1);
    m_lightTiles.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

std::uint32_t ReSTIRPTPass::RecordReGIRLights(ID3D12GraphicsCommandList* commandList, UploadBuffer& uploadBuffer,
    std::uint32_t lightSamplingIndex, std::uint32_t tableLightCount)
{
    if(!m_initialized || !commandList || !m_regirLights.GetResource() || tableLightCount == 0 ||
        tableLightCount > ReGIRLightCapacity || lightSamplingIndex == 0xFFFFFFFFu)
    {
        return 0xFFFFFFFFu;
    }

    // Phase 0 (one thread per table light) writes (table index, 0) at the light's RTXDI local index; phase 1 (one thread
    // per global alias entry) then writes the entry light's realized probability. Every local light is rewritten.
    m_regirLights.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_regirLightsPipeline.Bind(commandList);

    for(std::uint32_t phase = 0; phase < 2; ++phase)
    {
        ReSTIRPTLightTileConstants constants{};
        constants.lightSamplingIndex = lightSamplingIndex;
        constants.outputIndex = m_regirLights.GetUAVDescriptorIndex();
        constants.tableLightCount = tableLightCount;
        constants.padding[0] = phase;
        const D3D12_GPU_VIRTUAL_ADDRESS address = uploadBuffer.Allocate(&constants, sizeof(constants),
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(address == 0)
        {
            return 0xFFFFFFFFu;
        }

        commandList->SetComputeRootConstantBufferView(0, address);
        // Phase 1 covers the global table's entries, at most one per table light.
        commandList->Dispatch((tableLightCount + 255u) / 256u, 1, 1);

        if(phase == 0)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = m_regirLights.GetResource();
            commandList->ResourceBarrier(1, &barrier);
        }
    }

    m_regirLights.Transition(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_regirThisFrame = true;
    return m_regirLights.GetDescriptorIndex();
}

ReSTIRPTConstants ReSTIRPTPass::MakeConstants(const ReSTIRPTSettings& settings, bool extract, bool rawDiffuseHitDistance,
    std::uint32_t lightSamplingIndex, std::uint32_t tableLightCount, std::uint32_t frameIndex,
    std::uint32_t productionRouletteDepth, const ReSTIRPTFrame& frame, bool spatial, bool temporalPass) const
{
    ReSTIRPTConstants constants{};
    const Buffer& writeReservoirs = m_reservoirs[frame.writeSet];
    const Buffer& previousReservoirs = m_reservoirs[frame.previousSet];
    constants.reservoirIndex = writeReservoirs.GetResource() ? writeReservoirs.GetUAVDescriptorIndex() : 0xFFFFFFFFu;
    constants.historyValid = frame.historyValid && previousReservoirs.GetResource() ? 1u : 0u;
    constants.previousReservoirIndex = constants.historyValid != 0u ? previousReservoirs.GetUAVDescriptorIndex() :
        0xFFFFFFFFu;
    constants.temporalConfidenceCap = settings.temporalConfidenceCap;
    // M5.5-B: final-shading only, so it is not part of the history signature.
    constants.decorrelation = settings.decorrelation;
    constants.spatialPairCount = std::clamp(settings.spatialPairs, 1u, SpatialNeighbours);
    constants.lightTileIndex = m_lightTiles.GetResource() ? m_lightTiles.GetDescriptorIndex() : 0xFFFFFFFFu;
    constants.lightSamplingIndex = lightSamplingIndex;
    constants.flags = (extract ? FlagExtract : 0u) | (extract && rawDiffuseHitDistance ? FlagRawDiffuseHitDistance : 0u) |
        (settings.ris ? FlagRis : 0u) | (settings.reservoir ? FlagReservoir : 0u) |
        (settings.normalizationOneOverM ? FlagNormalizationOneOverM : 0u) | (settings.directOnly ? FlagDirectOnly : 0u) |
        (settings.indirectOnly ? FlagIndirectOnly : 0u) |
        (settings.inject == ReSTIRPTSettings::Inject::CandidateNaN ? FlagInjectCandidateNaN : 0u) |
        (settings.inject == ReSTIRPTSettings::Inject::ResultNaN ? FlagInjectResultNaN : 0u) |
        (settings.reservoir && settings.outputSum ? FlagOutputSum : 0u) |
        (settings.reservoir && settings.selfShift ? FlagSelfShift : 0u) |
        (settings.reservoir && settings.selfShift && settings.selfShiftUnquantized ? FlagSelfShiftUnquantized : 0u) |
        (settings.reservoir && settings.selfShift && settings.selfShiftExactDirection ? FlagShiftExactDirection : 0u) |
        (settings.reservoir && settings.selfShift && settings.selfShiftExactBarycentrics ? FlagShiftExactBarycentrics : 0u) |
        (settings.visibilityBench != ReSTIRPTSettings::VisibilityBench::None ? FlagVisibilityBench : 0u) |
        (settings.visibilityBench == ReSTIRPTSettings::VisibilityBench::Plain ? FlagVisibilityBenchPlain : 0u) |
        (settings.reservoir && settings.temporal ? FlagTemporal : 0u) |
        (settings.reservoir && settings.temporal && settings.temporalRoundTrip ? FlagTemporalRoundTrip : 0u) |
        (settings.reservoir && settings.temporal && temporalPass ? FlagTemporalPass : 0u) |
        (settings.footprintCriteria ? FlagFootprintCriteria : 0u) |
        (settings.footprintCriteria && settings.neeInverseProxy ? FlagNeeInverseProxy : 0u);

    // Residual boiling §5d option 2: the bounce part reaches the final passes through the extraction textures, so the mode
    // needs extraction (denoiser frames); it also turns the decorrelation swap off (below).
    const bool directReservoir = settings.directReservoir && settings.reservoir && extract;

    if(directReservoir)
    {
        constants.flags |= FlagDirectReservoir;
        constants.decorrelation = 0.0f;
    }

    // M5.4 C6: dual motion vectors read last frame's instance history only with valid reservoir history (the same
    // commit/abandon lifecycle: the sets' parity); this frame's history is written into the write set's partner.
    if(settings.reservoir && settings.temporal && settings.dualMotion && m_instanceHistory[0].GetResource() &&
        m_instanceHistory[1].GetResource())
    {
        constants.flags |= FlagDualMotion;
        constants.instanceOutputIndex = m_instanceHistory[frame.writeSet].GetUAVDescriptorIndex();
        constants.previousInstanceIndex = constants.historyValid != 0u ?
            m_instanceHistory[frame.previousSet].GetUAVDescriptorIndex() : 0xFFFFFFFFu;
    }

    // M5.4 C7: the duplication map of this frame's final reservoirs (into the write set's partner); temporal reuse reads
    // last frame's only with valid history, and only when applying it.
    if(settings.reservoir && settings.temporal && settings.duplication != ReSTIRPTSettings::Duplication::Off &&
        m_duplication[0].GetResource() && m_duplication[1].GetResource())
    {
        constants.flags |= FlagDuplicationMap;
        constants.duplicationOutputIndex = m_duplication[frame.writeSet].GetUAVDescriptorIndex();

        if(settings.duplication == ReSTIRPTSettings::Duplication::Apply && constants.historyValid != 0u)
        {
            constants.flags |= FlagDuplicationApply;
            constants.previousDuplicationIndex = m_duplication[frame.previousSet].GetUAVDescriptorIndex();
        }
    }

    // Optimization experiment B1a: stagnancy decorrelation writes this frame's smoothed map in the final (resample) pass
    // and reads last frame's only with valid history; its factor replaces the Uniform probability.
    if(settings.decorrelationMode == ReSTIRPTSettings::DecorrelationMode::Stagnancy && settings.reservoir &&
        !directReservoir && m_stagnancy[0].GetResource() && m_stagnancy[1].GetResource())
    {
        constants.flags |= FlagStagnancy;
        constants.decorrelation = settings.stagnancyFactor;
        constants.stagnancyOutputIndex = m_stagnancy[frame.writeSet].GetUAVDescriptorIndex();

        if(constants.historyValid != 0u)
        {
            constants.stagnancyPreviousIndex = m_stagnancy[frame.previousSet].GetUAVDescriptorIndex();
        }
    }

    // Optimization experiment B1b: firefly replacement in the resample pass (temporal + spatial reuse).
    // Direct-reservoir mode keeps it: a reused direct firefly is replaced by the fresh direct estimate (the resample pass).
    if(settings.fireflyReplacement && settings.reservoir && settings.temporal && settings.spatial)
    {
        constants.flags |= FlagFirefly;
    }

    // M5.4 C5: replay compaction needs the separate temporal pass and the list.
    if(settings.reservoir && settings.temporal && temporalPass && settings.replayCompaction && m_replayList.GetResource())
    {
        constants.flags |= FlagReplayCompaction;
        constants.replayListIndex = m_replayList.GetUAVDescriptorIndex();
    }

    // M5.3: the initial/temporal kernel writes the scratch set; the resample pass writes this frame's set (committed as
    // history as before). Pairing transforms: an independent flip, mirror, transpose and offset per texture and frame
    // [P 3.2].
    if(spatial && settings.reservoir && m_spatialScratch.GetResource() && m_pairs.GetResource() &&
        m_pairing.GetResource() && writeReservoirs.GetResource())
    {
        constants.spatialScratchIndex = m_spatialScratch.GetUAVDescriptorIndex();
        constants.spatialOutputIndex = writeReservoirs.GetUAVDescriptorIndex();
        constants.reservoirIndex = constants.spatialScratchIndex;
        constants.pairsIndex = m_pairs.GetUAVDescriptorIndex();
        constants.pairingIndex = m_pairing.GetDescriptorIndex();
        constants.flags |= FlagSpatial | (settings.spatialColourNoiseReduction ? FlagSpatialColour : 0u) |
            (settings.spatialRoundTrip ? FlagSpatialRoundTrip : 0u);
        std::uint32_t firstTexel = 0;

        for(std::uint32_t t = 0; t < SpatialNeighbours; ++t)
        {
            const std::uint32_t size = PairingSizes[t];
            const std::uint64_t bits = SplitMix((std::uint64_t{frameIndex} << 2u) ^ (0x5350u + t) ^
                (std::uint64_t{t} << 40u));
            constants.pairingTransform[4 * t] = size | (static_cast<std::uint32_t>(bits & 7u) << 16u);
            constants.pairingTransform[4 * t + 1] = static_cast<std::uint32_t>((bits >> 8u) % size);
            constants.pairingTransform[4 * t + 2] = static_cast<std::uint32_t>((bits >> 32u) % size);
            constants.pairingTransform[4 * t + 3] = firstTexel;
            firstTexel += size * size;
        }
    }
    constants.tileCount = m_tileCount;
    constants.tileSize = m_tileSize;
    constants.primaryCandidates = settings.primaryCandidates;

    if(settings.rouletteFromVertex == ReSTIRPTSettings::NeverRoulette)
    {
        constants.rouletteFromVertex = 0;
    }
    else
    {
        // The production path tracer applies roulette after vertex v's continuation when v + 1 >= russianRouletteDepth,
        // v counted from 0 at the primary hit: paper vertex j = v + 1 >= russianRouletteDepth.
        constants.rouletteFromVertex = settings.rouletteFromVertex != 0 ? settings.rouletteFromVertex :
            (std::max)(productionRouletteDepth, 1u);
    }

    constants.frameIndex = frameIndex;
    constants.counterIndex = settings.counters && m_counters.GetResource() ? m_counters.GetUAVDescriptorIndex() : 0xFFFFFFFFu;
    constants.debugView = settings.debugView;
    constants.tableLightCount = tableLightCount;
    return constants;
}

void ReSTIRPTPass::PrepareInitialPass(ID3D12GraphicsCommandList* commandList)
{
    // Both sets: one is written, the other (with history) read through its raw UAV.
    for(Buffer& reservoirs : m_reservoirs)
    {
        if(reservoirs.GetResource())
        {
            reservoirs.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    // M5.3: the scratch reservoirs and pair records (UAV throughout).
    for(Buffer* spatial : {&m_spatialScratch, &m_pairs, &m_replayList, &m_instanceHistory[0], &m_instanceHistory[1],
        &m_duplication[0], &m_duplication[1], &m_stagnancy[0], &m_stagnancy[1]})
    {
        if(spatial->GetResource())
        {
            spatial->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    if(m_counters.GetResource())
    {
        m_counters.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}

std::uint64_t ReSTIRPTPass::GetResourceBytes() const
{
    std::uint64_t bytes = 0;

    if(m_lightTiles.GetResource())
    {
        bytes += std::uint64_t{m_tileCount} * m_tileSize * LightTileEntryBytes;
    }

    if(m_regirLights.GetResource())
    {
        bytes += std::uint64_t{ReGIRLightCapacity} * 12u;
    }

    if(m_counters.GetResource())
    {
        bytes += CounterCount * sizeof(std::uint32_t);
    }

    for(const Buffer& reservoirs : m_reservoirs)
    {
        if(reservoirs.GetResource())
        {
            bytes += std::uint64_t{m_width} * m_height * ReservoirBytes;
        }
    }

    if(m_spatialScratch.GetResource())
    {
        bytes += std::uint64_t{m_width} * m_height * ReservoirBytes;
    }

    if(m_pairs.GetResource())
    {
        bytes += std::uint64_t{m_width} * m_height * SpatialNeighbours * PairRecordBytes;
    }

    if(m_replayList.GetResource())
    {
        bytes += (std::uint64_t{m_width} * m_height + 4u) * sizeof(std::uint32_t);
    }

    for(const Buffer* perPixel : {&m_instanceHistory[0], &m_instanceHistory[1], &m_duplication[0], &m_duplication[1],
        &m_stagnancy[0], &m_stagnancy[1]})
    {
        if(perPixel->GetResource())
        {
            bytes += std::uint64_t{m_width} * m_height * sizeof(std::uint32_t);
        }
    }

    bytes += m_pairingTexels.size() * sizeof(std::uint32_t);

    return bytes;
}

#pragma once

#include "Math/Vector3.h"
#include "Math/Matrix4.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/DepthBuffer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/Pipeline.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/EnvironmentMap.h"
#include "Renderer/Passes/DiffuseIrradiancePass.h"
#include "Renderer/Passes/EnvironmentMipPass.h"
#include "Renderer/Passes/EquirectangularToCubemapPass.h"
#include "Renderer/Passes/GGXMultiscatterIrradiancePass.h"
#include "Renderer/StructuredBuffer.h"
#include "Renderer/Texture.h"
#include "Scene/PhysicalSkySettings.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <d3d12.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <wrl/client.h>

class ShaderLibrary;
class UploadBuffer;
struct SkyHdriTexels;

// Physical sky S1 (Documentation/Rendering/Physical_Sky_Design_Proposal.md §4.1-4.6): the renderer-owned atmosphere. It bakes a
// PhysicalSkySettings sky into an environment with exactly EnvironmentMap's interface (equirect radiance, importance CDFs, GGX
// cube, irradiance cube, GGX multiscatter cube), so every environment consumer lights from it unchanged.
//
// Residency (§4.4): every resource is allocated once, at renderer initialization in the Editor and in EngineRuntime whatever the
// scene's sky, and released only at Renderer::Shutdown after the final GPU drain: the transmittance and multiple-scattering LUTs
// and two complete environment sets, A and B. Scene activation, sky edits, sky type changes and streaming never create or release
// anything; descriptor indices are fixed for the session.
//
// Publication: a rebuild of the inactive set is recorded in the normal frame recordings, one stage per frame (time slicing, so no
// frame carries the whole rebuild):
//   1. Environment: [1]-[2] only when the atmosphere's parameters changed, [3] the equirect, [5a] its mips, [4] the CDFs;
//   2. Cubes: [5b] the irradiance cube and the GGX cube;
//   3. Multiscatter: [5b] the GGX multiscatter cube.
// The set is published (active and inactive swap, the generation advances) at the start of the frame after the last stage's
// submission was confirmed; nothing is retired, because the previous set is simply the next rebuild's target. All readers and the
// rebuild are on the one frame queue, so a set's last reads (as the active set) precede its next writes. A stage whose recording
// was discarded, failed or whose submission is uncertain drops the rebuild, which starts again from stage 1.
// A rebuild keeps the inputs it started with. Inputs that change meanwhile are taken up by the next rebuild, which starts as soon
// as this one publishes, so the newest request always wins and a continuous edit still publishes every few frames.
//
// S4, HDRI option A (§4.11): a set can hold an HDRI sky instead of the atmosphere. Its environment stage copies the HDRI's 1024x512
// texels (SkyHdriPreparation.h) from a persistent upload staging buffer into the equirect instead of running [1]-[3]; the mips, CDFs
// and cubes follow as for the physical sky, so importance sampling and every non-delta miss read the set as before. At the 2048
// and 4096 HDRI sky qualities a third, single-buffered background slot (an equirect at the quality and a GGX cube a quarter of its
// width) holds the same HDRI at the higher resolution for camera rays, delta misses and the reflection cube. It is built after the
// set publishes, time-sliced (4 MiB of texels per frame, then its mips, then the cube in rows), and used only once complete; until
// then readers use the set. All of it is allocated at initialization and released only at Shutdown.
class SkyAtmosphere
{
public:
    // What a bake depends on (§4.9).
    struct BakeInputs
    {
        PhysicalSkySettings settings;
        // FrameLighting::atmosphereSun: the direction its light travels and its irradiance above the atmosphere in render units.
        Vector3 sunDirection{0.0f, -1.0f, 0.0f};
        Vector3 sunIrradiance{0.0f, 0.0f, 0.0f};
        float referenceEV100 = 10.0f;
        // max(camera y - sea level, 0.5 m).
        double cameraAltitudeMeters = 0.5;
        // S5 (§14.1): the moon (FrameLighting::atmosphereMoon): the direction its light travels and its light above the atmosphere
        // in render units (zero without a moon).
        Vector3 moonDirection{0.0f, -1.0f, 0.0f};
        Vector3 moonIrradiance{0.0f, 0.0f, 0.0f};
        // S5 (§14.3): the sky clock the clouds' wind moves with (seconds), and the camera's world x and z (metres): the baked
        // clouds are the ones around the camera.
        double cloudTimeSeconds = 0.0;
        double cameraWorldX = 0.0;
        double cameraWorldZ = 0.0;
        // S4: the HDRI sky the set holds instead of the atmosphere (null: the physical sky). The settings and the sun are then
        // unused.
        std::shared_ptr<const SkyHdriTexels> hdri;
    };

    // The re-bake thresholds (§4.9). S1 re-bakes in full in one frame; time slicing is S4.
    static constexpr double SunTurnDegrees = 0.25;
    static constexpr double RelativeIrradianceChange = 0.01;
    static constexpr double AltitudeChangeMeters = 50.0;

    static constexpr unsigned int TransmittanceWidth = 256;
    static constexpr unsigned int TransmittanceHeight = 64;
    static constexpr unsigned int MultipleScatteringSize = 32;
    // S3 (§4.7): the sky-view map per set (the background for camera rays), and the aerial-perspective volume: Tiles x Tiles screen
    // tiles by Slices depth slices up to AerialMaxDistanceKm, as a 2D atlas.
    static constexpr unsigned int SkyViewWidth = 384;
    static constexpr unsigned int SkyViewHeight = 192;
    static constexpr unsigned int AerialTiles = 32;
    static constexpr unsigned int AerialSlices = 32;
    static constexpr float AerialMaxDistanceKm = 32.0f;

    SkyAtmosphere() = default;
    SkyAtmosphere(const SkyAtmosphere&) = delete;
    SkyAtmosphere& operator=(const SkyAtmosphere&) = delete;

    // S4: the persistent upload staging per frame slot (an HDRI set equirect, or one background chunk), at least this size.
    static constexpr std::size_t HdriStagingBytes = 4u << 20;

    // S4 (§4.12.5): the session's sizes, from the project settings (fixed until shutdown).
    struct ResourceSizes
    {
        // The sets' equirect width (512, 1024 or 2048; height is half) and GGX cube size (128, 256 or 512).
        unsigned int environmentWidth = 1024;
        unsigned int cubeSize = 256;
        // The HDRI sky quality (1024, 2048 or 4096); a background slot exists only when it is wider than the sets' equirect.
        unsigned int hdriBackgroundWidth = 1024;
        // The frame queue's slots (one staging buffer each).
        unsigned int frameCount = 2;
    };

    // Creates every resource, view and pipeline. On failure nothing stays allocated and the renderer lights from its S0.5
    // environment only (GetFailure says why).
    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& resourceHeap,
        const ResourceSizes& sizes = {});

    // The sets' equirect width (an HDRI sky's set texels are prepared at it).
    unsigned int GetEnvironmentWidth() const noexcept
    {
        return m_layout.width;
    }
    // After the renderer's final GPU drain.
    void Shutdown();

    bool IsInitialized() const noexcept
    {
        return m_initialized;
    }

    const std::string& GetFailure() const noexcept
    {
        return m_failure;
    }

    // Frame start, before anything reads the environment: a rebuild whose last stage's submission was confirmed becomes the
    // published set. A stage recorded without a confirmed submission drops the rebuild. Returns whether a new set was published.
    bool PublishSubmittedBake();

    // Whether this frame records a rebuild stage: a rebuild is in progress, or these inputs differ from the published bake by more
    // than the thresholds (always before the first bake).
    bool NeedsBake(const BakeInputs& inputs) const;

    // Records the next rebuild stage into the inactive set on the frame's command list; a new rebuild starts with these inputs.
    // Only after PublishSubmittedBake ran this frame. The command list's descriptor heaps, pipelines, viewports and render targets
    // are changed. frameIndex: the frame slot being recorded (its previous submission has completed).
    bool RecordBake(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, UploadBuffer& uploadBuffer,
        Texture& brdfIntegrationLUT, const BakeInputs& inputs, unsigned int frameIndex = 0);

    // S4: the next slice of the HDRI background slot's build, in a frame that recorded no rebuild stage. Nothing when the slot is
    // complete, absent (1024 quality) or the published set holds no HDRI.
    void RecordHdriBackground(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, unsigned int frameIndex);

    // After the frame's submission: executed confirms the recorded stage (the last one publishes next frame); otherwise the
    // rebuild is dropped, and the LUTs are treated as unknown if that stage wrote them. fenceValue: the frame queue's fence value
    // signalled by that submission (S5B: the irradiance read-back waits for it).
    void CommitSubmission(bool executed, std::uint64_t fenceValue = 0);

    // S5B: the irradiance-cube mean (luminance, render units) of the latest rebuild, once the GPU has completed its last stage
    // (completedFence: the frame queue's completed fence value). Returned once per rebuild; the renderer's lighting-cut test
    // compares it with the previous set's. Rebuilds publish the frame after their last stage, so the mean arrives a frame or two
    // after its set publishes.
    std::optional<double> TakeIrradianceMean(std::uint64_t completedFence);

    // Diagnostics, from environment variables read here (after Initialize; the frequency is the frame queue's):
    // - ENGINE_DIAG_SKY_PASS_TIMING: GPU timestamps around every pass of every rebuild; medians logged every 120 rebuilds.
    // - ENGINE_DIAG_SKY_FORCE_BAKE=sun|full: a bake every frame whatever changed (full also rebuilds the LUTs).
    void EnableDiagnostics(ID3D12Device* device, std::uint64_t timestampFrequency);

    bool HasPublished() const noexcept
    {
        return m_published;
    }

    // The published set (valid once HasPublished).
    EnvironmentMap& GetPublishedEnvironment() noexcept
    {
        return m_sets[m_publishedSet].environment;
    }

    // S3: the published set's sky-view map (DirectionToSkyViewUV), for camera-ray backgrounds.
    Texture& GetPublishedSkyView() noexcept
    {
        return m_sets[m_publishedSet].skyView;
    }

    // S4: whether the published set holds an HDRI sky (else the physical sky), and that HDRI.
    bool PublishedHdri() const noexcept;
    const SkyHdriTexels* GetPublishedHdri() const noexcept;
    // S4: the background slot's equirect and GGX cube when they hold the published HDRI completely, else null (readers then use the
    // published set's equirect and cube).
    Texture* GetHdriBackground() noexcept;
    Texture* GetHdriReflectionCube() noexcept;
    // Advances each time the background slot completes a build (it then replaces the set for the background and reflections).
    std::uint64_t GetHdriBackgroundGeneration() const noexcept
    {
        return m_backgroundGeneration;
    }
    // S4 residency: the background slot's equirect and cube, and the staging buffers (allocation sizes, bytes; 0 when absent).
    std::uint64_t GetHdriBackgroundBytes() const noexcept
    {
        return m_backgroundBytes;
    }
    std::uint64_t GetHdriStagingBytes() const noexcept
    {
        return m_stagingBytes;
    }
    unsigned int GetHdriBackgroundWidth() const noexcept
    {
        return m_background.width;
    }

    // S3 (§4.7): the aerial perspective of the published sky, composited into the HDR scene colour (scene * T + light) for
    // geometry pixels, before haze and mist. Builds the frame's volume for the camera, then draws a full-screen blend. The command
    // list's descriptor heaps, pipelines, viewports and render targets are changed; the depth's state is restored.
    // betweenParts, when given, is called after the volume (true) and after the composite (false), for timing.
    bool RecordAerialPerspective(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, UploadBuffer& uploadBuffer,
        RenderTarget& sceneColor, DepthBuffer& depth, const Matrix4& viewProjection, const Vector3& cameraPosition,
        double cameraAltitudeMeters, const std::function<void(bool)>& betweenParts = {});

    // S5 (§14.3): the clouds. The camera-cloud targets follow the output size (quarter resolution); they are created and
    // released with the renderer's other screen targets, after its GPU drain (0 releases them).
    bool ResizeClouds(ID3D12Device* device, DescriptorHeap& resourceHeap, unsigned int outputWidth, unsigned int outputHeight);

    // S5: per frame, after the rebuild stage and before any reader: the noise volumes (once), the cloud shadow plane and the frame
    // information buffer (SkyFrame.hlsli). active: the physical sky lights this frame (otherwise the buffer says so).
    struct FrameParameters
    {
        bool active = false;
        // World metres, the camera's altitude above sea level (metres), the sky clock (seconds).
        Vector3 cameraPosition{0.0f, 0.0f, 0.0f};
        double cameraAltitudeMeters = 0.5;
        double timeSeconds = 0.0;
        // The moon's light at the camera (render units, through the air) and its disk's angular radius (radians).
        Vector3 moonGroundIrradiance{0.0f, 0.0f, 0.0f};
        float moonAngularRadius = 0.0f;
        // The sun's disk's angular radius (radians): the clouds' march reaches its true transmittance around it.
        float sunAngularRadius = 0.0f;
        // The reference exposure's render scale (the stars' render units).
        double renderScale = 0.0;
    };
    void RecordFrame(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, UploadBuffer& uploadBuffer,
        const FrameParameters& frame);
    // The frame information buffer's descriptor while the physical sky lights the frame, else 0xFFFFFFFF.
    unsigned int GetFrameInfoIndex() const noexcept
    {
        return m_frameInfoActive ? m_frameInfo.GetDescriptorIndex() : 0xFFFFFFFFu;
    }

    // S5: whether the published set holds a physical sky with clouds.
    bool PublishedHasClouds() const noexcept;

    // S5: the camera clouds (one pixel of each 4x4 block traced, the rest reprojected) and their composite into the scene colour,
    // after the aerial perspective. betweenParts, when given, is called after the trace and reprojection (true) and after the
    // composite (false), for timing. False when there is nothing to draw.
    bool RecordClouds(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, UploadBuffer& uploadBuffer,
        RenderTarget& sceneColor, DepthBuffer& depth, const Matrix4& viewProjection, const Vector3& cameraPosition,
        double cameraAltitudeMeters, double timeSeconds, const std::function<void(bool)>& betweenParts = {});

    // S5 resources' sizes.
    static constexpr unsigned int CloudShapeNoiseSize = 128;
    static constexpr unsigned int CloudDetailNoiseSize = 32;
    static constexpr unsigned int CloudEnvironmentWidth = 256;
    static constexpr unsigned int CloudEnvironmentHeight = 128;
    static constexpr unsigned int CloudShadowSize = 512;
    static constexpr float CloudShadowExtentMeters = 40000.0f;
    // A rebuild follows the clouds once the wind or the camera has moved them this far (metres).
    static constexpr double CloudRebuildDistanceMeters = 100.0;
    static constexpr float StarDiskRadius = 0.0008f;

    // S4: an On Demand sky's next rebuild (any mode: the next frame starts one).
    void RequestUpdate() noexcept
    {
        m_updateRequested = true;
    }

    // Advances with every publication; the path-trace accumulation key includes it (in-place updates keep descriptor indices).
    std::uint64_t GetGeneration() const noexcept
    {
        return m_generation;
    }

    // The inputs of the published set (empty before the first publication).
    const std::optional<BakeInputs>& GetPublishedInputs() const noexcept
    {
        return m_publishedInputs;
    }

    // A conservative estimate of the relative change of the sky's radiance between two bakes, from their inputs alone: the
    // largest relative change of the sun's light above the atmosphere, the luminance factor and each medium's coefficients
    // (luminance-weighted). The sun's direction is excluded: a turning sun is a continuous change. Between an HDRI sky and any
    // other sky, or two different HDRIs: infinity (a replacement).
    static double RadianceChange(const BakeInputs& before, const BakeInputs& after);

private:
#if defined(ENGINE_HAS_VERIFICATION)
    // The Debug-only GPU verification reads the sets and LUTs to check residency.
    friend class SceneHierarchyVerification;
#endif
#if defined(ENGINE_HAS_S0_RELEASE_DRIVER)
    // Local Release diagnostics drivers (Git-ignored, diagnostics builds only) run inside Engine::Frame.
    friend class Engine;
#endif

    // The passes a rebuild times (ENGINE_DIAG_SKY_PASS_TIMING), in recording order; CubeConversion is a diagnostic extra.
    enum class TimedPass : unsigned int
    {
        Transmittance,
        MultipleScattering,
        Environment,
        EquirectMips,
        Cdf,
        Irradiance,
        CubeConversion,
        GgxCube,
        Multiscatter,
        Count
    };

    // ENGINE_DIAG_SKY_PASS_TIMING: a begin and an end timestamp per pass, one slot per rebuild, in a ring of Slots. A slot is read
    // when it comes round again, Slots rebuilds later: a rebuild spans at least three frames, so its frames have completed on the
    // GPU by then.
    struct PassTiming
    {
        static constexpr unsigned int Slots = 4;
        static constexpr unsigned int Passes = static_cast<unsigned int>(TimedPass::Count);
        static constexpr unsigned int Stamps = 2 * Passes;
        static constexpr std::size_t SamplesPerLog = 120;

        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        double ticksPerMillisecond = 0.0;
        unsigned int nextSlot = 0;
        // The slot the rebuild in progress writes, and the slots whose rebuilds executed and were not read yet.
        unsigned int currentSlot = 0;
        std::array<bool, Slots> executed{};
        std::vector<std::array<double, Passes>> samples;
    };

    enum class ForcedBake
    {
        None,
        Sun,
        Full
    };

    // One persistent environment set and the precompute passes bound to its render targets.
    struct EnvironmentSet
    {
        EnvironmentMap environment;
        // S3: the background for camera rays, baked with the environment.
        Texture skyView;
        EnvironmentMipPass mipPass;
        EquirectangularToCubemapPass cubemapPass;
        DiffuseIrradiancePass diffusePass;
        GGXMultiscatterIrradiancePass multiscatterPass;
        // S5: the set's cloud map (in-scatter, background transmittance), written by the rebuild's cloud stage.
        Texture cloudEnvironment;
    };

    // S5: a 3D noise volume (RGBA8), created once and generated on the GPU in the first frame.
    struct CloudVolume
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        unsigned int srv = 0xFFFFFFFFu;
        unsigned int uav = 0xFFFFFFFFu;
        unsigned int size = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };

    // S5: the camera clouds' screen targets (quarter resolution) and their history.
    struct CloudCamera
    {
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int traceWidth = 0;
        unsigned int traceHeight = 0;
        Texture trace;
        Texture traceDepth;
        std::array<Texture, 2> history;
        std::array<Texture, 2> historyDepth;
        // The history the next frame reprojects, and whether it holds a confirmed frame.
        unsigned int current = 0;
        bool valid = false;
        bool recorded = false;
        // The previous frame's camera-relative view-projection and camera position (the Sparse fix, 2026-11-25).
        Matrix4 previousRelativeViewProjection = Matrix4::Identity();
        Vector3 previousCameraPosition{0.0f, 0.0f, 0.0f};
        float previousReferenceEV100 = 0.0f;
        std::uint32_t frame = 0;
    };

    // S4: the background slot's build, in order. Progress advances only with confirmed submissions.
    enum class BackgroundStep
    {
        Empty,
        Upload,
        Mips,
        Cube,
        Ready
    };

    struct BackgroundProgress
    {
        BackgroundStep step = BackgroundStep::Empty;
        unsigned int uploadRow = 0;
        unsigned int cubeMip = 0;
        unsigned int cubeFace = 0;
        unsigned int cubeRow = 0;
    };

    struct HdriBackground
    {
        // 0: the 1024 quality, no slot.
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int cubeSize = 0;
        Texture equirect;
        Texture cube;
        EnvironmentMipPass mipPass;
        EquirectangularToCubemapPass cubemapPass;
        // The HDRI the slot holds or builds (the texels are kept while it is published, to rebuild after a lost submission).
        std::shared_ptr<const SkyHdriTexels> source;
        BackgroundProgress progress;
        // The progress the open frame's recording reaches, adopted when its submission is confirmed.
        std::optional<BackgroundProgress> recorded;
    };

    // The rebuild's stages, one per frame (see the class comment).
    enum class RebuildStage : unsigned int
    {
        // S5: a physical sky with clouds starts here (the LUTs when the atmosphere changed, the clouds' ambient light and the set's
        // cloud map); other rebuilds start at Environment.
        Clouds,
        Environment,
        Cubes,
        Multiscatter
    };

    enum class BakeState
    {
        Idle,
        // A stage was recorded in the open frame; waits for CommitSubmission.
        Recorded,
        // A stage's submission was confirmed and the next stage records next frame.
        Building,
        // The last stage's submission was confirmed; published by the next PublishSubmittedBake.
        Submitted
    };

    bool CreatePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* entryPoint, ComputePipeline& pipeline);
    bool CreateHdriResources(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& resourceHeap,
        unsigned int hdriBackgroundWidth, unsigned int frameCount);
    // The session's set layout (S4: from ResourceSizes).
    EnvironmentMap::PersistentSkyLayout m_layout;
    // The staging buffers' size (bytes per frame slot).
    std::size_t m_stagingSlotBytes = HdriStagingBytes;
    // S4: the HDRI set's environment stage: its texels through the frame slot's staging into mip 0, then [5a] and [4].
    void RecordHdriEnvironmentStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress, const SkyHdriTexels& hdri, unsigned int frameIndex);
    // [5a] and [4] of a set (both sky types).
    void RecordMipsAndCdf(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set, D3D12_GPU_VIRTUAL_ADDRESS constantsAddress);
    // S5: the clouds' persistent resources and pipelines; the [1]-[2] LUT update; the rebuild's cloud stage.
    bool CreateCloudResources(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& resourceHeap);
    bool CreateCloudPipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* entryPoint, ComputePipeline& pipeline);
    void RecordLuts(ID3D12GraphicsCommandList* commandList, D3D12_GPU_VIRTUAL_ADDRESS constantsAddress);
    void RecordCloudStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set, DescriptorHeap& resourceHeap,
        UploadBuffer& uploadBuffer, D3D12_GPU_VIRTUAL_ADDRESS constantsAddress, const BakeInputs& inputs, bool atmosphereChanged);
    // The first stage of a rebuild with these inputs.
    static RebuildStage FirstStage(const BakeInputs& inputs);
    static void TransitionVolume(ID3D12GraphicsCommandList* commandList, CloudVolume& volume, D3D12_RESOURCE_STATES state);
    // Restarts the background slot's build for the published HDRI when it holds another.
    void RestartHdriBackground();
    bool CreateAerialComposite(ID3D12Device* device, ShaderLibrary& shaderLibrary);
    // The rebuild stages' recordings (into the inactive set).
    void RecordEnvironmentStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set,
        D3D12_GPU_VIRTUAL_ADDRESS constantsAddress, bool atmosphereChanged);
    void RecordCubesStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set);
    void RecordMultiscatterStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set, Texture& brdfIntegrationLUT);
    // Abandons the rebuild in progress (its next start is from stage 1).
    void DropRebuild();
    // ENGINE_DIAG_SKY_PASS_TIMING: a pass's begin or end timestamp in the current slot (nothing when timing is off).
    void StampPass(ID3D12GraphicsCommandList* commandList, TimedPass pass, bool end) const;
    // Reads a pass-timing slot whose rebuild executed, and logs the medians when enough rebuilds were read.
    void ReadPassTiming(unsigned int slot);
    // Whether two settings give the same LUTs (the media, the planet and the ground albedo).
    static bool SameAtmosphere(const PhysicalSkySettings& left, const PhysicalSkySettings& right);
    void Fail(const std::string& message);

    std::array<EnvironmentSet, 2> m_sets;
    // Index of the published set; the other is baked. Starts at 1 so the first bake writes set 0.
    unsigned int m_publishedSet = 1;
    bool m_published = false;
    std::uint64_t m_generation = 0;

    Texture m_transmittanceLut;
    Texture m_multipleScatteringLut;
    // Render-target views of both sets' mips and cube faces, owned here so the renderer's shared RTV heap is not consumed.
    DescriptorHeap m_rtvHeap;

    ComputePipeline m_transmittancePipeline;
    ComputePipeline m_multipleScatteringPipeline;
    ComputePipeline m_environmentPipeline;
    ComputePipeline m_importanceRowsPipeline;
    ComputePipeline m_importanceMarginalPipeline;
    // The CDF build's per-row totals (doubles, as two words each), between its two dispatches.
    StructuredBuffer m_rowWeights;
    // S3: the sky-view kernel, the frame's aerial-perspective volume (written and read within the frame) and its composite.
    ComputePipeline m_skyViewPipeline;
    ComputePipeline m_aerialPipeline;
    Texture m_aerialVolume;
    Pipeline m_aerialCompositePipeline;

    BakeState m_bakeState = BakeState::Idle;
    // While Building, the next stage to record; while Recorded, the stage recorded in the open frame.
    RebuildStage m_stage = RebuildStage::Environment;
    std::optional<BakeInputs> m_publishedInputs;
    std::optional<BakeInputs> m_pendingInputs;
    // The atmosphere the LUTs hold (confirmed submissions only), and the one the pending recording writes, if it writes them.
    std::optional<PhysicalSkySettings> m_lutAtmosphere;
    std::optional<PhysicalSkySettings> m_pendingLutAtmosphere;

    bool m_initialized = false;
    std::string m_failure;

    // S5: clouds and the night sky (§14).
    CloudVolume m_shapeNoise;
    CloudVolume m_detailNoise;
    // 0 not generated, 1 recorded in the open frame, 2 generated.
    unsigned int m_noiseState = 0;
    Texture m_cloudAmbient;
    Texture m_cloudShadow;
    // S5B: the same plane along the moon (the moon's table light and its NEE read it), allocated once with the sun's.
    Texture m_cloudMoonShadow;
    // Diagnostics (Release sky checks): false leaves the per-pixel stars out of the frame information (a frame without them).
    bool m_starsForTesting = true;
    StructuredBuffer m_frameInfo;
    bool m_frameInfoActive = false;
    // RecordFrame's cone around the sun and the moon (SkyAtmosphereConstants::cloudDiskCosine; above 1: none).
    float m_cloudDiskCosine = 2.0f;
    CloudCamera m_cloudCamera;
    ComputePipeline m_cloudShapePipeline;
    ComputePipeline m_cloudDetailPipeline;
    ComputePipeline m_cloudAmbientPipeline;
    ComputePipeline m_cloudEnvironmentPipeline;
    ComputePipeline m_cloudShadowPipeline;
    ComputePipeline m_cloudTracePipeline;
    ComputePipeline m_cloudReprojectPipeline;
    Pipeline m_cloudCompositePipeline;
    ID3D12Device* m_device = nullptr;

    // S4: persistent upload staging, one per frame slot, persistently mapped; and the HDRI background slot.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_hdriStaging;
    std::vector<std::byte*> m_hdriStagingMapped;
    // The frame slot whose staging the open recording used (a rebuild stage or a background slice: at most one per frame).
    bool m_stagingRecorded = false;
    HdriBackground m_background;
    std::uint64_t m_backgroundGeneration = 0;
    std::uint64_t m_backgroundBytes = 0;
    std::uint64_t m_stagingBytes = 0;

    std::optional<PassTiming> m_passTiming;

    // S5 (ENGINE_DIAG_SKY_PASS_TIMING): the clouds' passes per frame: the shadow plane, the rebuild's cloud stage, the camera trace,
    // the reprojection and the composite (begin and end stamps each), in a ring of frames read Slots frames later.
    enum class CloudPass : unsigned int
    {
        Shadow,
        Stage,
        Trace,
        Reproject,
        Composite,
        Count
    };

    struct CloudTiming
    {
        static constexpr unsigned int Slots = 4;
        static constexpr unsigned int Passes = static_cast<unsigned int>(CloudPass::Count);
        static constexpr unsigned int Stamps = 2 * Passes;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        double ticksPerMillisecond = 0.0;
        std::uint64_t frame = 0;
        // Per slot, the passes its frame recorded (bit per pass), and whether that frame's submission was confirmed.
        std::array<std::uint32_t, Slots> recorded{};
        std::array<bool, Slots> executed{};
        // Per pass, every measured frame's milliseconds (read by the diagnostics; cleared by them).
        std::array<std::vector<double>, Passes> samples;
    };

    std::optional<CloudTiming> m_cloudTiming;
    // Brackets one cloud pass in the current frame's slot (nothing when timing is off).
    void StampCloudPass(ID3D12GraphicsCommandList* commandList, CloudPass pass, bool end);

    // S5B: the irradiance cube's read-back copy (one set's six faces, allocated once), recorded with a rebuild's last stage; the
    // copy's submission fence once confirmed (TakeIrradianceMean reads it when the GPU has passed it).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_irradianceReadback;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 6> m_irradianceFootprints{};
    DXGI_FORMAT m_irradianceFormat = DXGI_FORMAT_UNKNOWN;
    bool m_irradianceCopyRecorded = false;
    std::optional<std::uint64_t> m_irradianceCopyFence;
    bool CreateIrradianceReadback(ID3D12Device* device);
    void RecordIrradianceCopy(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set);
    ForcedBake m_forcedBake = ForcedBake::None;
    // S4 update modes: an explicit request, and when the last rebuild started (Realtime's period).
    bool m_updateRequested = false;
    std::chrono::steady_clock::time_point m_lastRebuildStart{};
};

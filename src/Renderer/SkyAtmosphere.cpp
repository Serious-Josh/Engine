#include "Renderer/SkyAtmosphere.h"

#include "Core/Log.h"
#include "Debug/MemoryRegistry.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/SkyHdriPreparation.h"
#include "Renderer/UploadBuffer.h"
#include "Scene/PhotometricUnits.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

namespace
{
    // Mirrors SkyAtmosphere.hlsl's SkyAtmosphereConstants (HLSL packing: each float3 shares its 16-byte register with the next
    // scalar). Distances in kilometres, coefficients per kilometre.
    struct SkyAtmosphereConstants
    {
        float bottomRadius;
        float topRadius;
        float rayleighDensityScale;
        float mieDensityScale;

        float rayleighScattering[3];
        float miePhaseG;

        float rayleighExtinction[3];
        float ozoneCenter;

        float mieScattering[3];
        float ozoneHalfWidth;

        float mieExtinction[3];
        float multipleScatteringFactor;

        float ozoneAbsorption[3];
        float cameraAltitude;

        float groundAlbedo[3];
        float skyLuminanceFactor;

        float directionToSun[3];
        std::uint32_t showGroundBelowHorizon;

        float sunIrradiance[3];
        std::uint32_t sampleBelowHorizon;

        std::uint32_t transmittanceWidth;
        std::uint32_t transmittanceHeight;
        std::uint32_t multipleScatteringSize;
        std::uint32_t environmentWidth;

        std::uint32_t environmentHeight;
        std::uint32_t transmittanceIndex;
        std::uint32_t transmittanceOutputIndex;
        std::uint32_t multipleScatteringIndex;

        std::uint32_t multipleScatteringOutputIndex;
        std::uint32_t environmentIndex;
        std::uint32_t environmentOutputIndex;
        std::uint32_t conditionalCdfIndex;

        std::uint32_t marginalCdfIndex;
        std::uint32_t rowWeightsIndex;
        std::uint32_t skyViewOutputIndex;
        std::uint32_t skyViewWidth;

        std::uint32_t skyViewHeight;
        std::uint32_t aerialOutputIndex;
        float aerialMaxDistance;
        std::uint32_t aerialSlices;

        Matrix4 aerialInverseViewProjection;

        float aerialCameraPosition[3];
        float aerialPadding;

        float horizonTint[3];
        float colorSaturation;

        float zenithTint[3];
        float horizonZenithShift;

        // S5 (§14.1): the moon (single scattering only) and the night sky.
        float directionToMoon[3];
        std::uint32_t moonEnabled;

        float moonIrradiance[3];
        // The airglow's zenith radiance and the stars' mean radiance, in render units (cd/m^2 x the render scale x the intensity).
        float airglowRadiance;

        float starMeanRadiance;
        // Radians about +Y.
        float planetRotation;
        // S5 (§14.3): the set's cloud environment map (in-scatter, background transmittance), composited by CSEnvironment.
        std::uint32_t cloudEnvironmentIndex;
        std::uint32_t cloudEnvironmentWidth;

        std::uint32_t cloudEnvironmentHeight;
        // Sun behind clouds (2026-11-25): the cosine of the cone around the sun and the moon in which the clouds' march carries on
        // past its early-out (above 1: none).
        float cloudDiskCosine;
        std::uint32_t nightPadding[2];
    };

    static_assert(sizeof(SkyAtmosphereConstants) == 400, "SkyAtmosphereConstants must match the HLSL cbuffer layout.");

    // S5: the star field's mean irradiance over the sphere, lux (the distribution SkyFrame.hlsli draws: about 9,000 stars to
    // magnitude 6.5, N(<m) proportional to 10^(0.43 m), E(m) = 2.54e-6 * 10^(-0.4 m) lux), as mean radiance cd/m^2.
    constexpr double StarFieldMeanLuminance = 3.08e-5;

    // S5: SkyAtmosphere.hlsl's SkyCloudConstants (b1) and one layer of it.
    struct SkyCloudLayerConstants
    {
        float bottomRadius;
        float topRadius;
        float coverage;
        float density;

        float shapeFrequency;
        float detailFrequency;
        float erosion;
        float shapeFactor;

        float albedo[3];
        float multipleScattering;

        float phaseForward;
        float phaseBackward;
        float phaseBlend;
        float ambientStrength;

        float windOffset[2];
        std::uint32_t kind;
        std::uint32_t castShadows;
    };

    static_assert(sizeof(SkyCloudLayerConstants) == 80, "SkyCloudLayer must match the HLSL cbuffer layout.");

    struct SkyCloudConstants
    {
        SkyCloudLayerConstants layers[4];

        std::uint32_t layerCount;
        std::uint32_t shapeNoiseIndex;
        std::uint32_t detailNoiseIndex;
        std::uint32_t ambientIndex;

        float cameraWorldKm[2];
        std::uint32_t frame;
        std::uint32_t historyValid;

        // Camera-relative (the camera at the origin; the Sparse fix, 2026-11-25).
        Matrix4 inverseViewProjection;
        Matrix4 previousViewProjection;

        // The camera's move since the previous frame (metres).
        float cameraMove[3];
        float historyScale;

        std::uint32_t traceWidth;
        std::uint32_t traceHeight;
        std::uint32_t width;
        std::uint32_t height;

        std::uint32_t traceOutputIndex;
        std::uint32_t traceDepthOutputIndex;
        std::uint32_t historyIndex;
        std::uint32_t historyDepthIndex;

        std::uint32_t outputIndex;
        std::uint32_t depthOutputIndex;
        std::uint32_t traceIndex;
        std::uint32_t traceDepthIndex;

        float shadowOrigin[2];
        float shadowExtent;
        float shadowReferenceY;

        std::uint32_t shadowOutputIndex;
        std::uint32_t shadowSize;
        std::uint32_t noiseOutputIndex;
        std::uint32_t noiseSize;

        std::uint32_t environmentOutputIndex;
        std::uint32_t environmentOutputWidth;
        std::uint32_t environmentOutputHeight;
        float seaLevel;
    };

    static_assert(sizeof(SkyCloudConstants) == 592, "SkyCloudConstants must match the HLSL cbuffer layout.");

    // SkyCloudComposite.hlsl's constants.
    struct CloudCompositeConstants
    {
        Matrix4 inverseViewProjection;

        float cameraPosition[3];
        std::uint32_t depthIndex;

        std::uint32_t cloudIndex;
        std::uint32_t cloudDepthIndex;
        std::uint32_t padding[2];
    };

    static_assert(sizeof(CloudCompositeConstants) == 96, "CloudCompositeConstants must match the HLSL cbuffer layout.");

    // SkyFrame.hlsli's SkyFrameInfoData (a StructuredBuffer element: tightly packed).
    struct SkyFrameInfoData
    {
        std::uint32_t flags;
        std::uint32_t transmittanceIndex;
        std::uint32_t transmittanceWidth;
        std::uint32_t transmittanceHeight;

        float bottomRadius;
        float topRadius;
        float cameraAltitude;
        float starRadiance;

        float planetRotation;
        float starDiskRadius;
        std::uint32_t cloudShadowIndex;
        std::uint32_t cloudEnvironmentIndex;

        float cloudShadowOrigin[2];
        float cloudShadowInvExtent;
        float cloudShadowReferenceY;

        float directionToSun[3];
        std::uint32_t cloudShadowSize;

        float directionToMoon[3];
        float moonCosRadius;

        float moonDiskRadiance[3];
        std::uint32_t cloudEnvironmentWidth;

        std::uint32_t cloudEnvironmentHeight;
        // S5B.
        std::uint32_t moonCloudShadowIndex;
        std::uint32_t padding[2];
    };

    static_assert(sizeof(SkyFrameInfoData) == 128, "SkyFrameInfoData must match SkyFrame.hlsli.");

    // SkyAerialPerspective.hlsl's composite constants.
    struct AerialCompositeConstants
    {
        Matrix4 inverseViewProjection;

        float cameraPosition[3];
        std::uint32_t depthIndex;

        std::uint32_t volumeIndex;
        std::uint32_t slices;
        std::uint32_t tiles;
        float maxDistance;
    };

    static_assert(sizeof(AerialCompositeConstants) == 96, "AerialCompositeConstants must match the HLSL cbuffer layout.");

    constexpr D3D12_RESOURCE_STATES ShaderRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Render-target views one set's passes create: equirect mips 1..N, six faces of every cube mip, the irradiance faces and six
    // faces of every multiscatter mip.
    unsigned int RenderTargetViewsPerSet(const EnvironmentMap::PersistentSkyLayout& layout)
    {
        const unsigned int equirectMips = EnvironmentMap::SourceMipLevels(layout.width, layout.height);
        return (equirectMips - 1) + 6 * layout.cubemapMipLevels + 6 + 6 * layout.multiscatterMipLevels;
    }

    // S4: the HDRI background slot at a quality (0 at 1024): its equirect's mips and its cube's faces, and the cube's mip count.
    unsigned int BackgroundCubeMipLevels(unsigned int width)
    {
        return EnvironmentMap::SourceMipLevels(width / 4, width / 4);
    }

    unsigned int BackgroundRenderTargetViews(unsigned int width, unsigned int setWidth)
    {
        if(width <= setWidth)
        {
            return 0;
        }

        return (EnvironmentMap::SourceMipLevels(width, width / 2) - 1) + 6 * BackgroundCubeMipLevels(width);
    }

    // The background cube's per-frame budget, estimated with S0.5's precompute cost model (texel-samples at its sustained rate),
    // and the sky variant's samples per texel (SkyEquirectangularToCubemap.hlsl: mip 0 copies, the terminal mip averages 2,048
    // directions, every other mip prefilters with 256).
    // Measured on the RTX 4080 (S4 hdriswitch): the slices cost about 1.8x this estimate, so 400 estimated is about 0.7 ms.
    constexpr double BackgroundCubeMicrosecondsPerFrame = 400.0;
    constexpr double SamplesPerMicrosecond = 64000.0;

    std::uint64_t BackgroundCubeSamplesPerTexel(unsigned int mip, unsigned int mipLevels)
    {
        if(mip == 0)
        {
            return 1;
        }

        return mip + 1 == mipLevels ? 2048 : 256;
    }

    // Copies `rows` RGBA16F rows of `width` texels, starting at row `firstRow` of the texels, through a staging buffer into mip 0
    // of a texture (already in COPY_DEST).
    void CopyRowsThroughStaging(ID3D12GraphicsCommandList* commandList, ID3D12Resource* staging, std::byte* mapped,
        const std::uint16_t* texels, unsigned int width, unsigned int firstRow, unsigned int rows, ID3D12Resource* destination)
    {
        const std::size_t rowBytes = std::size_t(width) * 4 * sizeof(std::uint16_t);
        std::memcpy(mapped, texels + std::size_t(firstRow) * width * 4, rowBytes * rows);

        D3D12_TEXTURE_COPY_LOCATION target{};
        target.pResource = destination;
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = staging;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = 0;
        source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        source.PlacedFootprint.Footprint.Width = width;
        source.PlacedFootprint.Footprint.Height = rows;
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch = UINT(rowBytes);
        commandList->CopyTextureRegion(&target, 0, firstRow, 0, &source, nullptr);
    }

    std::uint64_t AllocationBytes(ID3D12Device* device, ID3D12Resource* resource)
    {
        if(!device || !resource)
        {
            return 0;
        }

        const D3D12_RESOURCE_DESC description = resource->GetDesc();
        return device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    }

    void CopyChannels(const std::array<float, 3>& source, float (&target)[3], double scale)
    {
        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            target[channel] = float(double(source[channel]) * scale);
        }
    }

    void CopyChannelSums(const std::array<float, 3>& first, const std::array<float, 3>& second, float (&target)[3], double scale)
    {
        for(std::size_t channel = 0; channel < 3; ++channel)
        {
            target[channel] = float((double(first[channel]) + double(second[channel])) * scale);
        }
    }

    // The direction toward the sun, from the direction its light travels; straight up for a degenerate direction.
    Vector3 DirectionToSun(const Vector3& lightDirection)
    {
        const double length = std::sqrt(double(lightDirection.x) * lightDirection.x + double(lightDirection.y) * lightDirection.y +
            double(lightDirection.z) * lightDirection.z);

        if(!(length > 0.0) || !std::isfinite(length))
        {
            return {0.0f, 1.0f, 0.0f};
        }

        return {float(-lightDirection.x / length), float(-lightDirection.y / length), float(-lightDirection.z / length)};
    }

    // The largest channel's change relative to the larger value's largest channel (0 when both are black).
    double RelativeChange(const Vector3& left, const Vector3& right)
    {
        const double scale = (std::max)({std::abs(double(left.x)), std::abs(double(left.y)), std::abs(double(left.z)),
            std::abs(double(right.x)), std::abs(double(right.y)), std::abs(double(right.z))});
        const double change = (std::max)({std::abs(double(left.x) - right.x), std::abs(double(left.y) - right.y),
            std::abs(double(left.z) - right.z)});
        return scale > 0.0 ? change / scale : 0.0;
    }

    SkyAtmosphereConstants MakeConstants(const SkyAtmosphere::BakeInputs& inputs, const EnvironmentMap::PersistentSkyLayout& layout)
    {
        const PhysicalSkySettings& sky = inputs.settings;
        // The settings store coefficients per metre; the shaders work per kilometre.
        constexpr double PerKilometre = 1000.0;

        SkyAtmosphereConstants constants{};
        constants.bottomRadius = sky.planetRadiusKm;
        constants.topRadius = float(double(sky.planetRadiusKm) + double(sky.atmosphereHeightKm));
        constants.rayleighDensityScale = float(1.0 / double(sky.rayleighScaleHeightKm));
        constants.mieDensityScale = float(1.0 / double(sky.mieScaleHeightKm));
        CopyChannels(sky.rayleighScattering, constants.rayleighScattering, PerKilometre);
        CopyChannelSums(sky.rayleighScattering, sky.rayleighAbsorption, constants.rayleighExtinction, PerKilometre);
        CopyChannels(sky.mieScattering, constants.mieScattering, PerKilometre);
        CopyChannelSums(sky.mieScattering, sky.mieAbsorption, constants.mieExtinction, PerKilometre);
        CopyChannels(sky.ozoneAbsorption, constants.ozoneAbsorption, PerKilometre);
        constants.miePhaseG = sky.mieAnisotropy;
        // HDRP's tent: it starts at the minimum altitude and peaks at half the layer width.
        constants.ozoneHalfWidth = 0.5f * sky.ozoneLayerWidthKm;
        constants.ozoneCenter = sky.ozoneMinimumAltitudeKm + constants.ozoneHalfWidth;
        constants.multipleScatteringFactor = sky.multipleScattering;
        CopyChannels(sky.groundAlbedo, constants.groundAlbedo, 1.0);
        constants.cameraAltitude = float(inputs.cameraAltitudeMeters / 1000.0);
        constants.skyLuminanceFactor = float(PhysicalSkyLuminanceFactor(sky));

        const Vector3 toSun = DirectionToSun(inputs.sunDirection);
        constants.directionToSun[0] = toSun.x;
        constants.directionToSun[1] = toSun.y;
        constants.directionToSun[2] = toSun.z;
        constants.sunIrradiance[0] = inputs.sunIrradiance.x;
        constants.sunIrradiance[1] = inputs.sunIrradiance.y;
        constants.sunIrradiance[2] = inputs.sunIrradiance.z;
        constants.showGroundBelowHorizon = sky.showGroundBelowHorizon ? 1u : 0u;
        constants.sampleBelowHorizon = sky.sampleBelowHorizon ? 1u : 0u;

        constants.transmittanceWidth = SkyAtmosphere::TransmittanceWidth;
        constants.transmittanceHeight = SkyAtmosphere::TransmittanceHeight;
        constants.multipleScatteringSize = SkyAtmosphere::MultipleScatteringSize;
        constants.environmentWidth = layout.width;
        constants.environmentHeight = layout.height;
        constants.skyViewWidth = SkyAtmosphere::SkyViewWidth;
        constants.skyViewHeight = SkyAtmosphere::SkyViewHeight;
        constants.aerialMaxDistance = SkyAtmosphere::AerialMaxDistanceKm;
        constants.aerialSlices = SkyAtmosphere::AerialSlices;
        constants.aerialInverseViewProjection = Matrix4::Identity();
        CopyChannels(sky.horizonTint, constants.horizonTint, 1.0);
        CopyChannels(sky.zenithTint, constants.zenithTint, 1.0);
        constants.colorSaturation = sky.colorSaturation;
        constants.horizonZenithShift = sky.horizonZenithShift;

        // S5: the moon and the night sky (render units at the inputs' reference exposure).
        const Vector3 toMoon = DirectionToSun(inputs.moonDirection);
        constants.directionToMoon[0] = toMoon.x;
        constants.directionToMoon[1] = toMoon.y;
        constants.directionToMoon[2] = toMoon.z;
        constants.moonIrradiance[0] = inputs.moonIrradiance.x;
        constants.moonIrradiance[1] = inputs.moonIrradiance.y;
        constants.moonIrradiance[2] = inputs.moonIrradiance.z;
        constants.moonEnabled = (inputs.moonIrradiance.x > 0.0f || inputs.moonIrradiance.y > 0.0f || inputs.moonIrradiance.z > 0.0f) ?
            1u : 0u;
        const double renderScale = Photometry::RenderScale(double(inputs.referenceEV100));
        constants.airglowRadiance = float(double(sky.airglowLuminance) * renderScale * double(sky.nightSkyIntensity));
        constants.starMeanRadiance = float(StarFieldMeanLuminance * renderScale * double(sky.nightSkyIntensity));
        constants.planetRotation = float(double(sky.planetRotationDegrees) * 3.14159265358979323846 / 180.0);
        constants.cloudEnvironmentIndex = 0xFFFFFFFFu;
        constants.cloudDiskCosine = 2.0f;
        return constants;
    }

    void UnorderedAccessBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        commandList->ResourceBarrier(1, &barrier);
    }

    // The CDF buffers live in COMMON between bakes, as an HDRI's do (readers promote them implicitly); a bake writes them as
    // unordered access and returns them to COMMON.
    void TransitionBuffers(ID3D12GraphicsCommandList* commandList, std::initializer_list<ID3D12Resource*> buffers,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        for(ID3D12Resource* buffer : buffers)
        {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = buffer;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            commandList->ResourceBarrier(1, &barrier);
        }
    }

    unsigned int GroupCount(unsigned int size, unsigned int groupSize)
    {
        return (size + groupSize - 1) / groupSize;
    }
    // The clouds' constants for a sky at a time and camera (world metres); the passes' indices are the caller's.
    SkyCloudConstants MakeCloudConstants(const PhysicalSkySettings& sky, double timeSeconds, double cameraX, double cameraZ)
    {
        SkyCloudConstants constants{};
        constants.layerCount = static_cast<std::uint32_t>((std::min)(sky.cloudLayers.size(), std::size_t(4)));
        constants.cameraWorldKm[0] = float(cameraX / 1000.0);
        constants.cameraWorldKm[1] = float(cameraZ / 1000.0);
        constants.inverseViewProjection = Matrix4::Identity();
        constants.previousViewProjection = Matrix4::Identity();
        constants.historyScale = 1.0f;
        constants.seaLevel = sky.seaLevelMeters;

        for(std::uint32_t index = 0; index < constants.layerCount; ++index)
        {
            const CloudLayerSettings& layer = sky.cloudLayers[index];
            SkyCloudLayerConstants& target = constants.layers[index];
            const bool sheet = layer.kind == CloudLayerKind::Layer;
            target.bottomRadius = float(double(sky.planetRadiusKm) + double(layer.altitudeKm));
            target.topRadius = float(double(sky.planetRadiusKm) + double(layer.altitudeKm) + (sheet ? 0.0 : double(layer.thicknessKm)));
            target.coverage = layer.coverage;
            target.density = layer.density;
            target.shapeFrequency = 1.0f / layer.shapeScaleKm;
            target.detailFrequency = 1.0f / layer.detailScaleKm;
            target.erosion = layer.erosion;
            target.shapeFactor = layer.shapeFactor;
            CopyChannels(layer.albedo, target.albedo, 1.0);
            target.multipleScattering = layer.multipleScattering;
            target.phaseForward = layer.phaseForward;
            target.phaseBackward = layer.phaseBackward;
            target.phaseBlend = layer.phaseBlend;
            target.ambientStrength = layer.ambientStrength;
            // The noise moves with the wind: the pattern at x at time t is the pattern at x - v t at time 0 (km).
            const double angle = double(layer.windDirectionDegrees) * 3.14159265358979323846 / 180.0;
            const double travelled = double(layer.windSpeedMetersPerSecond) * timeSeconds / 1000.0;
            target.windOffset[0] = float(-travelled * std::cos(angle));
            target.windOffset[1] = float(-travelled * std::sin(angle));
            target.kind = sheet ? 0u : 1u;
            target.castShadows = layer.castShadows ? 1u : 0u;
        }

        return constants;
    }
}

void SkyAtmosphere::Fail(const std::string& message)
{
    m_failure = message;
    Log::Error("Physical sky unavailable: " + message);
}

bool SkyAtmosphere::CreatePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* entryPoint,
    ComputePipeline& pipeline)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "SkyAtmosphere.hlsl";
    shaderDescription.entryPoint = entryPoint;
    shaderDescription.stage = ShaderStage::Compute;
    Shader* shader = shaderLibrary.Load(shaderDescription);

    if(!shader)
    {
        return false;
    }

    ComputePipelineDescription description{};
    description.computeShader = shader;

    RootParameterDescription constants{};
    constants.type = RootParameterType::ConstantBuffer;
    constants.shaderRegister = 0;
    constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
    description.rootParameters.push_back(constants);

    // The LUT coordinates are remapped to texel centres, so a linear clamped sampler reads them exactly.
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    description.staticSamplers.push_back(sampler);

    return pipeline.Initialize(device, description);
}

bool SkyAtmosphere::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& resourceHeap,
    const ResourceSizes& sizes)
{
    unsigned int hdriBackgroundWidth = sizes.hdriBackgroundWidth;
    const unsigned int frameCount = sizes.frameCount;
    if(m_initialized)
    {
        return true;
    }

    m_failure.clear();
    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);

    if(!device)
    {
        Fail("no device.");
        return false;
    }

    // Stage 1: the render-target views of both sets, in a heap of their own.
    if(hdriBackgroundWidth != 1024 && hdriBackgroundWidth != 2048 && hdriBackgroundWidth != 4096)
    {
        Log::Warning("Physical sky: invalid HDRI sky quality " + std::to_string(hdriBackgroundWidth) + "; using 2048.");
        hdriBackgroundWidth = 2048;
    }

    // S4 (§4.12.5): the sets' sizes for the session.
    m_layout = EnvironmentMap::PersistentSkyLayout{};
    const bool validWidth = sizes.environmentWidth == 512 || sizes.environmentWidth == 1024 || sizes.environmentWidth == 2048;
    const bool validCube = sizes.cubeSize == 128 || sizes.cubeSize == 256 || sizes.cubeSize == 512;

    if(!validWidth || !validCube)
    {
        Log::Warning("Physical sky: invalid environment size " + std::to_string(sizes.environmentWidth) + " or cube size " +
            std::to_string(sizes.cubeSize) + "; using 1024 and 256.");
    }

    m_layout.width = validWidth ? sizes.environmentWidth : 1024u;
    m_layout.height = m_layout.width / 2;
    m_layout.cubemapSize = validCube ? sizes.cubeSize : 256u;
    m_layout.cubemapMipLevels = EnvironmentMap::SourceMipLevels(m_layout.cubemapSize, m_layout.cubemapSize);
    m_stagingSlotBytes = (std::max)(HdriStagingBytes, std::size_t(m_layout.width) * m_layout.height * 4 * sizeof(std::uint16_t));

    if(!m_rtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        2 * RenderTargetViewsPerSet(m_layout) + BackgroundRenderTargetViews(hdriBackgroundWidth, m_layout.width), false))
    {
        Fail("its render-target view heap could not be created.");
        return false;
    }

    // Stage 2: the LUTs, written by unordered access and read by the bake.
    const bool lutsCreated =
        m_transmittanceLut.Initialize(device, TransmittanceWidth, TransmittanceHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
        m_transmittanceLut.CreateSRV(device, resourceHeap) && m_transmittanceLut.CreateUAV(device, resourceHeap) &&
        m_multipleScatteringLut.Initialize(device, MultipleScatteringSize, MultipleScatteringSize, 1, 1,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
        m_multipleScatteringLut.CreateSRV(device, resourceHeap) && m_multipleScatteringLut.CreateUAV(device, resourceHeap) &&
        m_rowWeights.InitializeUnorderedStorage(device, 2 * sizeof(std::uint32_t), m_layout.height, resourceHeap) &&
        m_aerialVolume.Initialize(device, AerialTiles * AerialSlices, AerialTiles, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
        m_aerialVolume.CreateSRV(device, resourceHeap) && m_aerialVolume.CreateUAV(device, resourceHeap);

    if(!lutsCreated)
    {
        Fail("its LUTs could not be created.");
        Shutdown();
        return false;
    }

    // Stage 3: the two environment sets and their precompute passes. The cube passes use the sky's sample counts (the Sky*.hlsl
    // variants of the HDRI path's shaders).
    for(EnvironmentSet& set : m_sets)
    {
        EnvironmentMap& environment = set.environment;
        const bool setCreated = environment.InitializePersistentSky(device, resourceHeap, m_layout) &&
            set.skyView.Initialize(device, SkyViewWidth, SkyViewHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
            set.skyView.CreateSRV(device, resourceHeap) && set.skyView.CreateUAV(device, resourceHeap) &&
            set.mipPass.Initialize(device, shaderLibrary, m_rtvHeap, environment.GetSourceTexture()) &&
            set.cubemapPass.Initialize(device, shaderLibrary, m_rtvHeap, environment.GetCubemap(),
                "SkyEquirectangularToCubemap.hlsl") &&
            set.diffusePass.Initialize(device, shaderLibrary, m_rtvHeap, environment.GetDiffuseIrradiance(),
                "SkyDiffuseIrradiance.hlsl") &&
            set.multiscatterPass.Initialize(device, shaderLibrary, m_rtvHeap, environment.GetGGXMultiscatterIrradiance(),
                "SkyGGXMultiscatterIrradiance.hlsl");

        if(!setCreated)
        {
            Fail("an environment set or its precompute passes could not be created.");
            Shutdown();
            return false;
        }
    }

    // Stage 4: the bake's compute pipelines.
    const bool pipelinesCreated = CreatePipeline(device, shaderLibrary, "CSTransmittance", m_transmittancePipeline) &&
        CreatePipeline(device, shaderLibrary, "CSMultipleScattering", m_multipleScatteringPipeline) &&
        CreatePipeline(device, shaderLibrary, "CSEnvironment", m_environmentPipeline) &&
        CreatePipeline(device, shaderLibrary, "CSImportanceRows", m_importanceRowsPipeline) &&
        CreatePipeline(device, shaderLibrary, "CSImportanceMarginal", m_importanceMarginalPipeline) &&
        CreatePipeline(device, shaderLibrary, "CSSkyView", m_skyViewPipeline) &&
        CreatePipeline(device, shaderLibrary, "CSAerialPerspective", m_aerialPipeline) &&
        CreateAerialComposite(device, shaderLibrary);

    if(!pipelinesCreated)
    {
        Fail("its compute pipelines could not be created.");
        Shutdown();
        return false;
    }

    // Stage 5 (S4): the HDRI staging and background slot.
    if(!CreateHdriResources(device, shaderLibrary, resourceHeap, hdriBackgroundWidth, frameCount))
    {
        Fail("its HDRI staging or background slot could not be created.");
        Shutdown();
        return false;
    }

    // Stage 6 (S5): the clouds' volumes, maps, shadow plane, frame information buffer and pipelines.
    if(!CreateCloudResources(device, shaderLibrary, resourceHeap))
    {
        Fail("its cloud resources could not be created.");
        Shutdown();
        return false;
    }

    // Stage 7 (S5B): the irradiance cube's read-back buffer (the lighting-cut test).
    if(!CreateIrradianceReadback(device))
    {
        Fail("its irradiance read-back buffer could not be created.");
        Shutdown();
        return false;
    }

    m_initialized = true;
    Log::Info("Physical sky resources allocated for the session: two " + std::to_string(m_layout.width) + "x" +
        std::to_string(m_layout.height) + " environment sets, " + std::to_string(m_layout.cubemapSize) + " cube, LUTs " +
        std::to_string(TransmittanceWidth) + "x" + std::to_string(TransmittanceHeight) + " and " +
        std::to_string(MultipleScatteringSize) + "x" + std::to_string(MultipleScatteringSize) + ".");
    char residency[256];
    std::snprintf(residency, sizeof(residency),
        "HDRI sky quality %u: background slot %s (%.2f MiB), upload staging %zu x %.0f MiB (%.2f MiB, upload heap).",
        hdriBackgroundWidth, m_background.width ? (std::to_string(m_background.width) + "x" + std::to_string(m_background.height) +
        " equirect + " + std::to_string(m_background.cubeSize) + " cube").c_str() : "none (the sets serve it)",
        double(m_backgroundBytes) / double(1u << 20), m_hdriStaging.size(), double(m_stagingSlotBytes) / double(1u << 20),
        double(m_stagingBytes) / double(1u << 20));
    Log::Info(residency);
    return true;
}

bool SkyAtmosphere::CreateHdriResources(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& resourceHeap,
    unsigned int hdriBackgroundWidth, unsigned int frameCount)
{
    // Upload heap, one buffer per frame slot, mapped for the session (the CPU writes only a slot whose previous submission has
    // completed).
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = m_stagingSlotBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    m_hdriStaging.assign((std::max)(frameCount, 1u), nullptr);
    m_hdriStagingMapped.assign(m_hdriStaging.size(), nullptr);
    m_stagingBytes = 0;

    for(std::size_t slot = 0; slot < m_hdriStaging.size(); ++slot)
    {
        const D3D12_RANGE noRead{0, 0};

        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_hdriStaging[slot]))) ||
            FAILED(m_hdriStaging[slot]->Map(0, &noRead, reinterpret_cast<void**>(&m_hdriStagingMapped[slot]))))
        {
            return false;
        }

        MemoryRegistry::TrackResource(m_hdriStaging[slot].Get(), MemoryCategory::UploadReadback);
        m_stagingBytes += AllocationBytes(device, m_hdriStaging[slot].Get());
    }

    if(hdriBackgroundWidth <= m_layout.width)
    {
        return true;
    }

    m_background.width = hdriBackgroundWidth;
    m_background.height = hdriBackgroundWidth / 2;
    m_background.cubeSize = hdriBackgroundWidth / 4;
    const bool created =
        m_background.equirect.Initialize(device, m_background.width, m_background.height, 1,
            EnvironmentMap::SourceMipLevels(m_background.width, m_background.height), DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, ShaderRead) &&
        m_background.equirect.CreateSRV(device, resourceHeap) &&
        m_background.cube.Initialize(device, m_background.cubeSize, m_background.cubeSize, 6,
            BackgroundCubeMipLevels(m_background.width), DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            ShaderRead) &&
        m_background.cube.CreateCubeSRV(device, resourceHeap) &&
        m_background.mipPass.Initialize(device, shaderLibrary, m_rtvHeap, m_background.equirect) &&
        m_background.cubemapPass.Initialize(device, shaderLibrary, m_rtvHeap, m_background.cube, "SkyEquirectangularToCubemap.hlsl");

    if(!created)
    {
        return false;
    }

    m_backgroundBytes = AllocationBytes(device, m_background.equirect.GetResource()) +
        AllocationBytes(device, m_background.cube.GetResource());
    return true;
}

void SkyAtmosphere::Shutdown()
{
    m_aerialCompositePipeline.Shutdown();
    m_aerialPipeline.Shutdown();
    m_skyViewPipeline.Shutdown();
    m_importanceMarginalPipeline.Shutdown();
    m_importanceRowsPipeline.Shutdown();
    m_environmentPipeline.Shutdown();
    m_multipleScatteringPipeline.Shutdown();
    m_transmittancePipeline.Shutdown();

    for(EnvironmentSet& set : m_sets)
    {
        set.multiscatterPass.Shutdown();
        set.diffusePass.Shutdown();
        set.cubemapPass.Shutdown();
        set.mipPass.Shutdown();
        set.skyView.Shutdown();
        set.environment.Shutdown();
    }

    // S5.
    m_cloudCompositePipeline.Shutdown();
    m_cloudReprojectPipeline.Shutdown();
    m_cloudTracePipeline.Shutdown();
    m_cloudShadowPipeline.Shutdown();
    m_cloudEnvironmentPipeline.Shutdown();
    m_cloudAmbientPipeline.Shutdown();
    m_cloudDetailPipeline.Shutdown();
    m_cloudShapePipeline.Shutdown();
    m_cloudCamera.trace.Shutdown();
    m_cloudCamera.traceDepth.Shutdown();

    for(unsigned int index = 0; index < 2; ++index)
    {
        m_cloudCamera.history[index].Shutdown();
        m_cloudCamera.historyDepth[index].Shutdown();
    }

    m_cloudCamera.width = m_cloudCamera.height = m_cloudCamera.traceWidth = m_cloudCamera.traceHeight = 0;
    m_cloudCamera.valid = false;
    m_cloudCamera.recorded = false;
    m_frameInfo.Shutdown();
    m_frameInfoActive = false;
    m_cloudShadow.Shutdown();
    m_cloudMoonShadow.Shutdown();
    m_cloudAmbient.Shutdown();
    m_shapeNoise = CloudVolume{};
    m_detailNoise = CloudVolume{};
    m_noiseState = 0;

    for(EnvironmentSet& set : m_sets)
    {
        set.cloudEnvironment.Shutdown();
    }

    m_background.cubemapPass.Shutdown();
    m_background.mipPass.Shutdown();
    m_background.cube.Shutdown();
    m_background.equirect.Shutdown();
    m_background.width = 0;
    m_background.height = 0;
    m_background.cubeSize = 0;
    m_background.source.reset();
    m_background.progress = {};
    m_background.recorded.reset();
    m_backgroundBytes = 0;

    for(std::size_t slot = 0; slot < m_hdriStaging.size(); ++slot)
    {
        if(m_hdriStaging[slot] && m_hdriStagingMapped[slot])
        {
            m_hdriStaging[slot]->Unmap(0, nullptr);
        }
    }

    m_hdriStaging.clear();
    m_hdriStagingMapped.clear();
    m_stagingBytes = 0;
    m_stagingRecorded = false;

    // S5B.
    m_irradianceReadback.Reset();
    m_irradianceCopyRecorded = false;
    m_irradianceCopyFence.reset();

    m_aerialVolume.Shutdown();
    m_rowWeights.Shutdown();
    m_multipleScatteringLut.Shutdown();
    m_transmittanceLut.Shutdown();
    m_rtvHeap.Shutdown();

    m_publishedSet = 1;
    m_published = false;
    m_bakeState = BakeState::Idle;
    m_stage = RebuildStage::Environment;
    m_publishedInputs.reset();
    m_pendingInputs.reset();
    m_lutAtmosphere.reset();
    m_pendingLutAtmosphere.reset();
    m_passTiming.reset();
    m_forcedBake = ForcedBake::None;
    m_initialized = false;
}

void SkyAtmosphere::EnableDiagnostics(ID3D12Device* device, std::uint64_t timestampFrequency)
{
    if(!m_initialized || !device)
    {
        return;
    }

    if(const char* forced = std::getenv("ENGINE_DIAG_SKY_FORCE_BAKE"))
    {
        m_forcedBake = std::string_view(forced) == "full" ? ForcedBake::Full : ForcedBake::Sun;
        Log::Warning(std::string("Physical sky diagnostics: a ") + (m_forcedBake == ForcedBake::Full ? "full" : "sun-only") +
            " re-bake every frame (ENGINE_DIAG_SKY_FORCE_BAKE).");
    }

    if(!std::getenv("ENGINE_DIAG_SKY_PASS_TIMING") || timestampFrequency == 0)
    {
        return;
    }

    PassTiming timing;
    D3D12_QUERY_HEAP_DESC queries{};
    queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queries.Count = PassTiming::Slots * PassTiming::Stamps;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = std::uint64_t(PassTiming::Slots) * PassTiming::Stamps * sizeof(std::uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if(FAILED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&timing.queries))) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&timing.readback))))
    {
        Log::Warning("Physical sky diagnostics: pass timing could not be created.");
        return;
    }

    timing.ticksPerMillisecond = double(timestampFrequency) / 1000.0;
    m_passTiming = std::move(timing);

    // S5: the clouds' per-frame passes.
    CloudTiming clouds;
    queries.Count = CloudTiming::Slots * CloudTiming::Stamps;
    buffer.Width = std::uint64_t(CloudTiming::Slots) * CloudTiming::Stamps * sizeof(std::uint64_t);

    if(SUCCEEDED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&clouds.queries))) &&
        SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&clouds.readback))))
    {
        clouds.ticksPerMillisecond = double(timestampFrequency) / 1000.0;
        m_cloudTiming = std::move(clouds);
    }
    Log::Info("Physical sky diagnostics: pass timing on (ENGINE_DIAG_SKY_PASS_TIMING).");
}

void SkyAtmosphere::ReadPassTiming(unsigned int slot)
{
    PassTiming& timing = *m_passTiming;

    if(!timing.executed[slot])
    {
        return;
    }

    timing.executed[slot] = false;
    constexpr std::size_t SlotBytes = PassTiming::Stamps * sizeof(std::uint64_t);
    const D3D12_RANGE range{slot * SlotBytes, (slot + 1) * SlotBytes};
    void* mapped = nullptr;

    if(FAILED(timing.readback->Map(0, &range, &mapped)))
    {
        return;
    }

    std::uint64_t stamps[PassTiming::Stamps]{};
    std::memcpy(stamps, static_cast<const std::uint8_t*>(mapped) + range.Begin, SlotBytes);
    const D3D12_RANGE nothingWritten{0, 0};
    timing.readback->Unmap(0, &nothingWritten);

    std::array<double, PassTiming::Passes> passes{};

    for(unsigned int pass = 0; pass < PassTiming::Passes; ++pass)
    {
        passes[pass] = double(stamps[2 * pass + 1] - stamps[2 * pass]) / timing.ticksPerMillisecond;
    }

    timing.samples.push_back(passes);

    if(timing.samples.size() < PassTiming::SamplesPerLog)
    {
        return;
    }

    const auto median = [](std::vector<double> values)
    {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    const auto format = [](double value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", value);
        return std::string(text);
    };
    // Medians per pass, then of each stage's sum (one stage per frame) and of the rebuild's total.
    constexpr const char* Names[PassTiming::Passes] = {"transmittance LUT", "multiple-scattering LUT", "environment",
        "equirect mips", "CDF", "diffuse irradiance", "cube conversion alone (diagnostic)", "GGX cube (conversion + prefilter)",
        "GGX multiscatter"};
    constexpr unsigned int StageEnds[3] = {5, 8, 9};
    std::string text = "Physical sky pass timing (GPU ms, median of " + std::to_string(timing.samples.size()) + " rebuilds):";

    for(unsigned int pass = 0; pass < PassTiming::Passes; ++pass)
    {
        std::vector<double> values;

        for(const auto& sample : timing.samples)
        {
            values.push_back(sample[pass]);
        }

        text += std::string(pass == 0 ? " " : ", ") + Names[pass] + " " + format(median(values));
    }

    std::vector<double> totals;
    std::array<std::vector<double>, 3> stages;

    for(const auto& sample : timing.samples)
    {
        double total = 0.0;
        unsigned int pass = 0;

        for(unsigned int stage = 0; stage < 3; ++stage)
        {
            double sum = 0.0;

            for(; pass < StageEnds[stage]; ++pass)
            {
                sum += sample[pass];
            }

            stages[stage].push_back(sum);
            total += sum;
        }

        totals.push_back(total);
    }

    Log::Info(text + "; per frame: environment stage " + format(median(stages[0])) + ", cubes stage " + format(median(stages[1])) +
        ", multiscatter stage " + format(median(stages[2])) + "; rebuild total " + format(median(totals)));
    timing.samples.clear();
}

bool SkyAtmosphere::SameAtmosphere(const PhysicalSkySettings& left, const PhysicalSkySettings& right)
{
    return left.planetRadiusKm == right.planetRadiusKm && left.atmosphereHeightKm == right.atmosphereHeightKm &&
        left.groundAlbedo == right.groundAlbedo && left.rayleighScattering == right.rayleighScattering &&
        left.rayleighAbsorption == right.rayleighAbsorption && left.rayleighScaleHeightKm == right.rayleighScaleHeightKm &&
        left.mieScattering == right.mieScattering && left.mieAbsorption == right.mieAbsorption &&
        left.mieScaleHeightKm == right.mieScaleHeightKm && left.ozoneAbsorption == right.ozoneAbsorption &&
        left.ozoneMinimumAltitudeKm == right.ozoneMinimumAltitudeKm && left.ozoneLayerWidthKm == right.ozoneLayerWidthKm;
}

double SkyAtmosphere::RadianceChange(const BakeInputs& before, const BakeInputs& after)
{
    if(before.hdri || after.hdri)
    {
        return before.hdri && after.hdri && before.hdri->identity == after.hdri->identity ? 0.0 :
            std::numeric_limits<double>::infinity();
    }

    const auto relative = [](double left, double right)
    {
        const double scale = (std::max)(std::abs(left), std::abs(right));
        return scale > 0.0 ? std::abs(left - right) / scale : 0.0;
    };
    const auto luminance = [](const std::array<float, 3>& channels)
    {
        return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
    };
    const PhysicalSkySettings& left = before.settings;
    const PhysicalSkySettings& right = after.settings;

    // S5: adding or removing a cloud layer, or a large change of a layer's coverage or density, is a discontinuity.
    if(left.cloudLayers.size() != right.cloudLayers.size())
    {
        return std::numeric_limits<double>::infinity();
    }

    double clouds = 0.0;

    for(std::size_t index = 0; index < left.cloudLayers.size(); ++index)
    {
        const auto relativeChange = [](double a, double b)
        {
            const double scale = (std::max)(std::abs(a), std::abs(b));
            return scale > 0.0 ? std::abs(a - b) / scale : 0.0;
        };
        clouds = (std::max)({clouds, relativeChange(left.cloudLayers[index].coverage, right.cloudLayers[index].coverage),
            relativeChange(left.cloudLayers[index].density, right.cloudLayers[index].density)});
    }

    const double sunBefore = 0.2126 * before.sunIrradiance.x + 0.7152 * before.sunIrradiance.y + 0.0722 * before.sunIrradiance.z;
    const double sunAfter = 0.2126 * after.sunIrradiance.x + 0.7152 * after.sunIrradiance.y + 0.0722 * after.sunIrradiance.z;
    return (std::max)({relative(sunBefore, sunAfter), relative(PhysicalSkyLuminanceFactor(left), PhysicalSkyLuminanceFactor(right)),
        relative(luminance(left.rayleighScattering), luminance(right.rayleighScattering)),
        relative(luminance(left.mieScattering), luminance(right.mieScattering)),
        relative(luminance(left.mieAbsorption), luminance(right.mieAbsorption)),
        relative(luminance(left.ozoneAbsorption), luminance(right.ozoneAbsorption)),
        relative(luminance(left.groundAlbedo), luminance(right.groundAlbedo)),
        relative(left.multipleScattering, right.multipleScattering), relative(left.mieAnisotropy, right.mieAnisotropy),
        relative(left.rayleighScaleHeightKm, right.rayleighScaleHeightKm), relative(left.mieScaleHeightKm, right.mieScaleHeightKm),
        relative(double(before.referenceEV100), double(after.referenceEV100)), clouds});
}

bool SkyAtmosphere::CreateAerialComposite(ID3D12Device* device, ShaderLibrary& shaderLibrary)
{
    const Shader* vertexShader = shaderLibrary.Load({"SkyAerialPerspective.hlsl", "VSComposite", ShaderStage::Vertex});
    const Shader* pixelShader = shaderLibrary.Load({"SkyAerialPerspective.hlsl", "PSComposite", ShaderStage::Pixel});

    if(!vertexShader || !pixelShader)
    {
        return false;
    }

    // Straight-alpha "over" blending of (light / (1 - T), 1 - T): scene * T + light. No depth test: the composite reads depth.
    PipelineDescription description;
    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.renderTargets.count = 1;
    description.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.alphaBlend = true;
    description.rootParameters.push_back({RootParameterType::ConstantBuffer, 0, 0, D3D12_SHADER_VISIBILITY_ALL});
    return m_aerialCompositePipeline.Initialize(device, description);
}

bool SkyAtmosphere::RecordAerialPerspective(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap,
    UploadBuffer& uploadBuffer, RenderTarget& sceneColor, DepthBuffer& depth, const Matrix4& viewProjection,
    const Vector3& cameraPosition, double cameraAltitudeMeters, const std::function<void(bool)>& betweenParts)
{
    if(!m_initialized || !m_published || !m_publishedInputs || m_publishedInputs->hdri || !commandList)
    {
        return false;
    }

    // The published sky seen from this frame's camera.
    BakeInputs inputs = *m_publishedInputs;
    inputs.cameraAltitudeMeters = cameraAltitudeMeters;
    SkyAtmosphereConstants constants = MakeConstants(inputs, m_layout);
    const Matrix4 inverseViewProjection = viewProjection.Inverse();
    constants.transmittanceIndex = m_transmittanceLut.GetDescriptorIndex();
    constants.multipleScatteringIndex = m_multipleScatteringLut.GetDescriptorIndex();
    constants.aerialOutputIndex = m_aerialVolume.GetUAVIndex();
    constants.aerialInverseViewProjection = inverseViewProjection;
    constants.aerialCameraPosition[0] = cameraPosition.x;
    constants.aerialCameraPosition[1] = cameraPosition.y;
    constants.aerialCameraPosition[2] = cameraPosition.z;
    const D3D12_GPU_VIRTUAL_ADDRESS volumeConstants =
        uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    AerialCompositeConstants composite{};
    composite.inverseViewProjection = inverseViewProjection;
    composite.cameraPosition[0] = cameraPosition.x;
    composite.cameraPosition[1] = cameraPosition.y;
    composite.cameraPosition[2] = cameraPosition.z;
    composite.depthIndex = depth.GetSRVIndex();
    composite.volumeIndex = m_aerialVolume.GetDescriptorIndex();
    composite.slices = AerialSlices;
    composite.tiles = AerialTiles;
    composite.maxDistance = AerialMaxDistanceKm;
    const D3D12_GPU_VIRTUAL_ADDRESS compositeConstants =
        uploadBuffer.Allocate(&composite, sizeof(composite), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(volumeConstants == 0 || compositeConstants == 0)
    {
        Log::Error("Failed to allocate the aerial perspective's constants; it is skipped this frame.");
        return false;
    }

    // 1. The volume.
    ID3D12DescriptorHeap* heaps[] = {resourceHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    m_aerialVolume.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_aerialPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, volumeConstants);
    commandList->Dispatch(GroupCount(AerialTiles, 8), GroupCount(AerialTiles, 8), 1);
    UnorderedAccessBarrier(commandList, m_aerialVolume.GetResource());
    m_aerialVolume.Transition(commandList, ShaderRead);

    if(betweenParts)
    {
        betweenParts(true);
    }

    // 2. The composite into the scene colour.
    const D3D12_RESOURCE_STATES depthState = depth.GetState();
    depth.Transition(commandList, ShaderRead);
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = sceneColor.GetRTV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(sceneColor.GetWidth()), float(sceneColor.GetHeight()), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(sceneColor.GetWidth()), LONG(sceneColor.GetHeight())};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_aerialCompositePipeline.Bind(commandList);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(0, compositeConstants);
    commandList->DrawInstanced(3, 1, 0, 0);
    depth.Transition(commandList, depthState);

    if(betweenParts)
    {
        betweenParts(false);
    }

    return true;
}

bool SkyAtmosphere::CreateIrradianceReadback(ID3D12Device* device)
{
    // Both sets' irradiance cubes share one description; the copy holds one set's six faces.
    ID3D12Resource* cube = m_sets[0].environment.GetDiffuseIrradiance().GetResource();

    if(!cube)
    {
        return false;
    }

    const D3D12_RESOURCE_DESC description = cube->GetDesc();
    const unsigned int mips = description.MipLevels;
    std::uint64_t total = 0;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 6> footprints{};

    for(unsigned int face = 0; face < 6; ++face)
    {
        std::uint64_t bytes = 0;
        device->GetCopyableFootprints(&description, face * mips, 1, total, &footprints[face], nullptr, nullptr, &bytes);
        total = (footprints[face].Offset + bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
            ~std::uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_irradianceReadback))))
    {
        return false;
    }

    MemoryRegistry::TrackResource(m_irradianceReadback.Get(), MemoryCategory::UploadReadback);
    m_irradianceFootprints = footprints;
    m_irradianceFormat = description.Format;
    return true;
}

void SkyAtmosphere::RecordIrradianceCopy(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set)
{
    if(!m_irradianceReadback)
    {
        return;
    }

    Texture& irradiance = set.environment.GetDiffuseIrradiance();
    const unsigned int mips = irradiance.GetResource()->GetDesc().MipLevels;
    irradiance.Transition(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE);

    for(unsigned int face = 0; face < 6; ++face)
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = m_irradianceReadback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = m_irradianceFootprints[face];
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = irradiance.GetResource();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = face * mips;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }

    irradiance.Transition(commandList, ShaderRead);
    m_irradianceCopyRecorded = true;
}

std::optional<double> SkyAtmosphere::TakeIrradianceMean(std::uint64_t completedFence)
{
    if(!m_irradianceCopyFence || completedFence < *m_irradianceCopyFence || !m_irradianceReadback)
    {
        return std::nullopt;
    }

    m_irradianceCopyFence.reset();
    const auto& last = m_irradianceFootprints[5];
    const std::uint64_t end = last.Offset + std::uint64_t(last.Footprint.RowPitch) * last.Footprint.Height;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(end)};
    void* mapped = nullptr;

    if(FAILED(m_irradianceReadback->Map(0, &range, &mapped)))
    {
        return std::nullopt;
    }

    const bool wide = m_irradianceFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
    double sum = 0.0;
    std::size_t count = 0;

    for(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint : m_irradianceFootprints)
    {
        for(unsigned int row = 0; row < footprint.Footprint.Height; ++row)
        {
            const std::uint8_t* bytes = static_cast<const std::uint8_t*>(mapped) + footprint.Offset +
                std::uint64_t(row) * footprint.Footprint.RowPitch;

            for(unsigned int column = 0; column < footprint.Footprint.Width; ++column)
            {
                float rgb[3];

                for(unsigned int channel = 0; channel < 3; ++channel)
                {
                    if(wide)
                    {
                        std::memcpy(&rgb[channel], bytes + column * 16 + channel * 4, 4);
                    }
                    else
                    {
                        std::uint16_t half = 0;
                        std::memcpy(&half, bytes + column * 8 + channel * 2, 2);
                        rgb[channel] = DirectX::PackedVector::XMConvertHalfToFloat(half);
                    }
                }

                sum += 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
                ++count;
            }
        }
    }

    const D3D12_RANGE nothingWritten{0, 0};
    m_irradianceReadback->Unmap(0, &nothingWritten);

    if(count == 0 || !std::isfinite(sum))
    {
        return std::nullopt;
    }

    return sum / double(count);
}

void SkyAtmosphere::DropRebuild()
{
    // A dropped cloud or environment stage may or may not have written the LUTs; later stages never write them.
    if(m_stage == RebuildStage::Clouds || m_stage == RebuildStage::Environment)
    {
        m_lutAtmosphere.reset();
    }

    m_bakeState = BakeState::Idle;
    m_stage = RebuildStage::Environment;
    m_pendingInputs.reset();
    m_pendingLutAtmosphere.reset();
}

bool SkyAtmosphere::PublishSubmittedBake()
{
    m_stagingRecorded = false;

    if(m_background.recorded)
    {
        // A background slice was recorded without a confirmed submission: the slot's contents are unknown, so its build starts
        // again (readers were not using it).
        m_background.recorded.reset();
        m_background.progress = m_background.source ? BackgroundProgress{BackgroundStep::Upload} : BackgroundProgress{};
    }

    if(m_bakeState == BakeState::Recorded)
    {
        // The frame that recorded a stage never confirmed its submission (it was discarded or failed before execution).
        DropRebuild();
        return false;
    }

    if(m_bakeState != BakeState::Submitted)
    {
        return false;
    }

    // Every stage of the rebuild is on the frame queue ahead of this frame, so readers of the new set see it complete.
    m_publishedSet ^= 1u;
    m_published = true;
    ++m_generation;
    m_publishedInputs = m_pendingInputs;
    m_pendingInputs.reset();
    m_bakeState = BakeState::Idle;
    m_stage = RebuildStage::Environment;
    RestartHdriBackground();
    return true;
}

void SkyAtmosphere::RestartHdriBackground()
{
    if(m_background.width == 0 || !m_publishedInputs || !m_publishedInputs->hdri)
    {
        return;
    }

    const std::shared_ptr<const SkyHdriTexels>& hdri = m_publishedInputs->hdri;

    if(m_background.source && m_background.source->identity == hdri->identity)
    {
        return;
    }

    if(hdri->backgroundWidth != m_background.width || hdri->backgroundHeight != m_background.height ||
        hdri->background.size() != std::size_t(m_background.width) * m_background.height * 4)
    {
        Log::Warning("Physical sky: the HDRI sky has no " + std::to_string(m_background.width) + " background; the sky's " +
            std::to_string(m_layout.width) + " equirect is shown.");
        m_background.source.reset();
        m_background.progress = {};
        return;
    }

    // Readers of the previous contents were earlier frames on this queue, so the slot is rewritten from this frame on.
    m_background.source = hdri;
    m_background.progress = {BackgroundStep::Upload};
}

bool SkyAtmosphere::PublishedHdri() const noexcept
{
    return m_published && m_publishedInputs && m_publishedInputs->hdri;
}

const SkyHdriTexels* SkyAtmosphere::GetPublishedHdri() const noexcept
{
    return PublishedHdri() ? m_publishedInputs->hdri.get() : nullptr;
}

Texture* SkyAtmosphere::GetHdriBackground() noexcept
{
    const SkyHdriTexels* hdri = GetPublishedHdri();
    return hdri && m_background.source && m_background.source->identity == hdri->identity &&
        m_background.progress.step == BackgroundStep::Ready ? &m_background.equirect : nullptr;
}

Texture* SkyAtmosphere::GetHdriReflectionCube() noexcept
{
    return GetHdriBackground() ? &m_background.cube : nullptr;
}

void SkyAtmosphere::RecordHdriBackground(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, unsigned int frameIndex)
{
    // One slice per frame, only in frames without a rebuild stage (each stays within the sky's per-frame budget).
    if(!m_initialized || !commandList || m_background.width == 0 || !m_background.source || m_stagingRecorded ||
        m_bakeState == BakeState::Recorded || m_background.recorded || m_background.progress.step == BackgroundStep::Empty ||
        m_background.progress.step == BackgroundStep::Ready || !PublishedHdri() ||
        m_publishedInputs->hdri->identity != m_background.source->identity)
    {
        return;
    }

    HdriBackground& slot = m_background;
    BackgroundProgress progress = slot.progress;
    ID3D12DescriptorHeap* heaps[] = {resourceHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);

    switch(progress.step)
    {
    case BackgroundStep::Upload:
    {
        const unsigned int rowsPerSlice = unsigned(m_stagingSlotBytes / (std::size_t(slot.width) * 4 * sizeof(std::uint16_t)));
        const unsigned int rows = (std::min)(rowsPerSlice, slot.height - progress.uploadRow);
        const std::size_t staging = frameIndex % m_hdriStaging.size();
        slot.equirect.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST, 0);
        CopyRowsThroughStaging(commandList, m_hdriStaging[staging].Get(), m_hdriStagingMapped[staging],
            slot.source->background.data(), slot.width, progress.uploadRow, rows, slot.equirect.GetResource());
        m_stagingRecorded = true;
        progress.uploadRow += rows;

        if(progress.uploadRow >= slot.height)
        {
            progress.step = BackgroundStep::Mips;
        }

        break;
    }
    case BackgroundStep::Mips:
        slot.mipPass.Execute(commandList, slot.equirect);
        slot.equirect.Transition(commandList, ShaderRead);
        progress.step = BackgroundStep::Cube;
        break;
    case BackgroundStep::Cube:
    {
        const unsigned int mipLevels = slot.cubemapPass.GetMipLevels();
        double budget = BackgroundCubeMicrosecondsPerFrame;
        // A whole mip's estimated cost (six faces).
        const auto mipCost = [&](unsigned int mip)
        {
            const double size = double((std::max)(1u, slot.cubeSize >> mip));
            return 6.0 * size * size * double(BackgroundCubeSamplesPerTexel(mip, mipLevels)) / SamplesPerMicrosecond;
        };

        while(progress.step == BackgroundStep::Cube && budget > 0.0)
        {
            // Whole mips that fit the remaining budget are drawn together, face draws overlapping (the small mips' draws are
            // latency-bound, so slicing them face by face would serialize them).
            if(progress.cubeFace == 0 && progress.cubeRow == 0)
            {
                unsigned int endMip = progress.cubeMip;
                double cost = 0.0;

                while(endMip < mipLevels && cost + mipCost(endMip) <= budget)
                {
                    cost += mipCost(endMip);
                    ++endMip;
                }

                if(endMip > progress.cubeMip)
                {
                    slot.cubemapPass.RecordMips(commandList, slot.equirect, slot.cube, progress.cubeMip, endMip);
                    budget -= cost;
                    progress.cubeMip = endMip;

                    if(progress.cubeMip == mipLevels)
                    {
                        slot.cube.Transition(commandList, ShaderRead);
                        progress.step = BackgroundStep::Ready;
                    }

                    // The remaining mips are larger than what is left of this frame's budget: they start next frame.
                    break;
                }
            }

            const unsigned int size = (std::max)(1u, slot.cubeSize >> progress.cubeMip);
            const double rowCost = double(size) * double(BackgroundCubeSamplesPerTexel(progress.cubeMip, mipLevels)) /
                SamplesPerMicrosecond;
            const unsigned int affordable = (std::max)(1u, unsigned(budget / rowCost));
            const unsigned int rowEnd = (std::min)(size, progress.cubeRow + affordable);
            slot.cubemapPass.RecordRows(commandList, slot.equirect, slot.cube, progress.cubeMip, progress.cubeFace,
                progress.cubeRow, rowEnd);
            budget -= double(rowEnd - progress.cubeRow) * rowCost;
            progress.cubeRow = rowEnd;

            if(progress.cubeRow < size)
            {
                continue;
            }

            progress.cubeRow = 0;

            if(++progress.cubeFace == 6)
            {
                progress.cubeFace = 0;

                if(++progress.cubeMip == mipLevels)
                {
                    // Complete: every subresource readable by every stage.
                    slot.cube.Transition(commandList, ShaderRead);
                    progress.step = BackgroundStep::Ready;
                }
            }
        }

        break;
    }
    case BackgroundStep::Empty:
    case BackgroundStep::Ready:
        return;
    }

    slot.recorded = progress;
}

bool SkyAtmosphere::NeedsBake(const BakeInputs& inputs) const
{
    if(!m_initialized)
    {
        return false;
    }

    if(m_bakeState == BakeState::Building)
    {
        return true;
    }

    if(m_bakeState != BakeState::Idle)
    {
        return false;
    }

    if(!m_publishedInputs || m_forcedBake != ForcedBake::None || m_updateRequested)
    {
        return true;
    }

    // S4: an HDRI sky rebuilds only when another HDRI, or the physical sky, replaces it (update modes do not apply).
    if(inputs.hdri || m_publishedInputs->hdri)
    {
        return !inputs.hdri || !m_publishedInputs->hdri || inputs.hdri->identity != m_publishedInputs->hdri->identity;
    }

    // S4 (§4.9): HDRP's update modes. On Demand rebuilds only when asked (RequestUpdate); Realtime also rebuilds every update
    // period (0: every frame), On Changed only on changes.
    switch(inputs.settings.updateMode)
    {
    case PhysicalSkyUpdateMode::OnDemand:
        return false;
    case PhysicalSkyUpdateMode::Realtime:
        if(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_lastRebuildStart).count() >=
            double(inputs.settings.updatePeriodSeconds))
        {
            return true;
        }

        break;
    case PhysicalSkyUpdateMode::OnChanged:
        break;
    }

    const BakeInputs& published = *m_publishedInputs;

    if(!(inputs.settings == published.settings) || inputs.referenceEV100 != published.referenceEV100)
    {
        return true;
    }

    const Vector3 current = DirectionToSun(inputs.sunDirection);
    const Vector3 baked = DirectionToSun(published.sunDirection);
    const double cosine = double(current.x) * baked.x + double(current.y) * baked.y + double(current.z) * baked.z;
    constexpr double Pi = 3.14159265358979323846;
    const bool sunTurned = cosine < std::cos(SunTurnDegrees * Pi / 180.0);

    if(sunTurned || RelativeChange(inputs.sunIrradiance, published.sunIrradiance) > RelativeIrradianceChange)
    {
        return true;
    }

    // S5: clouds follow the wind and the camera: a rebuild once either has moved them by CloudRebuildDistanceMeters.
    if(!inputs.settings.cloudLayers.empty())
    {
        double fastest = 0.0;

        for(const CloudLayerSettings& layer : inputs.settings.cloudLayers)
        {
            fastest = (std::max)(fastest, double(layer.windSpeedMetersPerSecond));
        }

        const double drift = fastest * std::abs(inputs.cloudTimeSeconds - published.cloudTimeSeconds);
        const double moved = std::hypot(inputs.cameraWorldX - published.cameraWorldX, inputs.cameraWorldZ - published.cameraWorldZ);

        if(drift > CloudRebuildDistanceMeters || moved > CloudRebuildDistanceMeters)
        {
            return true;
        }
    }

    // S5: the moon, by the sun's thresholds.
    const Vector3 moon = DirectionToSun(inputs.moonDirection);
    const Vector3 bakedMoon = DirectionToSun(published.moonDirection);
    const double moonCosine = double(moon.x) * bakedMoon.x + double(moon.y) * bakedMoon.y + double(moon.z) * bakedMoon.z;

    if(moonCosine < std::cos(SunTurnDegrees * Pi / 180.0) ||
        RelativeChange(inputs.moonIrradiance, published.moonIrradiance) > RelativeIrradianceChange)
    {
        return true;
    }

    return std::abs(inputs.cameraAltitudeMeters - published.cameraAltitudeMeters) > AltitudeChangeMeters;
}

void SkyAtmosphere::StampPass(ID3D12GraphicsCommandList* commandList, TimedPass pass, bool end) const
{
    if(m_passTiming)
    {
        const unsigned int stamp = m_passTiming->currentSlot * PassTiming::Stamps + 2 * static_cast<unsigned int>(pass) +
            (end ? 1u : 0u);
        commandList->EndQuery(m_passTiming->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, stamp);
    }
}

void SkyAtmosphere::RecordEnvironmentStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set,
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress, bool atmosphereChanged)
{
    Texture& radiance = set.environment.GetSourceTexture();

    // [1] and [2], only when the atmosphere changed (the multiple-scattering LUT reads the transmittance LUT). S5: a rebuild with
    // clouds updates them in its cloud stage instead.
    if(atmosphereChanged)
    {
        RecordLuts(commandList, constantsAddress);
    }
    else
    {
        StampPass(commandList, TimedPass::Transmittance, false);
        StampPass(commandList, TimedPass::Transmittance, true);
        StampPass(commandList, TimedPass::MultipleScattering, false);
        StampPass(commandList, TimedPass::MultipleScattering, true);
    }

    // [3] the environment into the inactive set's equirect mip 0.
    StampPass(commandList, TimedPass::Environment, false);
    radiance.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    m_environmentPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch(GroupCount(m_layout.width, 8), GroupCount(m_layout.height, 8), 1);
    UnorderedAccessBarrier(commandList, radiance.GetResource());

    // [3b] the sky-view map, the same radiance at the horizon's resolution.
    set.skyView.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_skyViewPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch(GroupCount(SkyViewWidth, 8), GroupCount(SkyViewHeight, 8), 1);
    UnorderedAccessBarrier(commandList, set.skyView.GetResource());
    set.skyView.Transition(commandList, ShaderRead);
    StampPass(commandList, TimedPass::Environment, true);
    RecordMipsAndCdf(commandList, set, constantsAddress);
}

void SkyAtmosphere::RecordLuts(ID3D12GraphicsCommandList* commandList, D3D12_GPU_VIRTUAL_ADDRESS constantsAddress)
{
    StampPass(commandList, TimedPass::Transmittance, false);
    m_transmittanceLut.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_transmittancePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch(GroupCount(TransmittanceWidth, 8), GroupCount(TransmittanceHeight, 8), 1);
    UnorderedAccessBarrier(commandList, m_transmittanceLut.GetResource());
    m_transmittanceLut.Transition(commandList, ShaderRead);
    StampPass(commandList, TimedPass::Transmittance, true);
    StampPass(commandList, TimedPass::MultipleScattering, false);
    m_multipleScatteringLut.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_multipleScatteringPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch(MultipleScatteringSize, MultipleScatteringSize, 1);
    UnorderedAccessBarrier(commandList, m_multipleScatteringLut.GetResource());
    m_multipleScatteringLut.Transition(commandList, ShaderRead);
    StampPass(commandList, TimedPass::MultipleScattering, true);
}

SkyAtmosphere::RebuildStage SkyAtmosphere::FirstStage(const BakeInputs& inputs)
{
    return !inputs.hdri && !inputs.settings.cloudLayers.empty() ? RebuildStage::Clouds : RebuildStage::Environment;
}

void SkyAtmosphere::TransitionVolume(ID3D12GraphicsCommandList* commandList, CloudVolume& volume, D3D12_RESOURCE_STATES state)
{
    if(!volume.resource || volume.state == state)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = volume.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = volume.state;
    barrier.Transition.StateAfter = state;
    commandList->ResourceBarrier(1, &barrier);
    volume.state = state;
}

// S5 (§14.3): the rebuild's cloud stage: the LUTs when the atmosphere changed (the clouds are lit through them), the clear sky's
// ambient light, and the set's cloud map for the bake's camera and wind.
void SkyAtmosphere::RecordCloudStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set, DescriptorHeap& resourceHeap,
    UploadBuffer& uploadBuffer, D3D12_GPU_VIRTUAL_ADDRESS constantsAddress, const BakeInputs& inputs, bool atmosphereChanged)
{
    (void)resourceHeap;

    if(atmosphereChanged)
    {
        RecordLuts(commandList, constantsAddress);
    }

    SkyCloudConstants clouds = MakeCloudConstants(inputs.settings, inputs.cloudTimeSeconds, inputs.cameraWorldX, inputs.cameraWorldZ);
    clouds.shapeNoiseIndex = m_shapeNoise.srv;
    clouds.detailNoiseIndex = m_detailNoise.srv;
    clouds.ambientIndex = m_cloudAmbient.GetUAVIndex();
    clouds.environmentOutputIndex = set.cloudEnvironment.GetUAVIndex();
    clouds.environmentOutputWidth = CloudEnvironmentWidth;
    clouds.environmentOutputHeight = CloudEnvironmentHeight;
    const D3D12_GPU_VIRTUAL_ADDRESS ambientAddress =
        uploadBuffer.Allocate(&clouds, sizeof(clouds), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    clouds.ambientIndex = m_cloudAmbient.GetDescriptorIndex();
    const D3D12_GPU_VIRTUAL_ADDRESS cloudAddress =
        uploadBuffer.Allocate(&clouds, sizeof(clouds), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(ambientAddress == 0 || cloudAddress == 0)
    {
        return;
    }

    // The ambient light (written through its UAV, then read through its SRV).
    StampCloudPass(commandList, CloudPass::Stage, false);
    m_cloudAmbient.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cloudAmbientPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->SetComputeRootConstantBufferView(1, ambientAddress);
    commandList->Dispatch(2, 1, 1);
    UnorderedAccessBarrier(commandList, m_cloudAmbient.GetResource());
    m_cloudAmbient.Transition(commandList, ShaderRead);

    TransitionVolume(commandList, m_shapeNoise, ShaderRead);
    TransitionVolume(commandList, m_detailNoise, ShaderRead);
    set.cloudEnvironment.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cloudEnvironmentPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->SetComputeRootConstantBufferView(1, cloudAddress);
    commandList->Dispatch(GroupCount(CloudEnvironmentWidth, 8), GroupCount(CloudEnvironmentHeight, 8), 1);
    UnorderedAccessBarrier(commandList, set.cloudEnvironment.GetResource());
    set.cloudEnvironment.Transition(commandList, ShaderRead);
    StampCloudPass(commandList, CloudPass::Stage, true);
}

void SkyAtmosphere::RecordHdriEnvironmentStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set,
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress, const SkyHdriTexels& hdri, unsigned int frameIndex)
{
    // No LUTs, environment kernel or sky-view map: the HDRI's texels replace [3] (the sky-view map is not read for an HDRI set).
    StampPass(commandList, TimedPass::Transmittance, false);
    StampPass(commandList, TimedPass::Transmittance, true);
    StampPass(commandList, TimedPass::MultipleScattering, false);
    StampPass(commandList, TimedPass::MultipleScattering, true);
    StampPass(commandList, TimedPass::Environment, false);
    Texture& radiance = set.environment.GetSourceTexture();
    const std::size_t staging = frameIndex % m_hdriStaging.size();
    radiance.Transition(commandList, D3D12_RESOURCE_STATE_COPY_DEST, 0);
    CopyRowsThroughStaging(commandList, m_hdriStaging[staging].Get(), m_hdriStagingMapped[staging], hdri.texels.data(),
        m_layout.width, 0, m_layout.height, radiance.GetResource());
    m_stagingRecorded = true;
    StampPass(commandList, TimedPass::Environment, true);
    RecordMipsAndCdf(commandList, set, constantsAddress);
}

void SkyAtmosphere::RecordMipsAndCdf(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set,
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress)
{
    Texture& radiance = set.environment.GetSourceTexture();

    // [5a] the equirect's mips (EnvironmentMipPass reads mip 0 as a pixel shader resource), so the CDF build and the cubes read a
    // texture whose every subresource is readable.
    StampPass(commandList, TimedPass::EquirectMips, false);
    set.mipPass.Execute(commandList, radiance);
    radiance.Transition(commandList, ShaderRead);
    StampPass(commandList, TimedPass::EquirectMips, true);

    // [4] the importance CDFs of mip 0: one group per row, then the marginal from the row totals.
    StampPass(commandList, TimedPass::Cdf, false);
    ID3D12Resource* conditional = set.environment.GetImportanceConditionalCDF().GetResource();
    ID3D12Resource* marginal = set.environment.GetImportanceMarginalCDF().GetResource();
    ID3D12Resource* rowWeights = m_rowWeights.GetResource();
    TransitionBuffers(commandList, {conditional, marginal, rowWeights}, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_importanceRowsPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch(m_layout.height, 1, 1);
    UnorderedAccessBarrier(commandList, rowWeights);
    m_importanceMarginalPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->Dispatch(1, 1, 1);
    UnorderedAccessBarrier(commandList, conditional);
    UnorderedAccessBarrier(commandList, marginal);
    TransitionBuffers(commandList, {conditional, marginal, rowWeights}, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON);
    StampPass(commandList, TimedPass::Cdf, true);
}

void SkyAtmosphere::RecordCubesStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set)
{
    // [5b] in PreprocessEnvironment's order. Each pass leaves its cube a pixel shader resource; readers transition what they
    // need.
    Texture& radiance = set.environment.GetSourceTexture();
    StampPass(commandList, TimedPass::Irradiance, false);
    set.diffusePass.Execute(commandList, radiance, set.environment.GetDiffuseIrradiance());
    StampPass(commandList, TimedPass::Irradiance, true);
    StampPass(commandList, TimedPass::CubeConversion, false);

    if(m_passTiming)
    {
        // Mip 0 alone (the equirect resampled to the cube), drawn again by Execute below with the same result. Execute is timed
        // whole: drawing the prefiltered mips face by face puts a barrier after every draw and serializes them.
        for(unsigned int face = 0; face < 6; ++face)
        {
            set.cubemapPass.RecordRows(commandList, radiance, set.environment.GetCubemap(), 0, face, 0, set.cubemapPass.GetSize());
        }
    }

    StampPass(commandList, TimedPass::CubeConversion, true);
    StampPass(commandList, TimedPass::GgxCube, false);
    set.cubemapPass.Execute(commandList, radiance, set.environment.GetCubemap());
    StampPass(commandList, TimedPass::GgxCube, true);
}

void SkyAtmosphere::RecordMultiscatterStage(ID3D12GraphicsCommandList* commandList, EnvironmentSet& set, Texture& brdfIntegrationLUT)
{
    StampPass(commandList, TimedPass::Multiscatter, false);
    brdfIntegrationLUT.Transition(commandList, ShaderRead);
    set.multiscatterPass.Execute(commandList, set.environment.GetSourceTexture(), brdfIntegrationLUT,
        set.environment.GetGGXMultiscatterIrradiance());
    StampPass(commandList, TimedPass::Multiscatter, true);
}

bool SkyAtmosphere::RecordBake(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, UploadBuffer& uploadBuffer,
    Texture& brdfIntegrationLUT, const BakeInputs& inputs, unsigned int frameIndex)
{
    if(inputs.hdri && (inputs.hdri->width != m_layout.width || inputs.hdri->height != m_layout.height ||
        inputs.hdri->texels.size() != std::size_t(m_layout.width) * m_layout.height * 4))
    {
        Log::Error("Physical sky: an HDRI sky's texels do not match the sky's equirect; it is not baked.");
        return false;
    }

    if(!m_initialized || !commandList || (m_bakeState != BakeState::Idle && m_bakeState != BakeState::Building))
    {
        return false;
    }

    // A new rebuild takes this frame's inputs and, with pass timing on, the next timing slot (read first: it holds the rebuild
    // from Slots rebuilds ago).
    if(m_bakeState == BakeState::Idle)
    {
        m_pendingInputs = inputs;
        m_stage = FirstStage(inputs);
        m_updateRequested = false;
        m_lastRebuildStart = std::chrono::steady_clock::now();

        if(m_passTiming)
        {
            m_passTiming->currentSlot = m_passTiming->nextSlot;
            m_passTiming->nextSlot = (m_passTiming->currentSlot + 1) % PassTiming::Slots;
            ReadPassTiming(m_passTiming->currentSlot);
        }
    }

    const BakeInputs& rebuild = *m_pendingInputs;
    EnvironmentSet& set = m_sets[m_publishedSet ^ 1u];
    EnvironmentMap& environment = set.environment;
    Texture& radiance = environment.GetSourceTexture();
    SkyAtmosphereConstants constants = MakeConstants(rebuild, m_layout);
    constants.cloudDiskCosine = m_cloudDiskCosine;
    constants.transmittanceIndex = m_transmittanceLut.GetDescriptorIndex();
    constants.transmittanceOutputIndex = m_transmittanceLut.GetUAVIndex();
    constants.multipleScatteringIndex = m_multipleScatteringLut.GetDescriptorIndex();
    constants.multipleScatteringOutputIndex = m_multipleScatteringLut.GetUAVIndex();
    constants.environmentIndex = radiance.GetDescriptorIndex();
    constants.environmentOutputIndex = radiance.GetUAVIndex();
    constants.conditionalCdfIndex = environment.GetImportanceConditionalCDF().GetUAVDescriptorIndex();
    constants.marginalCdfIndex = environment.GetImportanceMarginalCDF().GetUAVDescriptorIndex();
    constants.rowWeightsIndex = m_rowWeights.GetUAVDescriptorIndex();
    constants.skyViewOutputIndex = set.skyView.GetUAVIndex();

    // S5: the set's cloud map, composited by CSEnvironment (a rebuild with clouds wrote it in its first stage).
    if(FirstStage(rebuild) == RebuildStage::Clouds)
    {
        constants.cloudEnvironmentIndex = set.cloudEnvironment.GetDescriptorIndex();
        constants.cloudEnvironmentWidth = CloudEnvironmentWidth;
        constants.cloudEnvironmentHeight = CloudEnvironmentHeight;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress =
        uploadBuffer.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(constantsAddress == 0)
    {
        Log::Error("Failed to allocate the physical sky's constants; the sky rebuilds on a later frame.");
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = {resourceHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    // The stamps this stage writes, resolved by this stage's recording.
    unsigned int firstPass = 0;
    unsigned int endPass = 0;

    const bool lutsChanged = m_forcedBake == ForcedBake::Full || !m_lutAtmosphere || !SameAtmosphere(*m_lutAtmosphere, rebuild.settings);

    switch(m_stage)
    {
    case RebuildStage::Clouds:
        RecordCloudStage(commandList, set, resourceHeap, uploadBuffer, constantsAddress, rebuild, !rebuild.hdri && lutsChanged);
        m_pendingLutAtmosphere = !rebuild.hdri && lutsChanged ? std::optional<PhysicalSkySettings>(rebuild.settings) : m_lutAtmosphere;
        break;
    case RebuildStage::Environment:
    {
        if(rebuild.hdri)
        {
            // The LUTs keep the atmosphere they hold.
            RecordHdriEnvironmentStage(commandList, set, constantsAddress, *rebuild.hdri, frameIndex);
            m_pendingLutAtmosphere = m_lutAtmosphere;
            firstPass = static_cast<unsigned int>(TimedPass::Transmittance);
            endPass = static_cast<unsigned int>(TimedPass::Irradiance);
            break;
        }

        // A rebuild with clouds updated the LUTs in its cloud stage.
        const bool atmosphereChanged = FirstStage(rebuild) == RebuildStage::Environment && lutsChanged;
        RecordEnvironmentStage(commandList, set, constantsAddress, atmosphereChanged);
        m_pendingLutAtmosphere = atmosphereChanged ? std::optional<PhysicalSkySettings>(rebuild.settings) : m_lutAtmosphere;
        firstPass = static_cast<unsigned int>(TimedPass::Transmittance);
        endPass = static_cast<unsigned int>(TimedPass::Irradiance);
        break;
    }
    case RebuildStage::Cubes:
        RecordCubesStage(commandList, set);
        firstPass = static_cast<unsigned int>(TimedPass::Irradiance);
        endPass = static_cast<unsigned int>(TimedPass::Multiscatter);
        break;
    case RebuildStage::Multiscatter:
        RecordMultiscatterStage(commandList, set, brdfIntegrationLUT);
        RecordIrradianceCopy(commandList, set);
        firstPass = static_cast<unsigned int>(TimedPass::Multiscatter);
        endPass = static_cast<unsigned int>(TimedPass::Count);
        break;
    }

    if(m_passTiming && endPass > firstPass)
    {
        const unsigned int first = m_passTiming->currentSlot * PassTiming::Stamps + 2 * firstPass;
        commandList->ResolveQueryData(m_passTiming->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2 * (endPass - firstPass),
            m_passTiming->readback.Get(), std::uint64_t(first) * sizeof(std::uint64_t));
    }

    m_bakeState = BakeState::Recorded;
    return true;
}

void SkyAtmosphere::CommitSubmission(bool executed, std::uint64_t fenceValue)
{
    m_stagingRecorded = false;

    // S5B: the irradiance copy of this frame's last stage; a lost submission drops it with its rebuild.
    if(m_irradianceCopyRecorded)
    {
        m_irradianceCopyRecorded = false;
        m_irradianceCopyFence = executed && fenceValue != 0 ? std::optional<std::uint64_t>(fenceValue) : std::nullopt;
    }

    // S4: the background slice this frame recorded. Lost: the slot's contents are unknown and its build starts again.
    if(m_background.recorded)
    {
        if(executed)
        {
            const bool completes = m_background.recorded->step == BackgroundStep::Ready;
            m_background.progress = *m_background.recorded;

            if(completes)
            {
                ++m_backgroundGeneration;
                Log::Info("Physical sky: the " + std::to_string(m_background.width) + " HDRI background is complete.");
            }
        }
        else
        {
            m_background.progress = {BackgroundStep::Upload};
        }

        m_background.recorded.reset();
    }

    // S5: the clouds' timing slot of this frame, the noise volumes and the camera clouds recorded in it.
    if(m_cloudTiming)
    {
        m_cloudTiming->executed[unsigned(m_cloudTiming->frame % CloudTiming::Slots)] = executed;
    }

    if(m_noiseState == 1)
    {
        m_noiseState = executed ? 2u : 0u;
    }

    if(m_cloudCamera.recorded)
    {
        m_cloudCamera.recorded = false;
        m_cloudCamera.valid = executed;
    }

    if(m_bakeState != BakeState::Recorded)
    {
        return;
    }

    if(!executed)
    {
        // The commands may or may not have run: the set is unknown, and so are the LUTs if this stage wrote them. The rebuild
        // starts again from stage 1 on this queue.
        DropRebuild();
        return;
    }

    switch(m_stage)
    {
    case RebuildStage::Clouds:
        m_lutAtmosphere = m_pendingLutAtmosphere;
        m_pendingLutAtmosphere.reset();
        m_stage = RebuildStage::Environment;
        m_bakeState = BakeState::Building;
        break;
    case RebuildStage::Environment:
        m_lutAtmosphere = m_pendingLutAtmosphere;
        m_pendingLutAtmosphere.reset();
        m_stage = RebuildStage::Cubes;
        m_bakeState = BakeState::Building;
        break;
    case RebuildStage::Cubes:
        m_stage = RebuildStage::Multiscatter;
        m_bakeState = BakeState::Building;
        break;
    case RebuildStage::Multiscatter:
        m_bakeState = BakeState::Submitted;

        if(m_passTiming)
        {
            m_passTiming->executed[m_passTiming->currentSlot] = true;
        }

        break;
    }
}

bool SkyAtmosphere::CreateCloudPipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* entryPoint,
    ComputePipeline& pipeline)
{
    ShaderDescription shaderDescription{};
    shaderDescription.path = "SkyAtmosphere.hlsl";
    shaderDescription.entryPoint = entryPoint;
    shaderDescription.stage = ShaderStage::Compute;
    Shader* shader = shaderLibrary.Load(shaderDescription);

    if(!shader)
    {
        return false;
    }

    ComputePipelineDescription description{};
    description.computeShader = shader;

    for(unsigned int registerIndex = 0; registerIndex < 2; ++registerIndex)
    {
        RootParameterDescription constants{};
        constants.type = RootParameterType::ConstantBuffer;
        constants.shaderRegister = registerIndex;
        constants.visibility = D3D12_SHADER_VISIBILITY_ALL;
        description.rootParameters.push_back(constants);
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    description.staticSamplers.push_back(sampler);
    // s1: the noise volumes tile.
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 1;
    description.staticSamplers.push_back(sampler);
    return pipeline.Initialize(device, description);
}

bool SkyAtmosphere::CreateCloudResources(ID3D12Device* device, ShaderLibrary& shaderLibrary, DescriptorHeap& resourceHeap)
{
    m_device = device;

    const auto createVolume = [&](CloudVolume& volume, unsigned int size)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        description.Width = size;
        description.Height = size;
        description.DepthOrArraySize = static_cast<UINT16>(size);
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if(FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&volume.resource))))
        {
            return false;
        }

        MemoryRegistry::TrackResource(volume.resource, MemoryCategory::MeshesTextures);
        volume.size = size;
        volume.state = D3D12_RESOURCE_STATE_COMMON;
        volume.srv = resourceHeap.Allocate();
        volume.uav = resourceHeap.Allocate();

        if(volume.srv == UINT_MAX || volume.uav == UINT_MAX)
        {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture3D.MipLevels = 1;
        device->CreateShaderResourceView(volume.resource.Get(), &view, resourceHeap.GetCPUHandle(volume.srv));
        D3D12_UNORDERED_ACCESS_VIEW_DESC access{};
        access.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        access.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        access.Texture3D.WSize = size;
        device->CreateUnorderedAccessView(volume.resource.Get(), nullptr, &access, resourceHeap.GetCPUHandle(volume.uav));
        return true;
    };

    bool created = createVolume(m_shapeNoise, CloudShapeNoiseSize) && createVolume(m_detailNoise, CloudDetailNoiseSize) &&
        m_cloudAmbient.Initialize(device, 2, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            ShaderRead) &&
        m_cloudAmbient.CreateSRV(device, resourceHeap) && m_cloudAmbient.CreateUAV(device, resourceHeap) &&
        m_cloudShadow.Initialize(device, CloudShadowSize, CloudShadowSize, 1, 1, DXGI_FORMAT_R16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
        m_cloudShadow.CreateSRV(device, resourceHeap) && m_cloudShadow.CreateUAV(device, resourceHeap) &&
        m_cloudMoonShadow.Initialize(device, CloudShadowSize, CloudShadowSize, 1, 1, DXGI_FORMAT_R16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
        m_cloudMoonShadow.CreateSRV(device, resourceHeap) && m_cloudMoonShadow.CreateUAV(device, resourceHeap) &&
        m_frameInfo.InitializeStorage(device, sizeof(SkyFrameInfoData), 1, resourceHeap);

    for(EnvironmentSet& set : m_sets)
    {
        created = created && set.cloudEnvironment.Initialize(device, CloudEnvironmentWidth, CloudEnvironmentHeight, 1, 1,
            DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
            set.cloudEnvironment.CreateSRV(device, resourceHeap) && set.cloudEnvironment.CreateUAV(device, resourceHeap);
    }

    created = created && CreateCloudPipeline(device, shaderLibrary, "CSCloudShapeNoise", m_cloudShapePipeline) &&
        CreateCloudPipeline(device, shaderLibrary, "CSCloudDetailNoise", m_cloudDetailPipeline) &&
        CreateCloudPipeline(device, shaderLibrary, "CSCloudAmbient", m_cloudAmbientPipeline) &&
        CreateCloudPipeline(device, shaderLibrary, "CSCloudEnvironment", m_cloudEnvironmentPipeline) &&
        CreateCloudPipeline(device, shaderLibrary, "CSCloudShadow", m_cloudShadowPipeline) &&
        CreateCloudPipeline(device, shaderLibrary, "CSCloudTrace", m_cloudTracePipeline) &&
        CreateCloudPipeline(device, shaderLibrary, "CSCloudReproject", m_cloudReprojectPipeline);

    if(!created)
    {
        return false;
    }

    const Shader* vertexShader = shaderLibrary.Load({"SkyCloudComposite.hlsl", "VSComposite", ShaderStage::Vertex});
    const Shader* pixelShader = shaderLibrary.Load({"SkyCloudComposite.hlsl", "PSComposite", ShaderStage::Pixel});

    if(!vertexShader || !pixelShader)
    {
        return false;
    }

    // scene * T + light, with T itself as the destination factor (sun behind clouds, 2026-11-25: a small T keeps its value).
    PipelineDescription description;
    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.renderTargets.count = 1;
    description.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.transmittanceBlend = true;
    description.rootParameters.push_back({RootParameterType::ConstantBuffer, 0, 0, D3D12_SHADER_VISIBILITY_ALL});
    return m_cloudCompositePipeline.Initialize(device, description);
}

bool SkyAtmosphere::ResizeClouds(ID3D12Device* device, DescriptorHeap& resourceHeap, unsigned int outputWidth,
    unsigned int outputHeight)
{
    CloudCamera& camera = m_cloudCamera;
    const unsigned int width = (outputWidth + 1) / 2;
    const unsigned int height = (outputHeight + 1) / 2;

    if(camera.width == width && camera.height == height)
    {
        return true;
    }

    camera.trace.Shutdown();
    camera.traceDepth.Shutdown();

    for(unsigned int index = 0; index < 2; ++index)
    {
        camera.history[index].Shutdown();
        camera.historyDepth[index].Shutdown();
    }

    camera.width = camera.height = camera.traceWidth = camera.traceHeight = 0;
    camera.valid = false;

    if(!m_initialized || !device || width == 0 || height == 0)
    {
        return true;
    }

    const MemoryCategoryScope memoryScope(MemoryCategory::MeshesTextures);
    const unsigned int traceWidth = (width + 3) / 4;
    const unsigned int traceHeight = (height + 3) / 4;
    const auto target = [&](Texture& texture, unsigned int w, unsigned int h, DXGI_FORMAT format)
    {
        return texture.Initialize(device, w, h, 1, 1, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ShaderRead) &&
            texture.CreateSRV(device, resourceHeap) && texture.CreateUAV(device, resourceHeap);
    };
    bool created = target(camera.trace, traceWidth, traceHeight, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        target(camera.traceDepth, traceWidth, traceHeight, DXGI_FORMAT_R32_FLOAT);

    for(unsigned int index = 0; index < 2; ++index)
    {
        created = created && target(camera.history[index], width, height, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
            target(camera.historyDepth[index], width, height, DXGI_FORMAT_R32_FLOAT);
    }

    if(!created)
    {
        Log::Error("Physical sky: the camera cloud targets could not be created; clouds are not drawn on camera rays.");
        return false;
    }

    camera.width = width;
    camera.height = height;
    camera.traceWidth = traceWidth;
    camera.traceHeight = traceHeight;
    return true;
}

bool SkyAtmosphere::PublishedHasClouds() const noexcept
{
    return m_published && m_publishedInputs && !m_publishedInputs->hdri && !m_publishedInputs->settings.cloudLayers.empty();
}

void SkyAtmosphere::StampCloudPass(ID3D12GraphicsCommandList* commandList, CloudPass pass, bool end)
{
    if(!m_cloudTiming)
    {
        return;
    }

    CloudTiming& timing = *m_cloudTiming;
    const unsigned int slot = unsigned(timing.frame % CloudTiming::Slots);
    const unsigned int first = slot * CloudTiming::Stamps + 2 * static_cast<unsigned int>(pass);
    commandList->EndQuery(timing.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + (end ? 1u : 0u));

    if(end)
    {
        commandList->ResolveQueryData(timing.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2, timing.readback.Get(),
            std::uint64_t(first) * sizeof(std::uint64_t));
        timing.recorded[slot] |= 1u << static_cast<unsigned int>(pass);
    }
}

void SkyAtmosphere::RecordFrame(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, UploadBuffer& uploadBuffer,
    const FrameParameters& frame)
{
    m_frameInfoActive = false;

    if(!m_initialized || !commandList)
    {
        return;
    }

    // Sun behind clouds (2026-11-25): the cone in which the clouds' march carries on past its early-out (SkyMarchClouds), so a disk
    // behind a cloud gets the cloud's true transmittance: the larger of the sun's and the moon's disks plus 2 degrees (the cloud
    // map's texel is about 1.4 degrees; a camera-cloud block is a fraction of a degree).
    {
        constexpr double Pi = 3.14159265358979323846;
        const double radius = (std::max)(double(frame.sunAngularRadius), double(frame.moonAngularRadius)) + 2.0 * Pi / 180.0;
        m_cloudDiskCosine = float(std::cos((std::min)(radius, Pi)));
    }

    // S5 timing: a new frame's slot; first read what the frame Slots frames ago measured (its submission has completed).
    if(m_cloudTiming)
    {
        CloudTiming& timing = *m_cloudTiming;
        ++timing.frame;
        const unsigned int slot = unsigned(timing.frame % CloudTiming::Slots);

        if(timing.executed[slot] && timing.recorded[slot] != 0u)
        {
            constexpr std::size_t SlotBytes = CloudTiming::Stamps * sizeof(std::uint64_t);
            const D3D12_RANGE range{slot * SlotBytes, (slot + 1) * SlotBytes};
            void* mapped = nullptr;

            if(SUCCEEDED(timing.readback->Map(0, &range, &mapped)))
            {
                std::uint64_t stamps[CloudTiming::Stamps]{};
                std::memcpy(stamps, static_cast<const std::uint8_t*>(mapped) + range.Begin, SlotBytes);
                const D3D12_RANGE nothingWritten{0, 0};
                timing.readback->Unmap(0, &nothingWritten);

                for(unsigned int pass = 0; pass < CloudTiming::Passes; ++pass)
                {
                    if(timing.recorded[slot] & (1u << pass))
                    {
                        timing.samples[pass].push_back(double(stamps[2 * pass + 1] - stamps[2 * pass]) / timing.ticksPerMillisecond);
                    }
                }
            }
        }

        timing.recorded[slot] = 0u;
        timing.executed[slot] = false;
    }

    ID3D12DescriptorHeap* heaps[] = {resourceHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);

    // The noise volumes, once.
    if(m_noiseState == 0)
    {
        SkyCloudConstants noise{};
        noise.noiseOutputIndex = m_shapeNoise.uav;
        noise.noiseSize = m_shapeNoise.size;
        const D3D12_GPU_VIRTUAL_ADDRESS shapeAddress =
            uploadBuffer.Allocate(&noise, sizeof(noise), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        noise.noiseOutputIndex = m_detailNoise.uav;
        noise.noiseSize = m_detailNoise.size;
        const D3D12_GPU_VIRTUAL_ADDRESS detailAddress =
            uploadBuffer.Allocate(&noise, sizeof(noise), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        const SkyAtmosphereConstants atmosphere{};
        const D3D12_GPU_VIRTUAL_ADDRESS atmosphereAddress =
            uploadBuffer.Allocate(&atmosphere, sizeof(atmosphere), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(shapeAddress != 0 && detailAddress != 0 && atmosphereAddress != 0)
        {
            TransitionVolume(commandList, m_shapeNoise, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            TransitionVolume(commandList, m_detailNoise, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_cloudShapePipeline.Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, atmosphereAddress);
            commandList->SetComputeRootConstantBufferView(1, shapeAddress);
            commandList->Dispatch(GroupCount(m_shapeNoise.size, 4), GroupCount(m_shapeNoise.size, 4), GroupCount(m_shapeNoise.size, 4));
            m_cloudDetailPipeline.Bind(commandList);
            commandList->SetComputeRootConstantBufferView(1, detailAddress);
            commandList->Dispatch(GroupCount(m_detailNoise.size, 4), GroupCount(m_detailNoise.size, 4),
                GroupCount(m_detailNoise.size, 4));
            UnorderedAccessBarrier(commandList, m_shapeNoise.resource.Get());
            UnorderedAccessBarrier(commandList, m_detailNoise.resource.Get());
            TransitionVolume(commandList, m_shapeNoise, ShaderRead);
            TransitionVolume(commandList, m_detailNoise, ShaderRead);
            m_noiseState = 1;
        }
    }

    if(!frame.active || !m_published || !m_publishedInputs || m_publishedInputs->hdri)
    {
        return;
    }

    const BakeInputs& published = *m_publishedInputs;
    const PhysicalSkySettings& sky = published.settings;
    BakeInputs current = published;
    current.cameraAltitudeMeters = frame.cameraAltitudeMeters;
    SkyAtmosphereConstants atmosphere = MakeConstants(current, m_layout);
    atmosphere.transmittanceIndex = m_transmittanceLut.GetDescriptorIndex();
    atmosphere.multipleScatteringIndex = m_multipleScatteringLut.GetDescriptorIndex();
    const bool clouds = PublishedHasClouds();
    bool shadows = false;
    const Vector3 toSun = DirectionToSun(published.sunDirection);
    const Vector3 toMoon = DirectionToSun(published.moonDirection);
    const bool moon = published.moonIrradiance.x > 0.0f || published.moonIrradiance.y > 0.0f || published.moonIrradiance.z > 0.0f;
    // S5B: the moon's shadow map is drawn while the moon lights the frame from above the kernel's horizon limit.
    bool moonShadows = false;

    for(const CloudLayerSettings& layer : sky.cloudLayers)
    {
        shadows = shadows || layer.castShadows;
    }

    // The cloud shadow plane: texel-snapped around the camera, at the lowest shadowing layer's middle.
    const double texel = double(CloudShadowExtentMeters) / double(CloudShadowSize);
    const double originX = std::round(double(frame.cameraPosition.x) / texel) * texel;
    const double originZ = std::round(double(frame.cameraPosition.z) / texel) * texel;
    float referenceY = sky.seaLevelMeters;

    for(const CloudLayerSettings& layer : sky.cloudLayers)
    {
        if(layer.castShadows)
        {
            referenceY = float(double(sky.seaLevelMeters) + (double(layer.altitudeKm) + 0.5 * double(layer.thicknessKm)) * 1000.0);
            break;
        }
    }

    if(clouds && shadows && m_noiseState != 0)
    {
        SkyCloudConstants cloudConstants = MakeCloudConstants(sky, frame.timeSeconds, frame.cameraPosition.x, frame.cameraPosition.z);
        cloudConstants.shapeNoiseIndex = m_shapeNoise.srv;
        cloudConstants.detailNoiseIndex = m_detailNoise.srv;
        cloudConstants.ambientIndex = m_cloudAmbient.GetDescriptorIndex();
        // The shadow kernel works in the clouds' world coordinates around the origin of the world (the camera offset is zero).
        cloudConstants.cameraWorldKm[0] = 0.0f;
        cloudConstants.cameraWorldKm[1] = 0.0f;
        cloudConstants.shadowOrigin[0] = float(originX);
        cloudConstants.shadowOrigin[1] = float(originZ);
        cloudConstants.shadowExtent = CloudShadowExtentMeters;
        cloudConstants.shadowReferenceY = referenceY;
        cloudConstants.shadowOutputIndex = m_cloudShadow.GetUAVIndex();
        cloudConstants.shadowSize = CloudShadowSize;
        const D3D12_GPU_VIRTUAL_ADDRESS atmosphereAddress =
            uploadBuffer.Allocate(&atmosphere, sizeof(atmosphere), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        const D3D12_GPU_VIRTUAL_ADDRESS cloudAddress =
            uploadBuffer.Allocate(&cloudConstants, sizeof(cloudConstants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

        if(atmosphereAddress != 0 && cloudAddress != 0)
        {
            StampCloudPass(commandList, CloudPass::Shadow, false);
            m_cloudShadow.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_cloudShadowPipeline.Bind(commandList);
            commandList->SetComputeRootConstantBufferView(0, atmosphereAddress);
            commandList->SetComputeRootConstantBufferView(1, cloudAddress);
            commandList->Dispatch(GroupCount(CloudShadowSize, 8), GroupCount(CloudShadowSize, 8), 1);
            UnorderedAccessBarrier(commandList, m_cloudShadow.GetResource());
            m_cloudShadow.Transition(commandList, ShaderRead);

            // S5B: the moon's map. The kernel marches along the atmosphere constants' directionToSun, so its copy carries the moon's
            // direction; same plane, layers and noise.
            if(moon && toMoon.y > 0.02f)
            {
                SkyAtmosphereConstants moonAtmosphere = atmosphere;
                moonAtmosphere.directionToSun[0] = toMoon.x;
                moonAtmosphere.directionToSun[1] = toMoon.y;
                moonAtmosphere.directionToSun[2] = toMoon.z;
                SkyCloudConstants moonCloud = cloudConstants;
                moonCloud.shadowOutputIndex = m_cloudMoonShadow.GetUAVIndex();
                const D3D12_GPU_VIRTUAL_ADDRESS moonAtmosphereAddress =
                    uploadBuffer.Allocate(&moonAtmosphere, sizeof(moonAtmosphere), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
                const D3D12_GPU_VIRTUAL_ADDRESS moonCloudAddress =
                    uploadBuffer.Allocate(&moonCloud, sizeof(moonCloud), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

                if(moonAtmosphereAddress != 0 && moonCloudAddress != 0)
                {
                    m_cloudMoonShadow.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    commandList->SetComputeRootConstantBufferView(0, moonAtmosphereAddress);
                    commandList->SetComputeRootConstantBufferView(1, moonCloudAddress);
                    commandList->Dispatch(GroupCount(CloudShadowSize, 8), GroupCount(CloudShadowSize, 8), 1);
                    UnorderedAccessBarrier(commandList, m_cloudMoonShadow.GetResource());
                    m_cloudMoonShadow.Transition(commandList, ShaderRead);
                    moonShadows = true;
                }
            }

            StampCloudPass(commandList, CloudPass::Shadow, true);
        }
        else
        {
            shadows = false;
        }
    }

    // The frame information (SkyFrame.hlsli).
    SkyFrameInfoData info{};
    info.flags = (sky.nightSkyIntensity > 0.0f && m_starsForTesting ? 1u : 0u) | (moon && frame.moonAngularRadius > 0.0f ? 2u : 0u) |
        (clouds && shadows ? 4u : 0u) | (clouds ? 8u : 0u) | (clouds && shadows && moonShadows ? 16u : 0u);
    info.moonCloudShadowIndex = m_cloudMoonShadow.GetDescriptorIndex();
    info.transmittanceIndex = m_transmittanceLut.GetDescriptorIndex();
    info.transmittanceWidth = TransmittanceWidth;
    info.transmittanceHeight = TransmittanceHeight;
    info.bottomRadius = atmosphere.bottomRadius;
    info.topRadius = atmosphere.topRadius;
    info.cameraAltitude = atmosphere.cameraAltitude;
    info.starRadiance = float(frame.renderScale * double(sky.nightSkyIntensity) * PhysicalSkyLuminanceFactor(sky));
    info.planetRotation = atmosphere.planetRotation;
    info.starDiskRadius = StarDiskRadius;
    info.cloudShadowIndex = m_cloudShadow.GetDescriptorIndex();
    info.cloudEnvironmentIndex = m_sets[m_publishedSet].cloudEnvironment.GetDescriptorIndex();
    info.cloudShadowOrigin[0] = float(originX);
    info.cloudShadowOrigin[1] = float(originZ);
    info.cloudShadowInvExtent = 1.0f / CloudShadowExtentMeters;
    info.cloudShadowReferenceY = referenceY;
    info.directionToSun[0] = toSun.x;
    info.directionToSun[1] = toSun.y;
    info.directionToSun[2] = toSun.z;
    info.cloudShadowSize = CloudShadowSize;
    info.directionToMoon[0] = toMoon.x;
    info.directionToMoon[1] = toMoon.y;
    info.directionToMoon[2] = toMoon.z;
    info.moonCosRadius = std::cos(frame.moonAngularRadius);
    // The moon's disk: its light at the camera spread over the disk, clamped as the sun's disk is.
    const double sinSquared = std::sin(double(frame.moonAngularRadius)) * std::sin(double(frame.moonAngularRadius));
    const double diskScale = sinSquared > 0.0 ? 1.0 / (3.14159265358979323846 * sinSquared) : 0.0;
    const double diskLuminance = Photometry::Luminance(frame.moonGroundIrradiance) * diskScale;
    const double clamp = diskLuminance > 60000.0 ? 60000.0 / diskLuminance : 1.0;
    info.moonDiskRadiance[0] = float(double(frame.moonGroundIrradiance.x) * diskScale * clamp);
    info.moonDiskRadiance[1] = float(double(frame.moonGroundIrradiance.y) * diskScale * clamp);
    info.moonDiskRadiance[2] = float(double(frame.moonGroundIrradiance.z) * diskScale * clamp);
    info.cloudEnvironmentWidth = CloudEnvironmentWidth;
    info.cloudEnvironmentHeight = CloudEnvironmentHeight;
    UploadLocation location;

    if(uploadBuffer.Allocate(&info, sizeof(info), 16, location) == 0 || !location.resource)
    {
        return;
    }

    // COMMON between frames (a buffer decays at the end of the submission): the copy promotes it; readers need an explicit
    // transition from the copy.
    commandList->CopyBufferRegion(m_frameInfo.GetResource(), 0, location.resource, location.offset, sizeof(info));
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_frameInfo.GetResource();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = ShaderRead;
    commandList->ResourceBarrier(1, &barrier);
    m_frameInfoActive = true;
}

bool SkyAtmosphere::RecordClouds(ID3D12GraphicsCommandList* commandList, DescriptorHeap& resourceHeap, UploadBuffer& uploadBuffer,
    RenderTarget& sceneColor, DepthBuffer& depth, const Matrix4& viewProjection, const Vector3& cameraPosition,
    double cameraAltitudeMeters, double timeSeconds, const std::function<void(bool)>& betweenParts)
{
    CloudCamera& camera = m_cloudCamera;

    if(!m_initialized || !commandList || !PublishedHasClouds() || camera.width == 0 || m_noiseState == 0)
    {
        return false;
    }

    const BakeInputs& published = *m_publishedInputs;
    BakeInputs current = published;
    current.cameraAltitudeMeters = cameraAltitudeMeters;
    SkyAtmosphereConstants atmosphere = MakeConstants(current, m_layout);
    atmosphere.cloudDiskCosine = m_cloudDiskCosine;
    atmosphere.transmittanceIndex = m_transmittanceLut.GetDescriptorIndex();
    atmosphere.multipleScatteringIndex = m_multipleScatteringLut.GetDescriptorIndex();
    const Matrix4 inverseViewProjection = viewProjection.Inverse();
    const unsigned int previous = camera.current;
    const unsigned int next = previous ^ 1u;
    SkyCloudConstants clouds = MakeCloudConstants(published.settings, timeSeconds, cameraPosition.x, cameraPosition.z);
    clouds.shapeNoiseIndex = m_shapeNoise.srv;
    clouds.detailNoiseIndex = m_detailNoise.srv;
    clouds.ambientIndex = m_cloudAmbient.GetDescriptorIndex();
    clouds.frame = camera.frame;
    clouds.historyValid = camera.valid ? 1u : 0u;
    // Camera-relative matrices (the camera at the origin): the trace's directions and the history's reprojection keep their
    // precision far from the world origin (the Sparse fix, 2026-11-25).
    const Matrix4 relativeViewProjection = viewProjection * Matrix4::Translation(cameraPosition);
    clouds.inverseViewProjection = relativeViewProjection.Inverse();
    clouds.previousViewProjection = camera.previousRelativeViewProjection;
    clouds.cameraMove[0] = float(double(cameraPosition.x) - double(camera.previousCameraPosition.x));
    clouds.cameraMove[1] = float(double(cameraPosition.y) - double(camera.previousCameraPosition.y));
    clouds.cameraMove[2] = float(double(cameraPosition.z) - double(camera.previousCameraPosition.z));
    // A reference-exposure change rescales the history instead of resetting it.
    clouds.historyScale = camera.valid ? float(std::exp2(double(camera.previousReferenceEV100) - double(published.referenceEV100))) :
        1.0f;
    clouds.traceWidth = camera.traceWidth;
    clouds.traceHeight = camera.traceHeight;
    clouds.width = camera.width;
    clouds.height = camera.height;
    clouds.traceOutputIndex = camera.trace.GetUAVIndex();
    clouds.traceDepthOutputIndex = camera.traceDepth.GetUAVIndex();
    clouds.historyIndex = camera.history[previous].GetDescriptorIndex();
    clouds.historyDepthIndex = camera.historyDepth[previous].GetDescriptorIndex();
    clouds.outputIndex = camera.history[next].GetUAVIndex();
    clouds.depthOutputIndex = camera.historyDepth[next].GetUAVIndex();
    clouds.traceIndex = camera.trace.GetDescriptorIndex();
    clouds.traceDepthIndex = camera.traceDepth.GetDescriptorIndex();

    CloudCompositeConstants composite{};
    composite.inverseViewProjection = inverseViewProjection;
    composite.cameraPosition[0] = cameraPosition.x;
    composite.cameraPosition[1] = cameraPosition.y;
    composite.cameraPosition[2] = cameraPosition.z;
    composite.depthIndex = depth.GetSRVIndex();
    composite.cloudIndex = camera.history[next].GetDescriptorIndex();
    composite.cloudDepthIndex = camera.historyDepth[next].GetDescriptorIndex();

    const D3D12_GPU_VIRTUAL_ADDRESS atmosphereAddress =
        uploadBuffer.Allocate(&atmosphere, sizeof(atmosphere), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const D3D12_GPU_VIRTUAL_ADDRESS cloudAddress =
        uploadBuffer.Allocate(&clouds, sizeof(clouds), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const D3D12_GPU_VIRTUAL_ADDRESS compositeAddress =
        uploadBuffer.Allocate(&composite, sizeof(composite), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(atmosphereAddress == 0 || cloudAddress == 0 || compositeAddress == 0)
    {
        Log::Error("Failed to allocate the clouds' constants; they are skipped this frame.");
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = {resourceHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    TransitionVolume(commandList, m_shapeNoise, ShaderRead);
    TransitionVolume(commandList, m_detailNoise, ShaderRead);

    // 1. Trace one pixel of each block.
    StampCloudPass(commandList, CloudPass::Trace, false);
    camera.trace.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    camera.traceDepth.Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cloudTracePipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, atmosphereAddress);
    commandList->SetComputeRootConstantBufferView(1, cloudAddress);
    commandList->Dispatch(GroupCount(camera.traceWidth, 8), GroupCount(camera.traceHeight, 8), 1);
    UnorderedAccessBarrier(commandList, camera.trace.GetResource());
    UnorderedAccessBarrier(commandList, camera.traceDepth.GetResource());
    camera.trace.Transition(commandList, ShaderRead);
    camera.traceDepth.Transition(commandList, ShaderRead);
    StampCloudPass(commandList, CloudPass::Trace, true);

    // 2. Reproject into the other history.
    StampCloudPass(commandList, CloudPass::Reproject, false);
    camera.history[previous].Transition(commandList, ShaderRead);
    camera.historyDepth[previous].Transition(commandList, ShaderRead);
    camera.history[next].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    camera.historyDepth[next].Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_cloudReprojectPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, atmosphereAddress);
    commandList->SetComputeRootConstantBufferView(1, cloudAddress);
    commandList->Dispatch(GroupCount(camera.width, 8), GroupCount(camera.height, 8), 1);
    UnorderedAccessBarrier(commandList, camera.history[next].GetResource());
    UnorderedAccessBarrier(commandList, camera.historyDepth[next].GetResource());
    camera.history[next].Transition(commandList, ShaderRead);
    camera.historyDepth[next].Transition(commandList, ShaderRead);
    StampCloudPass(commandList, CloudPass::Reproject, true);

    if(betweenParts)
    {
        betweenParts(true);
    }

    // 3. The composite into the scene colour.
    StampCloudPass(commandList, CloudPass::Composite, false);
    const D3D12_RESOURCE_STATES depthState = depth.GetState();
    depth.Transition(commandList, ShaderRead);
    sceneColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = sceneColor.GetRTV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(sceneColor.GetWidth()), float(sceneColor.GetHeight()), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, LONG(sceneColor.GetWidth()), LONG(sceneColor.GetHeight())};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    m_cloudCompositePipeline.Bind(commandList);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(0, compositeAddress);
    commandList->DrawInstanced(3, 1, 0, 0);
    depth.Transition(commandList, depthState);
    StampCloudPass(commandList, CloudPass::Composite, true);

    if(betweenParts)
    {
        betweenParts(false);
    }

    camera.current = next;
    camera.previousRelativeViewProjection = relativeViewProjection;
    camera.previousCameraPosition = cameraPosition;
    camera.previousReferenceEV100 = published.referenceEV100;
    camera.frame = (camera.frame + 1u) % 16u;
    camera.recorded = true;
    return true;
}

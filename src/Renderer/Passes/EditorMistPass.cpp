#include "Renderer/Passes/EditorMistPass.h"

#include "Renderer/DepthBuffer.h"
#include "Renderer/DescriptorHeap.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/ResourceUploader.h"
#include "Renderer/Shader/ShaderLibrary.h"
#include "Renderer/UploadBuffer.h"
#include "Core/Log.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// Must match the EditorMistConstants cbuffer of EditorMist.hlsl.
struct EditorMistConstants
{
    Matrix4 clipToWorld;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float extinction = 0.0f;
    AirFieldBinding field;
    std::uint32_t mist0Index = 0;
    std::uint32_t mist1Index = 0;
    std::uint32_t occupancyIndex = 0;
    std::uint32_t depthIndex = 0;
    std::uint32_t fullSize[2] = {0, 0};
    std::uint32_t reducedSize[2] = {0, 0};
    std::uint32_t resultIndex = 0;
    std::uint32_t distanceIndex = 0;
    std::uint32_t maxSteps = 0;
    float minimumStep = 0.0f;
    // AIR.4F F4.1 detail: AirMistDetail, then the texture and whether detail is on.
    float phaseOffsetA[3] = {0.0f, 0.0f, 0.0f};
    float detailStrength = 0.0f;
    float phaseOffsetB[3] = {0.0f, 0.0f, 0.0f};
    float inverseDetailScale = 0.0f;
    float phaseTimeA = 0.0f;
    float phaseTimeB = 0.0f;
    float phaseWeightA = 1.0f;
    float noiseMeanInverse = 1.0f;
    std::uint32_t noiseIndex = 0;
    std::uint32_t detailEnabled = 0;
    // AIR.4F F4.2.
    std::uint32_t jitterEnabled = 1;
    std::uint32_t wideUpsample = 1;
    // AIR.4F F4.3.
    std::uint32_t lightingEnabled = 0;
    std::uint32_t visibilityIndex = UINT32_MAX;
    float scattering = 0.0f;
    float anisotropy = 0.0f;
    float sunDirection[3] = {0.0f, -1.0f, 0.0f};
    float lightingPadding0 = 0.0f;
    float sunRadiance[3] = {0.0f, 0.0f, 0.0f};
    float lightingPadding1 = 0.0f;
    float ambient = 0.0f;
    std::uint32_t irradianceIndex = UINT32_MAX;
    float lightingPadding2[2] = {0.0f, 0.0f};
    // AIR.4F F4.4: AirMistCurl, then the texture and whether curl is on.
    float curlDisplacement = 0.0f;
    float curlInverseScale = 0.0f;
    float curlFlow = 0.0f;
    float curlPadding0 = 0.0f;
    std::uint32_t curlIndex = UINT32_MAX;
    std::uint32_t curlEnabled = 0;
    std::uint32_t curlPadding1[2] = {0, 0};
    // AIR.4F F4.4.2: AirMistWisps, then the texture and whether wisps are on.
    float wispStrength = 0.0f;
    float wispInverseScale = 0.0f;
    float wispMeanInverse = 1.0f;
    float wispSquareMeanInverse = 1.0f;
    std::uint32_t wispIndex = UINT32_MAX;
    std::uint32_t wispEnabled = 0;
    // AIR.4F F4.4.2e: the density-dependent detail's strength (0: off).
    float densityDetailStrength = 0.0f;
    float densityPadding = 0.0f;
};

static_assert(sizeof(EditorMistConstants) == 384);

// A phase cycle's texture offset: three values in [0, 1) from an integer hash of the cycle's index, so each restart shows another part
// of the tile rather than pulsing the same pattern.
void CycleOffset(std::int64_t cycle, float (&offset)[3])
{
    std::uint64_t state = static_cast<std::uint64_t>(cycle) * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;

    for(float& component : offset)
    {
        state ^= state >> 29;
        state *= 0xBF58476D1CE4E5B9ull;
        state ^= state >> 32;
        component = static_cast<float>(state >> 40) / 16777216.0f;
    }
}

constexpr std::uint32_t BrickCells = 8;
// The depth buffer's state while the raymarch and the composite read it.
constexpr D3D12_RESOURCE_STATES DepthReadState = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES ShaderReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void UnorderedAccessBarrier(ID3D12GraphicsCommandList* commandList)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &barrier);
}

// What a compute pipeline binds besides its constants (b0).
struct ComputeBindings
{
    // AIR.4F F4.1: the Worley texture's sampler (s0).
    bool noiseSampler = false;
    // AIR.4F F4.3: the scene's Light table (b1) and acceleration structure (t0).
    bool lightTable = false;
    bool topLevel = false;
};

bool MakeComputePipeline(ID3D12Device* device, ShaderLibrary& shaderLibrary, const char* entryPoint, ComputePipeline& pipeline,
    ComputeBindings bindings = {})
{
    const Shader* shader = shaderLibrary.Load({"EditorMist.hlsl", entryPoint, ShaderStage::Compute});

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

    if(bindings.lightTable)
    {
        RootParameterDescription table{};
        table.type = RootParameterType::ConstantBuffer;
        table.shaderRegister = 1;
        table.visibility = D3D12_SHADER_VISIBILITY_ALL;
        description.rootParameters.push_back(table);
    }

    if(bindings.topLevel)
    {
        RootParameterDescription topLevel{};
        topLevel.type = RootParameterType::ShaderResource;
        topLevel.shaderRegister = 0;
        topLevel.visibility = D3D12_SHADER_VISIBILITY_ALL;
        description.rootParameters.push_back(topLevel);
    }

    // AIR.4F F4.1: the Worley texture tiles on every axis and is filtered trilinearly.
    if(bindings.noiseSampler)
    {
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        description.staticSamplers.push_back(sampler);
    }

    return pipeline.Initialize(device, description);
}
}

AirMistRenderRequest MakeAirMistRenderRequest(const MistRenderSettings& settings, AirMistQuality quality) noexcept
{
    AirMistRenderRequest request;
    request.opacity = settings.opacity;
    request.quality = quality;
    request.detail = settings.detail;
    request.detailStrength = settings.detailStrength;
    request.detailScale = settings.detailScale;
    request.curl = settings.curl;
    request.curlStrength = settings.curlStrength;
    request.curlScale = settings.curlScale;
    request.curlFlow = settings.curlFlow;
    request.wisps = settings.wisps;
    request.wispStrength = settings.wispStrength;
    request.wispScale = settings.wispScale;
    request.densityDetail = settings.densityDetail;
    request.densityDetailStrength = settings.densityDetailStrength;
    request.lighting = settings.lighting;
    request.sunShadows = settings.sunShadows;
    request.scattering = settings.scattering;
    request.anisotropy = settings.anisotropy;
    request.ambient = settings.ambient;
    return request;
}

AirMistDetailPhases EditorMistPass::DetailPhasesAt(double mistSeconds) noexcept
{
    AirMistDetailPhases phases;
    const double cycles = (std::max)(0.0, std::isfinite(mistSeconds) ? mistSeconds : 0.0) / DetailPhaseSeconds;
    const double cycleA = std::floor(cycles);
    const double cycleB = std::floor(cycles + 0.5);
    const double phaseA = cycles - cycleA;
    const double phaseB = cycles + 0.5 - cycleB;
    CycleOffset(static_cast<std::int64_t>(cycleA) * 2, phases.offsetA);
    CycleOffset(static_cast<std::int64_t>(cycleB) * 2 + 1, phases.offsetB);
    phases.timeA = static_cast<float>(phaseA * DetailPhaseSeconds);
    phases.timeB = static_cast<float>(phaseB * DetailPhaseSeconds);
    phases.weightA = static_cast<float>(1.0 - std::abs(2.0 * phaseA - 1.0));
    return phases;
}

AirMistQualitySettings AirMistQualityOf(AirMistQuality quality) noexcept
{
    switch(quality)
    {
    case AirMistQuality::Low:
        return {0.5f, 32};
    case AirMistQuality::High:
        return {0.70710678f, 96};
    case AirMistQuality::Medium:
        break;
    }

    return {0.5f, 64};
}

bool EditorMistPass::LoadScalarVolume(ResourceUploader& uploader, const std::filesystem::path& path, const char* what,
    ScalarVolume& volume)
{
    DirectX::TexMetadata metadata{};
    DirectX::ScratchImage image;

    if(FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image)))
    {
        Log::Warning(std::string("Mist ") + what + " texture missing or unreadable (" + path.string() + "): mist draws without it.");
        return false;
    }

    const bool cube = metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE3D && metadata.format == DXGI_FORMAT_R8_UNORM &&
        metadata.width == metadata.height && metadata.width == metadata.depth && metadata.width >= 4 && metadata.width <= 256 &&
        (metadata.width & (metadata.width - 1)) == 0;

    if(!cube)
    {
        Log::Warning(std::string("Mist ") + what + " texture is not a power-of-two R8_UNORM 3D cube (" + path.string() +
            "): mist draws without it.");
        return false;
    }

    const std::uint32_t size = static_cast<std::uint32_t>(metadata.width);
    const DirectX::Image* top = image.GetImage(0, 0, 0);
    const std::size_t texels = std::size_t{size} * size * size;

    if(top == nullptr || top->rowPitch != size || top->slicePitch != std::size_t{size} * size)
    {
        Log::Warning(std::string("Mist ") + what + " texture has an unexpected layout (" + path.string() + "): mist draws without it.");
        return false;
    }

    // 1 / the mean, so noise / mean averages 1 and modulation keeps the mist's average density; likewise for the square (F4.4.2e).
    std::uint64_t sum = 0;
    std::uint64_t squareSum = 0;

    for(std::size_t index = 0; index < texels; ++index)
    {
        const std::uint64_t value = top->pixels[index];
        sum += value;
        squareSum += value * value;
    }

    if(sum == 0)
    {
        Log::Warning(std::string("Mist ") + what + " texture is blank (" + path.string() + "): mist draws without it.");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = size;
    description.Height = size;
    description.DepthOrArraySize = static_cast<UINT16>(size);
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8_UNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12Resource> noise;

    // Created in COMMON: the upload's copy promotes it, and shader reads promote it to a read state afterwards.
    if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&noise))) || !uploader.UploadTexture(noise.Get(), top->pixels, texels))
    {
        Log::Warning(std::string("Mist ") + what + " texture could not be uploaded: mist draws without it.");
        return false;
    }

    const unsigned int descriptor = m_descriptorHeap->Allocate();

    if(descriptor == UINT32_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture3D.MipLevels = 1;
    m_device->CreateShaderResourceView(noise.Get(), &view, m_descriptorHeap->GetCPUHandle(descriptor));
    volume.resource = noise;
    volume.descriptor = descriptor;
    volume.size = size;
    volume.meanInverse = static_cast<float>(255.0 * static_cast<double>(texels) / static_cast<double>(sum));
    volume.squareMeanInverse = static_cast<float>(255.0 * 255.0 * static_cast<double>(texels) / static_cast<double>(squareSum));
    return true;
}

bool EditorMistPass::LoadNoise(ResourceUploader& uploader, const std::filesystem::path& path)
{
    ScalarVolume volume;

    if(!LoadScalarVolume(uploader, path, "detail (Worley)", volume))
    {
        return false;
    }

    m_noise = volume.resource;
    m_noiseDescriptor = volume.descriptor;
    m_noiseSize = volume.size;
    m_noiseMeanInverse = volume.meanInverse;
    return true;
}

bool EditorMistPass::LoadFilaments(ResourceUploader& uploader, const std::filesystem::path& path)
{
    return LoadScalarVolume(uploader, path, "wisp (filament)", m_filaments);
}

void EditorMistPass::ReleaseFilaments()
{
    if(m_filaments.descriptor != UINT32_MAX && m_descriptorHeap != nullptr)
    {
        m_descriptorHeap->Free(m_filaments.descriptor);
    }

    m_filaments = ScalarVolume{};
}


void EditorMistPass::ReleaseNoise()
{
    if(m_noiseDescriptor != UINT32_MAX && m_descriptorHeap != nullptr)
    {
        m_descriptorHeap->Free(m_noiseDescriptor);
    }

    m_noiseDescriptor = UINT32_MAX;
    m_noise.Reset();
    m_noiseSize = 0;
    m_noiseMeanInverse = 1.0f;
}

bool EditorMistPass::LoadCurl(ResourceUploader& uploader, const std::filesystem::path& path)
{
    DirectX::TexMetadata metadata{};
    DirectX::ScratchImage image;

    if(path.empty() || FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image)))
    {
        Log::Warning("Mist curl texture missing or unreadable (" + path.string() + "): mist detail is never curl-warped.");
        return false;
    }

    const bool cube = metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE3D && metadata.format == DXGI_FORMAT_R16G16B16A16_SNORM &&
        metadata.width == metadata.height && metadata.width == metadata.depth && metadata.width >= 4 && metadata.width <= 256 &&
        (metadata.width & (metadata.width - 1)) == 0;
    const std::uint32_t size = static_cast<std::uint32_t>(metadata.width);
    const DirectX::Image* top = image.GetImage(0, 0, 0);
    const std::size_t texels = std::size_t{size} * size * size;

    if(!cube || top == nullptr || top->rowPitch != std::size_t{size} * 8 || top->slicePitch != std::size_t{size} * size * 8)
    {
        Log::Warning("Mist curl texture is not a power-of-two R16G16B16A16_SNORM 3D cube (" + path.string() + "): mist detail is never curl-warped.");
        return false;
    }

    // 1 / the RMS vector length, so the strength control is the RMS displacement in metres. SNORM decodes v / 32767, clamped to -1.
    double squares = 0.0;

    for(std::size_t index = 0; index < texels; ++index)
    {
        for(std::size_t component = 0; component < 3; ++component)
        {
            std::int16_t encoded = 0;
            std::memcpy(&encoded, top->pixels + index * 8 + component * 2, 2);
            const double value = (std::max)(static_cast<double>(encoded) / 32767.0, -1.0);
            squares += value * value;
        }
    }

    if(!(squares > 0.0))
    {
        Log::Warning("Mist curl texture is blank (" + path.string() + "): mist detail is never curl-warped.");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description.Width = size;
    description.Height = size;
    description.DepthOrArraySize = static_cast<UINT16>(size);
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D12Resource> curl;

    // Created in COMMON: the upload's copy promotes it, and shader reads promote it to a read state afterwards.
    if(FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(&curl))) || !uploader.UploadTexture(curl.Get(), top->pixels, texels * 8))
    {
        Log::Warning("Mist curl texture could not be uploaded: mist detail is never curl-warped.");
        return false;
    }

    const unsigned int descriptor = m_descriptorHeap->Allocate();

    if(descriptor == UINT32_MAX)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture3D.MipLevels = 1;
    m_device->CreateShaderResourceView(curl.Get(), &view, m_descriptorHeap->GetCPUHandle(descriptor));
    m_curl = curl;
    m_curlDescriptor = descriptor;
    m_curlSize = size;
    m_curlRmsInverse = static_cast<float>(1.0 / std::sqrt(squares / static_cast<double>(texels)));
    return true;
}

void EditorMistPass::ReleaseCurl()
{
    if(m_curlDescriptor != UINT32_MAX && m_descriptorHeap != nullptr)
    {
        m_descriptorHeap->Free(m_curlDescriptor);
    }

    m_curlDescriptor = UINT32_MAX;
    m_curl.Reset();
    m_curlSize = 0;
    m_curlRmsInverse = 1.0f;
}

bool EditorMistPass::Initialize(ID3D12Device* device, DescriptorHeap& descriptorHeap, ShaderLibrary& shaderLibrary, ResourceUploader& uploader,
    const std::filesystem::path& noisePath, const std::filesystem::path& curlPath, const std::filesystem::path& filamentPath)
{
    m_device = device;
    m_descriptorHeap = &descriptorHeap;

    if(!MakeComputePipeline(device, shaderLibrary, "CSOccupancy", m_occupancyPipeline) ||
        !MakeComputePipeline(device, shaderLibrary, "CSRaymarch", m_raymarchPipeline, {true, true, false}) ||
        !MakeComputePipeline(device, shaderLibrary, "CSSunVisibility", m_visibilityPipeline, {false, false, true}))
    {
        return false;
    }

    // A missing texture only disables detail (or, F4.4, its curl warp).
    LoadNoise(uploader, noisePath);
    LoadCurl(uploader, curlPath);
    LoadFilaments(uploader, filamentPath);

    const Shader* vertexShader = shaderLibrary.Load({"EditorMist.hlsl", "VSComposite", ShaderStage::Vertex});
    const Shader* pixelShader = shaderLibrary.Load({"EditorMist.hlsl", "PSComposite", ShaderStage::Pixel});

    if(!vertexShader || !pixelShader)
    {
        return false;
    }

    // Straight-alpha "over" blending of (colour / alpha, alpha): scene * transmittance + mist colour. No depth test: the composite
    // reads depth itself.
    PipelineDescription description;
    description.vertexShader = vertexShader;
    description.pixelShader = pixelShader;
    description.renderTargets.count = 1;
    description.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.alphaBlend = true;
    description.rootParameters.push_back({RootParameterType::ConstantBuffer, 0, 0, D3D12_SHADER_VISIBILITY_ALL});

    if(!m_compositePipeline.Initialize(device, description))
    {
        return false;
    }

    // AIR.4F F4.3: the same composite into the HDR scene colour, for lit mist (scene radiance, before tone mapping).
    description.renderTargets.formats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    return m_hdrCompositePipeline.Initialize(device, description);
}

void EditorMistPass::Shutdown()
{
    // Buffer has no destructor: each is shut down explicitly to release its resource and descriptors.
    for(RetiredBuffer& retired : m_retired)
    {
        retired.buffer.Shutdown();
    }

    m_retired.clear();

    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Shutdown();
    }

    m_width = 0;
    m_height = 0;
    m_bricks = 0;
    ReleaseNoise();
    ReleaseCurl();
    ReleaseFilaments();
    m_occupancyPipeline.Shutdown();
    m_raymarchPipeline.Shutdown();
    m_compositePipeline.Shutdown();
    m_visibilityPipeline.Shutdown();
    m_hdrCompositePipeline.Shutdown();
}

void EditorMistPass::RetireStorage(std::uint64_t fence)
{
    for(Buffer* buffer : StorageBuffers())
    {
        if(buffer->GetResource() != nullptr)
        {
            RetiredBuffer retired;
            retired.buffer = std::move(*buffer);
            retired.fence = fence;
            m_retired.push_back(std::move(retired));
        }
    }

    m_width = 0;
    m_height = 0;
    m_bricks = 0;
}

void EditorMistPass::CollectRetired(std::uint64_t completedFence)
{
    while(!m_retired.empty() && m_retired.front().fence <= completedFence)
    {
        m_retired.front().buffer.Shutdown();
        m_retired.pop_front();
    }
}

void EditorMistPass::Idle(std::uint64_t lastSubmissionFence, std::uint64_t completedFence)
{
    CollectRetired(completedFence);

    if(HasStorage())
    {
        RetireStorage(lastSubmissionFence);
    }
}

bool EditorMistPass::EnsureStorage(std::uint32_t width, std::uint32_t height, std::uint32_t bricks, std::uint64_t lastSubmissionFence)
{
    if(HasStorage() && width == m_width && height == m_height && bricks == m_bricks)
    {
        return true;
    }

    // A new size: frames still in flight may read the old buffers, so they wait for the last submission.
    RetireStorage(lastSubmissionFence);
    const auto create = [this](Buffer& buffer, std::size_t bytes)
    {
        return buffer.InitializeGPUWritable(m_device, bytes) && buffer.CreateRawSRV(m_device, *m_descriptorHeap) &&
            buffer.CreateRawUAV(m_device, *m_descriptorHeap);
    };
    const std::size_t pixels = std::size_t{width} * height;

    const std::size_t cells = std::size_t{bricks} * BrickCells * BrickCells * BrickCells;

    if(!create(m_result, pixels * 16) || !create(m_distance, pixels * 4) || !create(m_occupancy, std::size_t{bricks} * 4) ||
        !create(m_visibility, cells * 4))
    {
        // Never recorded into: released at once.
        for(Buffer* buffer : StorageBuffers())
        {
            buffer->Shutdown();
        }

        return false;
    }

    m_width = width;
    m_height = height;
    m_bricks = bricks;
    ++m_allocations;
    return true;
}

bool EditorMistPass::Execute(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
    RenderTarget& viewportColor, DepthBuffer& viewportDepth, const Matrix4& viewProjection, const Vector3& cameraPosition,
    const AirFieldBinding& field, const AirMistBinding& mist, const AirMistRenderRequest& request, double mistSeconds,
    std::uint64_t lastSubmissionFence, std::uint64_t completedFence, ID3D12QueryHeap* timestamps, UINT firstQuery,
    const AirMistLighting* lighting)
{
    CollectRetired(completedFence);
    const bool matchingTargets = viewportColor.GetWidth() == viewportDepth.GetWidth() && viewportColor.GetHeight() == viewportDepth.GetHeight();
    // AIR.4F F4.3: lit mist is scene radiance for the HDR scene colour; unlit mist is display colour for the final image.
    const bool lit = request.lighting;
    const DXGI_FORMAT targetFormat = lit ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const bool lightingReady = !lit || (lighting != nullptr && lighting->lightTable != 0);
    const bool drawable = m_compositePipeline.IsInitialized() && field.valid != 0 && mist.valid != 0 && matchingTargets &&
        viewportColor.GetWidth() > 0 && viewportColor.GetHeight() > 0 && std::isfinite(request.opacity) && request.opacity > 0.0f &&
        viewportColor.GetFormat() == targetFormat && lightingReady;

    if(!drawable)
    {
        Idle(lastSubmissionFence, completedFence);
        return false;
    }

    // The reduced target and the window's bricks (the window is a whole number of 8-cell bricks on each axis).
    const AirMistQualitySettings quality = AirMistQualityOf(request.quality);
    const std::uint32_t fullWidth = viewportColor.GetWidth();
    const std::uint32_t fullHeight = viewportColor.GetHeight();
    const float resolutionScale = request.resolutionScale > 0.0f && request.resolutionScale <= 1.0f ? request.resolutionScale : quality.scale;
    const std::uint32_t width = (std::max)(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(fullWidth) * resolutionScale)));
    const std::uint32_t height = (std::max)(1u, static_cast<std::uint32_t>(std::ceil(static_cast<float>(fullHeight) * resolutionScale)));
    const std::uint32_t brickCounts[3] = {field.dimensions[0] / BrickCells, field.dimensions[1] / BrickCells, field.dimensions[2] / BrickCells};
    const std::uint32_t bricks = brickCounts[0] * brickCounts[1] * brickCounts[2];

    if(bricks == 0 || !EnsureStorage(width, height, bricks, lastSubmissionFence))
    {
        return false;
    }

    EditorMistConstants constants;
    constants.clipToWorld = viewProjection.Inverse();
    constants.cameraPosition[0] = cameraPosition.x;
    constants.cameraPosition[1] = cameraPosition.y;
    constants.cameraPosition[2] = cameraPosition.z;
    constants.extinction = request.opacity * ExtinctionPerDensity;
    constants.field = field;
    constants.mist0Index = mist.piece0Index;
    constants.mist1Index = mist.piece1Index;
    constants.occupancyIndex = m_occupancy.GetUAVDescriptorIndex();
    constants.depthIndex = viewportDepth.GetSRVIndex();
    constants.fullSize[0] = fullWidth;
    constants.fullSize[1] = fullHeight;
    constants.reducedSize[0] = width;
    constants.reducedSize[1] = height;
    constants.resultIndex = m_result.GetUAVDescriptorIndex();
    constants.distanceIndex = m_distance.GetUAVDescriptorIndex();
    // F4.4.2b diagnostics: finer steps scale both limits.
    const float stepScale = std::isfinite(request.stepScale) ? std::clamp(request.stepScale, 1.0f / 64.0f, 1.0f) : 1.0f;
    constants.maxSteps = static_cast<std::uint32_t>(std::ceil(static_cast<float>(quality.maxSteps) / stepScale));
    // About half a cell between samples.
    constants.minimumStep = 0.5f * field.cellSize * stepScale;
    constants.jitterEnabled = request.jitter ? 1u : 0u;
    constants.wideUpsample = request.wideUpsample ? 1u : 0u;

    // AIR.4F F4.1: the two detail phases at this transport time. Phase B runs half a period behind phase A; each restarts (a new cycle
    // offset, tau back to 0) exactly when its weight 1 - |2 * phase - 1| is 0.
    const bool detail = request.detail && m_noise != nullptr && std::isfinite(request.detailStrength) && request.detailStrength > 0.0f &&
        std::isfinite(request.detailScale) && request.detailScale > 0.0f && std::isfinite(mistSeconds);

    if(detail)
    {
        const AirMistDetailPhases phases = DetailPhasesAt(mistSeconds);

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            constants.phaseOffsetA[axis] = phases.offsetA[axis];
            constants.phaseOffsetB[axis] = phases.offsetB[axis];
        }

        constants.phaseTimeA = phases.timeA;
        constants.phaseTimeB = phases.timeB;
        constants.phaseWeightA = phases.weightA;
        constants.detailStrength = (std::min)(request.detailStrength, 1.0f);
        constants.inverseDetailScale = 1.0f / request.detailScale;
        constants.noiseIndex = m_noiseDescriptor;
        constants.noiseMeanInverse = m_noiseMeanInverse;
        constants.detailEnabled = 1;

        // AIR.4F F4.4: the curl warp of the detail's lookup; a strength of 0 leaves the F4.1 detail exactly.
        const bool curl = request.curl && m_curl != nullptr && std::isfinite(request.curlStrength) && request.curlStrength > 0.0f &&
            std::isfinite(request.curlScale) && request.curlScale > 0.0f && std::isfinite(request.curlFlow);

        if(curl)
        {
            constants.curlDisplacement = (std::min)(request.curlStrength, 4.0f) * m_curlRmsInverse;
            constants.curlInverseScale = 1.0f / request.curlScale;
            constants.curlFlow = std::clamp(request.curlFlow, 0.0f, 1.0f);
            constants.curlIndex = m_curlDescriptor;
            constants.curlEnabled = 1;
        }

        // AIR.4F F4.4.2: wisps, a second factor from the filament mask; a strength of 0 leaves the detail exactly as without them.
        const bool wisps = request.wisps && m_filaments.resource != nullptr && std::isfinite(request.wispStrength) && request.wispStrength > 0.0f &&
            std::isfinite(request.wispScale) && request.wispScale > 0.0f;

        if(wisps)
        {
            constants.wispStrength = (std::min)(request.wispStrength, 1.0f);
            constants.wispInverseScale = 1.0f / request.wispScale;
            constants.wispMeanInverse = m_filaments.meanInverse;
            constants.wispSquareMeanInverse = m_filaments.squareMeanInverse;
            constants.wispIndex = m_filaments.descriptor;
            constants.wispEnabled = 1;
        }

        // AIR.4F F4.4.2e: density-dependent detail; a strength of 0 leaves the detail exactly as without it (the shader skips it).
        if(request.densityDetail && std::isfinite(request.densityDetailStrength) && request.densityDetailStrength > 0.0f)
        {
            constants.densityDetailStrength = (std::min)(request.densityDetailStrength, 1.0f);
        }
    }

    // AIR.4F F4.3: single scattering; the sun is shadowed only with a TLAS to trace.
    const bool shadowed = lit && request.sunShadows && lighting->topLevel != 0;
    m_sunShadowed = shadowed;
    m_visibilityRayBound = 0;

    if(lit)
    {
        const auto finiteOr = [](float value, float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        };
        constants.lightingEnabled = 1;
        constants.visibilityIndex = shadowed ? m_visibility.GetUAVDescriptorIndex() : UINT32_MAX;
        constants.scattering = std::clamp(finiteOr(request.scattering, 0.0f), 0.0f, 1.0f);
        constants.anisotropy = std::clamp(finiteOr(request.anisotropy, 0.0f), -0.9f, 0.9f);
        constants.ambient = std::clamp(finiteOr(request.ambient, 0.0f), 0.0f, 1.0f);
        constants.irradianceIndex = lighting->environmentIrradiance;
        const Vector3 direction = lighting->sunDirection.Normalized();
        const float sun[3] = {lighting->sunRadiance.x, lighting->sunRadiance.y, lighting->sunRadiance.z};
        const float directions[3] = {direction.x, direction.y, direction.z};

        for(std::size_t axis = 0; axis < 3; ++axis)
        {
            constants.sunDirection[axis] = std::isfinite(directions[axis]) ? directions[axis] : 0.0f;
            constants.sunRadiance[axis] = (std::max)(finiteOr(sun[axis], 0.0f), 0.0f);
        }
    }
    const D3D12_GPU_VIRTUAL_ADDRESS writeAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    // AIR.4F F4.3: the raymarch writes its results through their UAVs and reads the visibility through its SRV.
    constants.visibilityIndex = shadowed ? m_visibility.GetDescriptorIndex() : UINT32_MAX;
    const D3D12_GPU_VIRTUAL_ADDRESS marchAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    // The composite reads the reduced buffers through their read-only views.
    constants.resultIndex = m_result.GetDescriptorIndex();
    constants.distanceIndex = m_distance.GetDescriptorIndex();
    const D3D12_GPU_VIRTUAL_ADDRESS readAddress = upload.Allocate(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);

    if(writeAddress == 0 || marchAddress == 0 || readAddress == 0)
    {
        return false;
    }

    const auto stamp = [&](UINT index)
    {
        if(timestamps != nullptr)
        {
            commandList->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + index);
        }
    };

    ID3D12DescriptorHeap* heaps[]{descriptorHeap.GetHeap()};
    commandList->SetDescriptorHeaps(1, heaps);
    const D3D12_RESOURCE_STATES previousDepthState = viewportDepth.GetState();
    viewportDepth.Transition(commandList, DepthReadState);

    for(Buffer* buffer : StorageBuffers())
    {
        buffer->Transition(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    stamp(0);

    // 1. Occupancy, one group per brick.
    m_occupancyPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, writeAddress);
    commandList->Dispatch(brickCounts[0], brickCounts[1], brickCounts[2]);
    UnorderedAccessBarrier(commandList);

    // AIR.4F F4.3: 1b. The sun's visibility, one group per brick (the bricks near mist trace), read by the raymarch.
    if(shadowed)
    {
        m_visibilityPipeline.Bind(commandList);
        commandList->SetComputeRootConstantBufferView(0, writeAddress);
        commandList->SetComputeRootShaderResourceView(1, lighting->topLevel);
        commandList->Dispatch(brickCounts[0], brickCounts[1], brickCounts[2]);
        m_visibility.Transition(commandList, ShaderReadState);
        m_visibilityRayBound = std::uint64_t{bricks} * BrickCells * BrickCells * BrickCells * 2;
    }

    stamp(1);

    // 2. The raymarch, one thread per reduced pixel.
    m_raymarchPipeline.Bind(commandList);
    commandList->SetComputeRootConstantBufferView(0, marchAddress);
    // Unlit, the table is never read; the constants stand in for it so every root parameter is set.
    commandList->SetComputeRootConstantBufferView(1, lit ? lighting->lightTable : writeAddress);
    commandList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    stamp(2);

    // 3. The composite, reading the reduced results.
    m_result.Transition(commandList, ShaderReadState);
    m_distance.Transition(commandList, ShaderReadState);
    viewportColor.Transition(commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE colorTarget = viewportColor.GetRTV();
    commandList->OMSetRenderTargets(1, &colorTarget, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(fullWidth), static_cast<float>(fullHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(fullWidth), static_cast<LONG>(fullHeight)};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    (lit ? m_hdrCompositePipeline : m_compositePipeline).Bind(commandList);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(0, readAddress);
    commandList->DrawInstanced(3, 1, 0, 0);
    stamp(3);

    viewportDepth.Transition(commandList, previousDepthState);
    ++m_executions;
    return true;
}

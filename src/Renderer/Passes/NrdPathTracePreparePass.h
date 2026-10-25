#pragma once

#include "Math/Matrix4.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/Texture.h"

#include <cstdint>

class DescriptorHeap;
class GraphicsDevice;
class ShaderLibrary;
class UploadBuffer;

// Must match NrdPathTracePrepare.hlsl.
struct NrdPathTracePrepareConstants
{
    Matrix4 inverseViewProjection;
    Matrix4 view;

    float cameraPosition[3]{};
    float denoisingRange = 0.0f;

    std::uint32_t resolution[2]{};
    std::uint32_t depthIndex = 0;
    std::uint32_t materialIndex = 0;

    std::uint32_t surfaceIndex = 0;
    std::uint32_t geometricNormalIndex = 0;
    std::uint32_t materialBufferIndex = 0;
    std::uint32_t rawDiffuseIndex = 0;

    std::uint32_t rawSpecularIndex = 0;
    std::uint32_t normalRoughnessIndex = 0;
    std::uint32_t viewZIndex = 0;
    std::uint32_t diffuseIndex = 0;

    std::uint32_t specularIndex = 0;
    std::uint32_t reconstructedDiffuseIndex = 0;
    std::uint32_t reconstructedSpecularIndex = 0;
    // nrd::ReblurHitDistanceParameters, which a REBLUR instance consuming these inputs must be given unchanged.
    float hitDistanceA = 3.0f;

    float hitDistanceB = 0.1f;
    float hitDistanceC = 20.0f;
    // NRD.4A: demodulated radiance is packed as L / (factor * radianceScale) and unpacked by the same product. A fixed
    // power of two (exact in both directions); at 64 or more no input the extraction can produce reaches fp16's maximum.
    float radianceScale = 64.0f;
    // NRD.4B: the G-buffer motion SRV (the caller's); ExecutePrepare copies it to GetMotion() for NRD's IN_MV.
    std::uint32_t motionIndex = 0;

    // NRD.4B production resolve (ExecuteDenoisedResolve fills these in).
    std::uint32_t denoisedDiffuseIndex = 0;
    std::uint32_t denoisedSpecularIndex = 0;
    std::uint32_t presentationIndex = 0;
    std::uint32_t motionOutputIndex = 0;

    // NRD.4C diagnostics: the denoised resolve's components (0 both, 1 diffuse only, 2 specular only).
    std::uint32_t resolveComponents = 0;
    // NRD.4D diagnostics: CSPrepare's specular hit-distance alpha (0 production; 1 every surface pixel 1; 2 every
    // surface pixel 0.1; 3 production with "no data" (0) replaced by 1). Radiance is unchanged in every mode.
    std::uint32_t specularHitDistanceMode = 0;
    std::uint32_t padding[2]{};
};

static_assert(sizeof(NrdPathTracePrepareConstants) == 2 * 64 + 8 * 16);

// NRD.3 path-traced NRD signal preparation and bypass reconstruction (diagnostics only; no NRD denoiser runs;
// NrdPathTracePrepare.hlsl has the contracts). Prepare packs this frame's raw extraction outputs and the G-buffer into
// REBLUR's normal/roughness, view Z and demodulated diffuse and specular inputs. Resolve unpacks those inputs as if NRD
// had returned them unchanged, remodulates them and writes the reconstructed raw signals, so a comparison with the
// extraction proves the round trip.
//
// Owns five NRD-format textures (R10G10B10A2, R32F, 3 x RGBA16F; the motion copy since NRD.4B) and, for the bypass diagnostic only, two RGBA32F
// reconstructions at the scene extent, created on first use and kept until released. Nothing here holds history: every
// frame rewrites every texel. NRD.4B: ExecuteDenoisedResolve writes REBLUR's remodulated outputs into the presentation.
class NrdPathTracePreparePass
{
    public:
    // The NRD inputs, and the bypass diagnostic's reconstructions on top.
    static constexpr std::uint64_t InputBytesPerPixel = 4 + 4 + 3 * 8;
    static constexpr std::uint64_t ReconstructionBytesPerPixel = 2 * 16;
    static constexpr std::uint64_t BytesPerPixel = InputBytesPerPixel + ReconstructionBytesPerPixel;

    // The resolution-independent part: the three compute pipelines. Created on first use; a failure is sticky.
    bool Initialize(GraphicsDevice& graphicsDevice, ShaderLibrary& shaderLibrary);

    // Creates the textures at the given extent when they are missing, of another extent, or (reconstructions) without the
    // bypass diagnostic's reconstructions it now needs. Creation only: no GPU work is recorded.
    bool EnsureTextures(ID3D12Device* device, DescriptorHeap& descriptorHeap, unsigned int width, unsigned int height,
        bool reconstructions);

    // Releases the textures. Caller guarantees the queue is idle.
    void ReleaseTextures();
    void Shutdown();

    // Records the preparation. constants carries the frame's camera, the G-buffer, material buffer and extraction
    // indices; the output indices and resolution are filled in here. The inputs must already be shader-readable;
    // afterwards the NRD-format textures are too.
    void ExecutePrepare(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        NrdPathTracePrepareConstants constants,
        unsigned int width,
        unsigned int height);

    // Records the bypass reconstruction from the textures ExecutePrepare wrote, with the same constants. Afterwards
    // the reconstructions are shader-readable.
    void ExecuteResolve(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        NrdPathTracePrepareConstants constants,
        unsigned int width,
        unsigned int height);

    // NRD.4B: remodulates REBLUR's outputs (unordered-access state after NRD) into presentation's surface pixels, with
    // the constants of this frame's ExecutePrepare. Afterwards the outputs are shader-readable and presentation is in
    // unordered-access state.
    void ExecuteDenoisedResolve(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        NrdPathTracePrepareConstants constants,
        Texture& denoisedDiffuse,
        Texture& denoisedSpecular,
        Texture& presentation,
        unsigned int width,
        unsigned int height);

    bool HasTextures() const
    {
        return m_normalRoughness.GetResource() != nullptr;
    }

    Texture& GetNormalRoughness()
    {
        return m_normalRoughness;
    }

    Texture& GetViewZ()
    {
        return m_viewZ;
    }

    // Packed REBLUR inputs: YCoCg demodulated radiance, normalized hit distance in alpha.
    Texture& GetDiffuse()
    {
        return m_diffuse;
    }

    Texture& GetSpecular()
    {
        return m_specular;
    }

    // IN_MV: the G-buffer motion, unchanged, in a texture NRD can bind.
    Texture& GetMotion()
    {
        return m_motion;
    }

    // The bypass reconstruction in the extraction's layout: remodulated radiance, hit distance in alpha.
    Texture& GetReconstructedDiffuse()
    {
        return m_reconstructedDiffuse;
    }

    Texture& GetReconstructedSpecular()
    {
        return m_reconstructedSpecular;
    }

    std::uint64_t GetResourceBytes() const;

    // Texture creations since startup (diagnostics: steady-state frames must not add any).
    std::uint64_t GetTextureCreations() const
    {
        return m_textureCreations;
    }

    private:
    // Fills in the resolution and the UAVs each entry point writes, uploads the constants and dispatches one thread per
    // pixel. The caller sets diffuseIndex and specularIndex (UAVs to write, SRVs to read).
    void Dispatch(
        ID3D12GraphicsCommandList* commandList,
        UploadBuffer& uploadBuffer,
        ComputePipeline& pipeline,
        NrdPathTracePrepareConstants& constants,
        unsigned int width,
        unsigned int height);

    ComputePipeline m_preparePipeline;
    ComputePipeline m_resolvePipeline;
    ComputePipeline m_denoisedResolvePipeline;
    bool m_initialized = false;
    bool m_initializationFailed = false;

    Texture m_normalRoughness;
    Texture m_viewZ;
    Texture m_diffuse;
    Texture m_specular;
    Texture m_motion;
    Texture m_reconstructedDiffuse;
    Texture m_reconstructedSpecular;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
    std::uint64_t m_textureCreations = 0;
};

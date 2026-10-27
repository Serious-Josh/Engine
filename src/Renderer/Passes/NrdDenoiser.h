#pragma once

#include "Math/Matrix4.h"
#include "Renderer/Texture.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d12.h>

#include <NRD.h>
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIWrapperD3D12.h>

#include <NRDIntegration.h>

class DescriptorHeap;

// One NRD instance: an nrd::Integration (the SDK's NRI-based D3D12 layer) holding the denoisers its owner configured,
// plus the RGBA16F output textures those denoisers write. Every user (the hybrid renderer's reflection and GI
// denoisers, future path-traced modes) owns its own instance and its own configuration; instances share nothing.
//
// Lifetime: Initialize creates the instance at a fixed extent, Resize recreates it (NRD has no resize), Shutdown
// releases it. Shutdown and recreation wait for submitted GPU work inside the SDK before releasing outputs. The
// renderer must discard any unsubmitted commands referencing these resources and keep the device, queue and output
// descriptor heap alive through Shutdown (or destruction). Settings are per frame: NewFrame, SetCommonSettings, then
// SetDenoiserSettings for each denoiser that will run, then Denoise.
class NrdDenoiser
{
    public:
    NrdDenoiser() = default;
    ~NrdDenoiser();

    NrdDenoiser(const NrdDenoiser&) = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    // A denoiser of the instance. Its outputs follow from its type: a diffuse output for the diffuse denoisers, a
    // specular output for the specular ones, both for the combined ones.
    struct DenoiserDescription
    {
        nrd::Identifier identifier = 0;
        nrd::Denoiser denoiser = nrd::Denoiser::REBLUR_DIFFUSE;
    };

    // Enough for Hybrid's two combined denoisers in one instance.
    static constexpr size_t MaximumDenoisers = 2;

    struct Description
    {
        // The integration's name (NRD resource and debug names).
        std::string name;
        std::vector<DenoiserDescription> denoisers;
        // Frames the CPU may record ahead of the GPU (CommandContext::FrameCount).
        std::uint32_t queuedFrameCount = 0;
        // NRD.4C diagnostics: IntegrationCreationDesc::promoteFloat16to32 (fp32 internal textures; the SDK calls it
        // "overkill, kills performance"). False for every production instance.
        bool promoteFloat16to32 = false;
        // ER.1 Phase 3.1: IntegrationCreationDesc::autoWaitForIdle = false. NRD then never waits for the GPU itself;
        // every Shutdown, Resize and recreation requires that no submitted work can still reference the instance (a
        // drained queue, or a completed last-use fence). False keeps NRD's own idle wait.
        bool externallySynchronized = false;
    };

    // Engine's per-frame camera and history state, converted into nrd::CommonSettings by SetCommonSettings.
    struct FrameSettings
    {
        // Unjittered, as NRD requires.
        Matrix4 view;
        Matrix4 projection;
        Matrix4 previousView;
        Matrix4 previousProjection;

        // Sub-pixel offsets of this and the previous frame's samples, in pixels ([-0.5, 0.5]).
        float jitter[2] = {0.0f, 0.0f};
        float previousJitter[2] = {0.0f, 0.0f};

        // IN_MV is multiplied by this; z = 0 means 2D motion, otherwise 2.5D (viewZ delta).
        float motionVectorScale[3] = {1.0f, 1.0f, 1.0f};

        std::uint32_t frameIndex = 0;
        // Pixels with view Z at or beyond this are not denoised.
        float denoisingRange = 0.0f;
        nrd::AccumulationMode accumulationMode = nrd::AccumulationMode::CONTINUE;
        // NRD.4B: NRD's debug overlay into Inputs::validation (CommonSettings::enableValidation).
        bool enableValidation = false;
        // R.9A diagnostics: CommonSettings::isHistoryConfidenceAvailable. When set, Inputs::diffuseConfidence and (for a
        // denoiser with a specular signal) Inputs::specularConfidence must be provided.
        bool historyConfidence = false;
    };

    // One Denoise call's inputs. Guides are always required; each radiance input only when the denoiser has that signal.
    // Formats are fixed: motion RGBA16F, normal/roughness R10G10B10A2 (NRD_NORMAL_ENCODING 2), view Z R32F, radiance
    // and hit distance RGBA16F. The inputs must be in a shader-readable state; NRD returns them to it. Every input
    // needs D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS: the integration creates unordered-access views of them, so a
    // G-buffer render target must be copied first (NRD.4B).
    struct Inputs
    {
        ID3D12Resource* motion = nullptr;
        Texture* normalRoughness = nullptr;
        Texture* viewZ = nullptr;
        Texture* diffuseRadianceHitDistance = nullptr;
        Texture* specularRadianceHitDistance = nullptr;
        // Optional OUT_VALIDATION target (RGBA16F, unordered-access capable), written when enableValidation is set.
        Texture* validation = nullptr;
        // R.9A diagnostics: IN_DIFF_CONFIDENCE and IN_SPEC_CONFIDENCE (R16F, any extent: NRD samples them bilinearly by
        // previous-frame UV), bound only while FrameSettings::historyConfidence is set.
        ID3D12Resource* diffuseConfidence = nullptr;
        ID3D12Resource* specularConfidence = nullptr;
    };

    // A live owner must be shut down before Initialize is called again. Invalid requests leave it intact.
    bool Initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* graphicsQueue,
        DescriptorHeap& descriptorHeap,
        uint32_t width,
        uint32_t height,
        const Description& description);
    bool Resize(
        DescriptorHeap& descriptorHeap,
        uint32_t width,
        uint32_t height);
    void Shutdown();

    bool IsInitialized() const
    {
        return m_width != 0;
    }

    bool IsExternallySynchronized() const
    {
        return m_description.externallySynchronized;
    }

    void NewFrame();
    bool SetCommonSettings(const FrameSettings& settings);
    bool SetDenoiserSettings(nrd::Identifier identifier, const nrd::ReblurSettings& settings);
    bool SetDenoiserSettings(nrd::Identifier identifier, const nrd::RelaxSettings& settings);

    // Records one denoiser. Its outputs are left in unordered-access state. NRD.RELAX.3: false when nothing was recorded
    // (unknown identifier, missing command list or input); the caller must then not treat the frame as denoised.
    bool Denoise(
        ID3D12GraphicsCommandList* commandList,
        nrd::Identifier identifier,
        const Inputs& inputs);

    // The denoiser's outputs; an empty texture when the denoiser has no such signal or the identifier is unknown.
    Texture& GetDiffuseOutput(nrd::Identifier identifier);
    Texture& GetSpecularOutput(nrd::Identifier identifier);

    double GetTotalMemoryUsageInMb() const;
    double GetPersistentMemoryUsageInMb() const;
    double GetAliasableMemoryUsageInMb() const;

    private:
    struct Denoiser
    {
        DenoiserDescription description;
        Texture diffuseOutput;
        Texture specularOutput;
    };

    bool Recreate(
        DescriptorHeap& descriptorHeap,
        uint32_t width,
        uint32_t height);
    Denoiser* FindDenoiser(nrd::Identifier identifier);
    void ReleaseOutputs();

    nrd::Integration m_integration;
    // R.9A: the last SetCommonSettings' FrameSettings::historyConfidence, which Denoise's confidence bindings follow.
    bool m_historyConfidence = false;
    Description m_description;
    std::array<Denoiser, MaximumDenoisers> m_denoisers;
    // Returned for an identifier the instance does not have; never initialized.
    Texture m_emptyOutput;

    ID3D12Device* m_device = nullptr;
    ID3D12CommandQueue* m_graphicsQueue = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

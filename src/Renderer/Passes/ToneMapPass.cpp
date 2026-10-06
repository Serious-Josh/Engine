#include "ToneMapPass.h"

#include "Core/Log.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Texture.h"
#include "Renderer/Shader/ShaderLibrary.h"

bool ToneMapPass::Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary)
{
    ShaderDescription vertexDescription{};
    vertexDescription.path = "ToneMap.hlsl";
    vertexDescription.entryPoint = "VSMain";
    vertexDescription.stage = ShaderStage::Vertex;

    Shader* vertexShader = shaderLibrary.Load(vertexDescription);

    if(!vertexShader){
        Log::Error("Failed to load tone map vertex shader.");
        return false;
    }

    ShaderDescription pixelDescription{};
    pixelDescription.path = "ToneMap.hlsl";
    pixelDescription.entryPoint = "PSMain";
    pixelDescription.stage = ShaderStage::Pixel;

    Shader* pixelShader = shaderLibrary.Load(pixelDescription);

    if(!pixelShader){
        Log::Error("Failed to load tone map pixel shader.");
        return false;
    }

    PipelineDescription pipelineDescription{};
    pipelineDescription.vertexShader = vertexShader;
    pipelineDescription.pixelShader = pixelShader;
    pipelineDescription.vertexLayout = nullptr;
    pipelineDescription.renderTargets.count = 1;
    pipelineDescription.renderTargets.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    pipelineDescription.rootParameters.push_back(
        {RootParameterType::Constants, 0, 5, D3D12_SHADER_VISIBILITY_PIXEL}
    );

    if(!m_pipeline.Initialize(device, pipelineDescription)){
        Log::Error("Failed to initialize tone map pipeline.");
        return false;
    }

    return true;
}

void ToneMapPass::Execute(
    ID3D12GraphicsCommandList* commandList,
    RenderTarget& sceneColor,
    D3D12_CPU_DESCRIPTOR_HANDLE outputRTV,
    Texture* debugTexture,
    ToneMapDebugView debugView,
    float nrdDenoisingRange,
    RenderTarget* debugRenderTarget)
{
    sceneColor.Transition(
        commandList,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );

    if(debugTexture){
        debugTexture->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if(debugRenderTarget){
        debugRenderTarget->Transition(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    commandList->OMSetRenderTargets(
        1,
        &outputRTV,
        FALSE,
        nullptr
    );

    m_pipeline.Bind(commandList);

    struct ToneMapConstants
    {
        unsigned int sceneColorIndex;
        float exposure;
        unsigned int debugTextureIndex;
        unsigned int debugView;
        float nrdDenoisingRange;
    };

    ToneMapConstants constants{};
    constants.sceneColorIndex = sceneColor.GetSRVIndex();
    constants.exposure = 1.0f;
    constants.debugTextureIndex = debugRenderTarget ? debugRenderTarget->GetSRVIndex()
        : (debugTexture ? debugTexture->GetDescriptorIndex() : 0);
    constants.debugView = (debugTexture || debugRenderTarget) ? static_cast<unsigned int>(debugView) : 0;
    constants.nrdDenoisingRange = nrdDenoisingRange;

    commandList->SetGraphicsRoot32BitConstants(
        0,
        5,
        &constants,
        0
    );

    commandList->DrawInstanced(3, 1, 0, 0);
}

void ToneMapPass::Shutdown()
{
    m_pipeline.Shutdown();
}

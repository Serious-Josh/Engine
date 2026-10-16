#include "Renderer/EditorColliderOverlay.h"
#include "Editor/SceneViewportLayout.h"
#include "Renderer/Passes/EditorColliderPass.h"
#include "Renderer/Shader/ShaderCompiler.h"
#include "Scene/Scene.h"

#include <d3d12shader.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if(!condition)
    {
        throw std::runtime_error(message);
    }
}

Vector3 TransformPoint(const Matrix4& transform, Vector3 localPosition)
{
    return {
        transform[0][0] * localPosition.x + transform[0][1] * localPosition.y + transform[0][2] * localPosition.z + transform[0][3],
        transform[1][0] * localPosition.x + transform[1][1] * localPosition.y + transform[1][2] * localPosition.z + transform[1][3],
        transform[2][0] * localPosition.x + transform[2][1] * localPosition.y + transform[2][2] * localPosition.z + transform[2][3]};
}

bool PositionsMatch(Vector3 actualPosition, Vector3 expectedPosition)
{
    constexpr float positionTolerance = 0.0001f;
    return (actualPosition - expectedPosition).Length() < positionTolerance;
}

unsigned int BoxCornerIndex(Vector3 position)
{
    const unsigned int xSide = position.x > 0 ? 1u : 0u;
    const unsigned int ySide = position.y > 0 ? 2u : 0u;
    const unsigned int zSide = position.z > 0 ? 4u : 0u;
    return xSide | ySide | zSide;
}

void VerifyBoxEdges()
{
    // Corner IDs in the original implementation's draw order. Each pair is one edge.
    constexpr std::array<unsigned int, 24> expectedCorners{
        0, 1, 2, 3, 4, 5, 6, 7,
        0, 2, 4, 6, 1, 3, 5, 7,
        0, 4, 1, 5, 2, 6, 3, 7};
    std::set<unsigned int> uniqueEdges;
    for(unsigned int vertexIndex = 0; vertexIndex < EditorColliderOverlay::VerticesPerBox; vertexIndex += 2)
    {
        const Vector3 firstEndpoint = ColliderBoxVertex(vertexIndex);
        const Vector3 secondEndpoint = ColliderBoxVertex(vertexIndex + 1);
        Check(std::abs((firstEndpoint - secondEndpoint).Length() - 1.0f) < 0.00001f, "Invalid unit box edge");

        const unsigned int firstCorner = BoxCornerIndex(firstEndpoint);
        const unsigned int secondCorner = BoxCornerIndex(secondEndpoint);
        Check(firstCorner == expectedCorners[vertexIndex], "First endpoint order changed");
        Check(secondCorner == expectedCorners[vertexIndex + 1], "Second endpoint order changed");
        constexpr unsigned int cornersPerBox = 8;
        const unsigned int edgeIdentity = (std::min)(firstCorner, secondCorner) * cornersPerBox +
            (std::max)(firstCorner, secondCorner);
        Check(uniqueEdges.insert(edgeIdentity).second, "Duplicate box edge");
    }
    Check(uniqueEdges.size() == 12, "Box does not have exactly twelve edges");

    bool invalidVertexRejected = false;
    try
    {
        ColliderBoxVertex(EditorColliderOverlay::VerticesPerBox);
    }
    catch(const std::out_of_range&)
    {
        invalidVertexRejected = true;
    }
    Check(invalidVertexRejected, "Vertex bounds were not checked");
}

void VerifyViewportToolbarLayout()
{
    for(float uiScale : {1.0f, 1.5f, 2.0f})
    {
        const EditorWorkspaceRect viewport{ImVec2(100, 50), ImVec2(800, 600)};
        const float toolbarHeight = 24.0f * uiScale;
        const auto regions = CalculateSceneViewportRegions(viewport, toolbarHeight);
        Check(regions.imageArea.position.y == viewport.position.y + toolbarHeight, "Image did not begin below toolbar");
        Check(regions.imageArea.size.y + regions.toolbar.size.y == viewport.size.y, "Toolbar/image extent mismatch");
        const auto image = FitSceneViewportImage(regions.imageArea, regions.imageArea.size);
        Check(image.position.x == viewport.position.x && image.position.y == regions.imageArea.position.y,
            "Image-relative coordinate origin is incorrect");
        Check(image.size.x == regions.imageArea.size.x && image.size.y == regions.imageArea.size.y,
            "Image dimensions differ from requested render region");
        const auto retainedImage = FitSceneViewportImage(regions.imageArea, ImVec2(1920, 1080));
        Check(retainedImage.position.y >= regions.imageArea.position.y &&
            retainedImage.position.y + retainedImage.size.y <= viewport.position.y + viewport.size.y + 0.001f,
            "Retained texture letterbox overlapped toolbar or viewport edge");
    }
    const auto collapsed = CalculateSceneViewportRegions({ImVec2(0, 0), ImVec2(100, 10)}, 24);
    Check(collapsed.imageArea.size.y == 0 && collapsed.toolbar.size.y == 10, "Collapsed viewport produced negative image size");
}

void VerifyInvalidCoordinates()
{
    RenderObject object;
    object.transform.position = {1, 2, 3};
    object.transform.rotation = {0, 0, 1.57079632679f};
    object.transform.scale = {10, 0.5f, -10};
    Physics::PhysicsComponent component;
    component.bodyType = Physics::BodyType::Kinematic;
    component.boxDimensions = {10, 0.5f, 10};
    component.colliderLocalPositionOffset = {2, 0, 0};
    Matrix4 colliderWorldTransform;

    auto invalidComponent = component;
    invalidComponent.boxDimensions.width = std::numeric_limits<float>::infinity();
    Check(!BuildColliderBoxMatrix(invalidComponent, object, false, colliderWorldTransform), "Nonfinite dimension accepted");

    object.transform.rotation.x = std::numeric_limits<float>::quiet_NaN();
    Check(!BuildColliderBoxMatrix(component, object, false, colliderWorldTransform), "Nonfinite rotation accepted");

    object.transform.rotation = {};
    object.transform.position = {(std::numeric_limits<float>::max)(), 0, 0};
    component.colliderLocalPositionOffset.x = (std::numeric_limits<float>::max)();
    Check(!BuildColliderBoxMatrix(component, object, false, colliderWorldTransform), "Overflowing collider coordinates accepted");
}

void VerifyCaptureIdentityAndCapacity()
{
    Scene scene;
    scene.AddRenderObject({});
    constexpr std::uint64_t rendererLifetime = 5;
    constexpr std::uint64_t replacementRendererLifetime = 6;
    EditorColliderOverlay capturedOverlay;
    capturedOverlay.scene = &scene;
    capturedOverlay.structureRevision = scene.GetStructureRevision();
    capturedOverlay.rendererLifetime = rendererLifetime;
    capturedOverlay.colliderWorldTransforms.resize(EditorColliderOverlay::MaxBoxes);
    Check(capturedOverlay.Matches(scene, rendererLifetime), "Maximum collider count was rejected");
    Check(!capturedOverlay.Matches(scene, replacementRendererLifetime), "Stale renderer lifetime was accepted");

    capturedOverlay.colliderWorldTransforms.emplace_back();
    Check(!capturedOverlay.Matches(scene, rendererLifetime), "Overlay capacity bound ignored");

    capturedOverlay.colliderWorldTransforms.clear();
    Scene replacementScene;
    Check(!capturedOverlay.Matches(replacementScene, rendererLifetime), "Stale scene identity accepted");
    scene.AddRenderObject({});
    Check(!capturedOverlay.Matches(scene, rendererLifetime), "Stale scene structure accepted");
}

void VerifyReflectedShaderLayout(ShaderStage stage, ID3D12ShaderReflection& reflection)
{
    if(stage == ShaderStage::Vertex)
    {
        D3D12_SHADER_BUFFER_DESC constantBufferDescription{};
        auto* constantBuffer = reflection.GetConstantBufferByName("ColliderConstants");
        Check(SUCCEEDED(constantBuffer->GetDesc(&constantBufferDescription)), "Collider constant buffer reflection failed");
        Check(constantBufferDescription.Size == sizeof(Matrix4), "Collider root constant size mismatch");
        Check(constantBufferDescription.Size == EditorColliderPass::TransformConstantCount * sizeof(float),
            "Collider pass root constant count mismatch");
        Check(constantBufferDescription.Variables == 1, "Unexpected collider constant variables");

        D3D12_SHADER_TYPE_DESC matrixDescription{};
        auto* transformVariable = constantBuffer->GetVariableByName("clipFromBox");
        Check(SUCCEEDED(transformVariable->GetType()->GetDesc(&matrixDescription)), "Collider matrix reflection failed");
        Check(matrixDescription.Class == D3D_SVC_MATRIX_ROWS, "Collider matrix is not row-major");
        Check(matrixDescription.Rows == 4 && matrixDescription.Columns == 4, "Collider matrix dimensions changed");
        return;
    }

    D3D12_SHADER_DESC pixelShaderDescription{};
    Check(SUCCEEDED(reflection.GetDesc(&pixelShaderDescription)), "Collider pixel shader reflection failed");
    Check(pixelShaderDescription.OutputParameters == 1, "Unexpected collider pixel outputs");
}

void VerifyShaderCompilationAndReflection()
{
    ShaderCompiler compiler;
    Check(compiler.Initialize(), "DXC initialization failed");
    struct CompilerShutdown
    {
        ShaderCompiler& compiler;
        ~CompilerShutdown() { compiler.Shutdown(); }
    } compilerShutdown{compiler};

    const auto shaderPath = (std::filesystem::path(ENGINE_SOURCE_ROOT) / "assets/Shaders/EditorCollider.hlsl").wstring();
    for(ShaderStage stage : {ShaderStage::Vertex, ShaderStage::Pixel})
    {
        const wchar_t* entryPoint = stage == ShaderStage::Vertex ? L"VSMain" : L"PSMain";
        ShaderCompileResult compiledShader;
        Check(compiler.Compile({shaderPath.c_str(), entryPoint, stage}, compiledShader), "Collider shader compilation failed");

        using Microsoft::WRL::ComPtr;
        ComPtr<IDxcUtils> reflectionUtils;
        ComPtr<IDxcBlobEncoding> shaderBlob;
        ComPtr<IDxcContainerReflection> shaderContainer;
        ComPtr<ID3D12ShaderReflection> shaderReflection;
        Check(SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&reflectionUtils))), "DXC utils failed");
        Check(SUCCEEDED(reflectionUtils->CreateBlob(compiledShader.bytecode.data(),
            static_cast<UINT32>(compiledShader.bytecode.size()), DXC_CP_ACP, &shaderBlob)), "DXC blob failed");
        Check(SUCCEEDED(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&shaderContainer))),
            "DXIL reflection container creation failed");
        Check(SUCCEEDED(shaderContainer->Load(shaderBlob.Get())), "DXIL reflection load failed");

        constexpr UINT32 dxilPartKind = 'D' | ('X' << 8) | ('I' << 16) | ('L' << 24);
        UINT32 dxilPartIndex = 0;
        Check(SUCCEEDED(shaderContainer->FindFirstPartKind(dxilPartKind, &dxilPartIndex)), "DXIL reflection part missing");
        Check(SUCCEEDED(shaderContainer->GetPartReflection(dxilPartIndex, IID_PPV_ARGS(&shaderReflection))),
            "DXIL reflection failed");
        VerifyReflectedShaderLayout(stage, *shaderReflection.Get());
    }
}
}

class ColliderVisualizationVerification
{
public:
    static void Run()
    {
        VerifyViewportToolbarLayout();
        VerifyBoxEdges();
        VerifyAuthoredAndSimulatedPlacement();
        VerifyInvalidCoordinates();
        VerifyCaptureIdentityAndCapacity();
        VerifyShaderCompilationAndReflection();
    }

private:
    static void VerifyAuthoredAndSimulatedPlacement()
    {
        constexpr float quarterTurnRadians = 1.57079632679f;
        RenderObject object;
        object.transform.position = {1, 2, 3};
        object.transform.rotation = {0, 0, quarterTurnRadians};
        object.transform.scale = {10, 0.5f, -10};
        Physics::PhysicsComponent component;
        component.boxDimensions = {10, 0.5f, 10};
        component.colliderLocalPositionOffset = {2, 0, 0};

        Matrix4 authoredColliderTransform;
        for(Physics::BodyType bodyType : {Physics::BodyType::Static, Physics::BodyType::Dynamic, Physics::BodyType::Kinematic})
        {
            component.bodyType = bodyType;
            Check(BuildColliderBoxMatrix(component, object, false, authoredColliderTransform), "Authored collider rejected");
            Check(PositionsMatch(TransformPoint(authoredColliderTransform, {}), {1, 4, 3}),
                "Local offset does not rotate with body origin");
            Check(PositionsMatch(TransformPoint(authoredColliderTransform, {0.5f, 0, 0}), {1, 9, 3}),
                "Dimensions were multiplied by visual scale");
            Check(PositionsMatch(TransformPoint(authoredColliderTransform, {0, 0.5f, 0}), {0.75f, 4, 3}),
                "Authored rotation mismatch");
        }
        Check(!BuildColliderBoxMatrix(component, object, true, authoredColliderTransform),
            "Missing simulated pose used authored placement");

        Scene scene;
        scene.AddRenderObject(object);
        Physics::Pose simulatedPose;
        simulatedPose.position = {10, 20, 30};
        const float quarterTurnQuaternionComponent = std::sqrt(0.5f);
        simulatedPose.orientation = {0, 0, quarterTurnQuaternionComponent, quarterTurnQuaternionComponent};
        const std::optional<Physics::Pose> presentationPoses[] = {simulatedPose};
        scene.PreparePresentationCommit();
        scene.CommitPresentation(presentationPoses, true);

        Matrix4 simulatedColliderTransform;
        Check(BuildColliderBoxMatrix(component, scene.GetRenderObjects()[0], true, simulatedColliderTransform),
            "Simulated collider rejected");
        Check(PositionsMatch(TransformPoint(simulatedColliderTransform, {}), {10, 22, 30}), "Simulated offset mismatch");
        Check(PositionsMatch(TransformPoint(simulatedColliderTransform, {0.5f, 0, 0}), {10, 27, 30}), "Simulated rotation mismatch");

        scene.CommitPresentation({}, false);
        Check(BuildColliderBoxMatrix(component, scene.GetRenderObjects()[0], false, authoredColliderTransform),
            "Stopped collider rejected");
        Check(PositionsMatch(TransformPoint(authoredColliderTransform, {}), {1, 4, 3}), "Stop retained simulated placement");
    }
};

int VerifyColliderVisualization()
{
    try
    {
        ColliderVisualizationVerification::Run();
        std::cout << "PASS: twelve collider edges, dimensions/offsets, authored/simulated pose, unscaled geometry, Stop, finite bounds, capacity/scene identity and compiled/reflected shaders. CPU/DXC only; no GPU/editor initialization.\n";
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr << "FAIL: collider visualization: " << exception.what() << '\n';
        return 1;
    }
}

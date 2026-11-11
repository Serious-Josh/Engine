#pragma once

#include "Math/Matrix4.h"
#include "Math/Vector3.h"
#include "Renderer/AirField.h"
#include "Renderer/Buffer.h"
#include "Renderer/ComputePipeline.h"
#include "Renderer/Pipeline.h"
#include "Renderer/VoxelResidencyResources.h"

#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

class ShaderLibrary;
class DepthBuffer;
class RenderTarget;
class UploadBuffer;

// AIR.4D D3: where the vectors start (AirVisualization.hlsli).
enum class AirVectorPlacement : std::uint32_t
{
    // One vector per block at its world-stable hashed point, in whichever fluid piece holds it.
    HashedPoints = 0,
    // Two per block (blocks of at least one cell): each fluid piece of the cell holding the block's point, at its anchor.
    PieceAnchors = 1
};

// AIR.4A/AIR.4B: what the Editor's velocity-vector view draws this frame (EditorAir.hlsl, AirVisualization.hlsli).
struct AirVisualizationRequest
{
    // One block per world-aligned 2^spacingExponent cells on a side (-2 to 3: a quarter of a cell to 8 cells). Piece anchors use
    // blocks of at least one cell (finer spacings draw as one cell).
    int spacingExponent = 0;
    AirVectorPlacement placement = AirVectorPlacement::HashedPoints;
    // At most this many vectors: past it, only a box of blocks around `focus` (world metres, normally the camera) is drawn.
    std::uint32_t maximumVectors = 262'144;
    Vector3 focus{0.0f, 0.0f, 0.0f};
    // Metres of line per m/s, up to maximumLength metres.
    float lengthScale = 0.1f;
    float maximumLength = 1.5f;
    // The speed at which the colour reaches red.
    float colorMaximumSpeed = 10.0f;
    // Slower vectors are not drawn.
    float minimumSpeed = 0.05f;
    // AIR.4D D3: the simulation's ambient velocity, which samples near the window's edge blend for corners outside it.
    Vector3 ambientVelocity{0.0f, 0.0f, 0.0f};

    bool operator==(const AirVisualizationRequest&) const = default;
};

// AIR.4C: the Editor's air-cell diagnostic (EditorAirCells.hlsl): the obstacle mask's blocked and unknown cells, drawn at their
// exact bounds in a box of cells around `focus`.
struct AirCellRequest
{
    bool blocked = true;
    bool unknown = false;
    // AIR.4D: the fluid pieces' sub-cell boundary (EditorAirPieces.hlsl).
    bool pieces = false;
    // At most this many cells are examined: a box of whole cells around the focus (as for the vectors at one-cell spacing).
    std::uint32_t maximumCells = 262'144;
    // At most this many sub-cells for the pieces: a box of sub-cells around the focus.
    std::uint32_t maximumSubcells = 1'048'576;
    Vector3 focus{0.0f, 0.0f, 0.0f};

    bool operator==(const AirCellRequest&) const = default;
};

// Mirrors AirVectorParameters in AirVisualization.hlsli.
struct AirVectorParameters
{
    std::uint32_t blocks[3] = {0, 0, 0};
    std::uint32_t quarterCells = 4;
    std::int32_t firstBlock[3] = {0, 0, 0};
    float lengthScale = 0.0f;
    float maximumLength = 0.0f;
    float colorMaximumSpeed = 1.0f;
    float minimumSpeed = 0.0f;
    std::uint32_t placement = 0;
    float ambientVelocity[3] = {0.0f, 0.0f, 0.0f};
    // 1 for hashed points, 2 for piece anchors.
    std::uint32_t vectorsPerBlock = 1;
};

static_assert(sizeof(AirVectorParameters) == 64);

// The box of blocks drawn this frame: every block holding a cell of the window, or, past the request's budget, the largest box
// around the focus that fits it (block-aligned, so a vector never moves while the box follows the camera). Shared by the pass and
// its verification.
AirVectorParameters MakeAirVectorParameters(const AirFieldBinding& field, const AirVisualizationRequest& request);
std::uint32_t AirVectorCount(const AirVectorParameters& parameters);

// Short unlit lines along the air velocity, drawn after tone mapping into the final image, depth-tested against the presented depth
// without writing it. Reads the published velocity only: it never changes the simulation, and nothing it draws reaches a later
// frame.
class EditorAirPass
{
public:
    // viewProjection (16), AirFieldBinding (16), AirVectorParameters (16), the split list and arguments (4).
    static constexpr unsigned int ConstantCount = 52;
    // The largest vector budget a request may ask for.
    static constexpr std::uint32_t MaximumVectors = 4'194'304;

    bool Initialize(ID3D12Device* device, ShaderLibrary& shaderLibrary);
    // Releases the pass, its split list included; the caller has drained the GPU.
    void Shutdown();
    // AIR.4D D3: releases only the split list and its arguments (recreated on the next hashed-point draw); the caller has drained
    // the GPU.
    void ReleaseSplitResources();

    // Returns the number of vectors recorded (0 when the field is invalid or the targets do not match). AIR.4D D3: hashed points are
    // drawn in two passes (EditorAirVectors.hlsli): the lean pass for every instance, and an indirect split pass for the instances
    // CSClassify lists, whose list is created on first use.
    std::uint32_t Execute(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, RenderTarget& viewportColor,
        DepthBuffer& viewportDepth, const Matrix4& viewProjection, const AirFieldBinding& field, const AirVisualizationRequest& request);

    // AIR.4D D3: the split pass's list: one 4-byte instance index per vector the budget allows, and its draw arguments.
    static constexpr std::size_t SplitListBytes = std::size_t{MaximumVectors} * sizeof(std::uint32_t);

    // AIR.4C: translucent faces of the shown cells, depth-tested without writing depth. Returns the number of cells examined.
    std::uint32_t ExecuteCells(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, RenderTarget& viewportColor,
        DepthBuffer& viewportDepth, const Matrix4& viewProjection, const AirFieldBinding& field, const AirCellRequest& request) const;

    // viewProjection (16), AirFieldBinding (16), first cell and mask (4), cell counts (4).
    static constexpr unsigned int CellConstantCount = 40;

    // AIR.4D: solid sub-cells' faces bordering fluid, coloured by piece; `request.pieces` must be set. The constants (with the VX.5
    // binding, which the view queries) go through `upload`. Returns the number of sub-cells examined.
    std::uint32_t ExecutePieces(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, UploadBuffer& upload,
        RenderTarget& viewportColor, DepthBuffer& viewportDepth, const Matrix4& viewProjection, const AirFieldBinding& field,
        const VoxelWorldBinding& voxels, const AirCellRequest& request) const;

private:
    friend class AirFieldVerification;

    // AIR.4D D3: creates the split list, its arguments and their views on first use; false when that fails.
    bool EnsureSplitResources(DescriptorHeap& descriptorHeap);

    // Binds read-only depth, the bindless heap and `pipeline`; returns false when the targets differ in size.
    bool Begin(ID3D12GraphicsCommandList* commandList, DescriptorHeap& descriptorHeap, RenderTarget& viewportColor, DepthBuffer& viewportDepth,
        const Pipeline& pipeline, D3D12_RESOURCE_STATES& previousDepthState) const;
    void End(ID3D12GraphicsCommandList* commandList, RenderTarget& viewportColor, DepthBuffer& viewportDepth, D3D12_RESOURCE_STATES previousDepthState) const;

    Pipeline m_pipeline;
    Pipeline m_cellPipeline;
    Pipeline m_piecePipeline;

    // AIR.4D D3 split pass: its vertex shader, the list's reset and classification, the list and the indirect draw's arguments.
    ID3D12Device* m_device = nullptr;
    Pipeline m_splitPipeline;
    ComputePipeline m_resetSplitPipeline;
    ComputePipeline m_classifyPipeline;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_splitSignature;
    Buffer m_splitList;
    Buffer m_splitArguments;
    bool m_splitResourcesReady = false;
};

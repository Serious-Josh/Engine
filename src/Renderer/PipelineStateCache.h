#pragma once

#include <d3d12.h>

#include <string>

// Layered prep (F3): a persistent cache of compiled compute pipelines (D3D12 cached PSO blobs), so a pipeline whose DXIL and
// root signature were seen before on this adapter and driver skips the driver's compile. The NVIDIA driver's own cache is
// per executable (a Release, Debug or copied exe each compile again) and bounded; this one is shared by every executable
// on the machine. One file per pipeline under %LOCALAPPDATA%\Engine\ShaderCache\PSO\v1 (ENGINE_PSO_CACHE_DIR names another
// directory, "off" disables it), keyed by SHA-256 of the adapter (vendor, device, subsystem, revision), the user-mode
// driver version, the serialized root signature and the DXIL. A file is written atomically (AtomicFileWrite) and checked
// completely before use (format, key and the blob's own hash); an unreadable, corrupt or mismatched entry, or a blob the
// runtime or driver refuses (another driver), is a miss: the pipeline is created normally and its entry written again.
// Free-threaded: any thread may create through it at once (concurrent writers of one entry each publish a complete file).
namespace PipelineStateCache
{
    struct Outcome
    {
        // The pipeline came from a cached blob (no driver compile).
        bool hit = false;
        // A cached entry existed but was refused (corrupt, or another driver); the pipeline was then created normally.
        bool rejected = false;
        // A new entry was written.
        bool stored = false;
    };

    // CreateComputePipelineState through the cache. description.CachedPSO must be empty; rootSignature is the serialized
    // root signature description.pRootSignature was created from. Returns the creation's HRESULT (of the normal creation
    // when the cached one failed).
    HRESULT CreateComputePipelineState(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC& description,
        const void* rootSignature, std::size_t rootSignatureSize, ID3D12PipelineState** pipelineState,
        Outcome* outcome = nullptr);
}

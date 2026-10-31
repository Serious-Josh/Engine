#include "TopLevelAccelerationStructure.h"
#include "Core/Log.h"
#include "BottomLevelAccelerationStructure.h"
#include "Math/Matrix4.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    uint64_t AllocateTLASRecordingIdentity()
    {
        static std::atomic<uint64_t> next{1};
        return AllocateTLASIdentity(next);
    }
} // namespace

std::shared_ptr<TLASRecordingCapture> TopLevelAccelerationStructure::PrepareCapture(unsigned int slot,
    std::uint64_t recording, const std::vector<const BottomLevelAccelerationStructure*>& bottomLevels) noexcept
{
    if(m_instanceDescriptions.size() > MaxCaptureInstances)
    {
        m_captureStatus = TLASCaptureStatus::InstanceLimit;
        return {};
    }

    auto freeSlot = m_captureSlots.end();

    for(auto iterator = m_captureSlots.begin(); iterator != m_captureSlots.end(); ++iterator)
    {
        if(iterator->tracked.expired())
        {
            freeSlot = iterator;
            break;
        }
    }

    if(freeSlot == m_captureSlots.end())
    {
        m_captureStatus = TLASCaptureStatus::CapacityExceeded;
        return {};
    }

    try
    {
        auto capture = std::make_shared<TLASRecordingCapture>();
        capture->ownerLifetime = m_ownerLifetime;
        capture->recordingIdentity = recording;
        capture->uploadSlot = slot;
        capture->instances = m_instanceDescriptions;
        capture->bottomLevels.resize(m_instanceDescriptions.size());
        capture->result = m_resource;
        capture->scratch = m_scratchResource;
        capture->instanceUpload = m_instanceBuffers[slot];
        capture->completeBLASReferences = true;
        Microsoft::WRL::ComPtr<ID3D12Device> device;

        if(FAILED(m_resource->GetDevice(IID_PPV_ARGS(&device))))
        {
            m_captureStatus = TLASCaptureStatus::ResourceBudgetExceeded;
            return {};
        }

        const auto charge = [&](ID3D12Resource* resource)
        {
            const auto description = resource->GetDesc();
            const auto info = device->GetResourceAllocationInfo(0, 1, &description);

            if(info.SizeInBytes > MaxCaptureResourceBytes ||
                capture->resourceCharge > MaxCaptureResourceBytes - info.SizeInBytes)
            {
                return false;
            }

            capture->resourceCharge += info.SizeInBytes;
            return true;
        };

        if(!charge(m_resource) || !charge(m_scratchResource) || !charge(m_instanceBuffers[slot]))
        {
            m_captureStatus = TLASCaptureStatus::ResourceBudgetExceeded;
            return {};
        }

        for(std::size_t index = 0; index < capture->instances.size(); ++index)
        {
            auto& reference = capture->bottomLevels[index];

            if(index < m_instanceBuildIdentities.size())
            {
                reference.identity = m_instanceBuildIdentities[index];
            }

            const auto matches = [&](const BLASBuildCapture& build)
            {
                return reference.identity.ownerLifetime != 0 && reference.identity.buildID != 0 &&
                    build.identity.ownerLifetime == reference.identity.ownerLifetime &&
                    build.identity.buildID == reference.identity.buildID && build.result &&
                    build.result->GetGPUVirtualAddress() == capture->instances[index].AccelerationStructure;
            };

            for(const auto* bottomLevel : bottomLevels)
            {
                if(!bottomLevel)
                {
                    continue;
                }

                const auto& current = bottomLevel->GetCurrentBuildCapture();

                if(matches(current))
                {
                    reference.result = current.result;
                    break;
                }

                for(std::size_t retiredIndex = 0; const auto* retired = bottomLevel->GetRetiredBuildCapture(retiredIndex);
                    ++retiredIndex)
                {
                    if(matches(*retired))
                    {
                        reference.result = retired->result;
                        break;
                    }
                }

                if(reference.result)
                {
                    break;
                }
            }

            if(!reference.result)
            {
                // Preserve the native descriptor even when the corresponding
                // build is no longer discoverable. Never substitute a later VA match.
                capture->completeBLASReferences = false;
            }
            else if(!charge(reference.result.Get()))
            {
                m_captureStatus = TLASCaptureStatus::ResourceBudgetExceeded;
                return {};
            }
        }

        *freeSlot = {};
        freeSlot->tracked = capture;
        freeSlot->evidence.ownerLifetime = capture->ownerLifetime;
        freeSlot->evidence.recordingIdentity = capture->recordingIdentity;
        return capture;
    }
    catch(const std::bad_alloc&)
    {
        m_captureStatus = TLASCaptureStatus::AllocationFailed;
        return {};
    }
}

bool TopLevelAccelerationStructure::GetSubmissionEvidence(const TLASRecordingHandle& capture,
    TLASSubmissionEvidence& output) const noexcept
{
    if(!capture)
    {
        return false;
    }

    for(const auto& slot : m_captureSlots)
    {
        const auto tracked = slot.tracked.lock();

        if(tracked == capture && !tracked.owner_before(capture) && !capture.owner_before(tracked) &&
            slot.evidence.ownerLifetime != 0 && slot.evidence.ownerLifetime == capture->ownerLifetime &&
            slot.evidence.recordingIdentity == capture->recordingIdentity)
        {
            output = slot.evidence;
            return true;
        }
    }

    return false;
}

bool TopLevelAccelerationStructure::AttributeCurrentCapture(const TLASRecordingHandle& expected,
    TLASRecordingProvenance provenance) noexcept
{
    TLASSubmissionEvidence evidence;

    if(!expected || expected != m_capture || expected.owner_before(m_capture) || m_capture.owner_before(expected) ||
        expected->ownerLifetime != m_ownerLifetime || expected->recordingIdentity != m_recordingIdentity ||
        !provenance.recordingOwner || !provenance.generation || !GetSubmissionEvidence(expected, evidence) ||
        evidence.state != TLASRecordingState::Unattributed)
    {
        return false;
    }

    for(auto& slot : m_captureSlots)
    {
        if(slot.evidence.ownerLifetime == evidence.ownerLifetime &&
            slot.evidence.recordingIdentity == evidence.recordingIdentity)
        {
            slot.evidence.provenance = provenance;
            slot.evidence.state = TLASRecordingState::Recorded;
            slot.unresolved = expected;
            return true;
        }
    }

    return false;
}

void TopLevelAccelerationStructure::TransitionCaptures(TLASRecordingProvenance provenance,
    TLASRecordingState next) noexcept
{
    if(!provenance.recordingOwner || !provenance.generation)
    {
        return;
    }

    for(auto& slot : m_captureSlots)
    {
        auto& evidence = slot.evidence;
        const auto state = evidence.state;
        const bool legal = ((state == TLASRecordingState::Recorded || state == TLASRecordingState::NotSubmitted) &&
            (next == TLASRecordingState::SubmissionUncertain || next == TLASRecordingState::Discarded)) ||
            (state == TLASRecordingState::SubmissionUncertain && next == TLASRecordingState::NotSubmitted);

        if(evidence.provenance == provenance && legal)
        {
            evidence.state = next;

            if(next == TLASRecordingState::Discarded)
            {
                slot.unresolved.reset();
            }
        }
    }
}

void TopLevelAccelerationStructure::ConfirmCaptures(TLASRecordingProvenance provenance,
    std::uint64_t fence, std::uint64_t lifetime) noexcept
{
    if(!provenance.recordingOwner || !provenance.generation || !fence || fence == UINT64_MAX || !lifetime)
    {
        return;
    }

    for(auto& slot : m_captureSlots)
    {
        auto& evidence = slot.evidence;

        if(evidence.provenance == provenance && evidence.state == TLASRecordingState::SubmissionUncertain)
        {
            evidence.submissionFence = fence;
            evidence.submissionLifetime = lifetime;
            evidence.state = TLASRecordingState::Submitted;
        }
    }
}

void TopLevelAccelerationStructure::ObserveCaptureCompletion(std::uint64_t completed, std::uint64_t lifetime,
    std::uint64_t recordingOwner) noexcept
{
    if(!completed || completed == UINT64_MAX || !lifetime || !recordingOwner)
    {
        return;
    }

    for(auto& slot : m_captureSlots)
    {
        auto& evidence = slot.evidence;

        if(evidence.state == TLASRecordingState::Submitted && evidence.provenance.recordingOwner == recordingOwner &&
            evidence.provenance.generation != 0 && evidence.submissionFence != 0 && evidence.submissionFence != UINT64_MAX &&
            evidence.submissionLifetime == lifetime && completed >= evidence.submissionFence)
        {
            evidence.state = TLASRecordingState::Completed;
            slot.unresolved.reset();
        }
    }
}

bool TopLevelAccelerationStructure::Initialize(
    ID3D12Device5* device,
    ID3D12GraphicsCommandList4* commandList,
    const RayTracingTopLevelDescription& description,
    const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures)
{
    m_recordingIdentity = 0;
    m_recordingSlot = UINT_MAX;
    m_capture.reset();
    m_captureStatus = TLASCaptureStatus::Unavailable;

    if (!device || !commandList || description.instances.empty() || bottomLevelStructures.empty() ||
        m_resource || m_scratchResource || m_instanceBuffers[0] || m_instanceBuffers[1] ||
        description.instances.size() > UINT_MAX)
    {
        return false;
    }

    m_ownerLifetime = AllocateTLASRecordingIdentity();
    const auto recordingIdentity = AllocateTLASRecordingIdentity();

    if(!m_ownerLifetime || !recordingIdentity)
    {
        return false;
    }

    m_instanceBuildIdentities.clear();

    if(description.instances.size() <= MaxCaptureInstances)
    {
        try
        {
            m_instanceBuildIdentities.resize(description.instances.size());
        }
        catch(const std::bad_alloc&)
        {
            // Native evidence may still be captured without BLAS associations.
        }
    }

    m_instanceDescriptions.clear();

    m_instanceDescriptions.resize(description.instances.size());

    for (std::size_t i = 0; i < description.instances.size(); ++i)
    {
        const RayTracingInstanceDescription& sourceInstance = description.instances[i];

        if (sourceInstance.bottomLevelIndex >= bottomLevelStructures.size())
        {
            return false;
        }

        const BottomLevelAccelerationStructure* bottomLevel = bottomLevelStructures[sourceInstance.bottomLevelIndex];

        if (!bottomLevel || bottomLevel->GetGPUAddress() == 0)
        {
            return false;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& instance = m_instanceDescriptions[i];

        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
            {
                instance.Transform[row][column] = sourceInstance.transform[row][column];
            }
        }

        instance.InstanceID = sourceInstance.instanceID;

        instance.InstanceMask = sourceInstance.mask;

        instance.InstanceContributionToHitGroupIndex = 0;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

        instance.AccelerationStructure = bottomLevel->GetGPUAddress();

        if(i < m_instanceBuildIdentities.size())
        {
            m_instanceBuildIdentities[i] = bottomLevel->GetCurrentBuildCapture().identity;
        }
    }

    const UINT64 instanceBufferSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * m_instanceDescriptions.size();

    D3D12_HEAP_PROPERTIES uploadHeapProperties{};
    uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC instanceBufferDescription{};
    instanceBufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    instanceBufferDescription.Width = instanceBufferSize;
    instanceBufferDescription.Height = 1;

    instanceBufferDescription.DepthOrArraySize = 1;
    instanceBufferDescription.MipLevels = 1;
    instanceBufferDescription.Format = DXGI_FORMAT_UNKNOWN;
    instanceBufferDescription.SampleDesc.Count = 1;

    instanceBufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (unsigned int i = 0; i < FrameCount; ++i)
    {
        HRESULT result = device->CreateCommittedResource(&uploadHeapProperties, D3D12_HEAP_FLAG_NONE,
                                                         &instanceBufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                         nullptr, IID_PPV_ARGS(&m_instanceBuffers[i]));

        if (FAILED(result))
        {
            Log::Error("Failed to create initial TLAS instance buffer. HRESULT: " + std::to_string(result));
            Shutdown();
            return false;
        }
    }

    for (unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        void* mappedData = nullptr;

        HRESULT result = m_instanceBuffers[frameIndex]->Map(0, nullptr, &mappedData);

        if (FAILED(result))
        {
            Log::Error("Failed to map initial TLAS instance buffer. HRESULT: " + std::to_string(result));
            Shutdown();
            return false;
        }

        std::memcpy(mappedData, m_instanceDescriptions.data(), static_cast<std::size_t>(instanceBufferSize));

        m_instanceBuffers[frameIndex]->Unmap(0, nullptr);
    }

    for (unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        m_pendingInstanceUpdates[frameIndex].clear();

        m_pendingInstanceUpdates[frameIndex].resize(m_instanceDescriptions.size(), false);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs{};
    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildInputs.NumDescs = static_cast<UINT>(m_instanceDescriptions.size());
    buildInputs.InstanceDescs = m_instanceBuffers[0]->GetGPUVirtualAddress();
    buildInputs.Flags = description.buildPreference == RayTracingAccelerationStructureBuildPreference::FastBuild
                            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
                            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    buildInputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};

    device->GetRaytracingAccelerationStructurePrebuildInfo(&buildInputs, &prebuildInfo);

    if (prebuildInfo.ResultDataMaxSizeInBytes == 0 || prebuildInfo.ScratchDataSizeInBytes == 0 ||

        prebuildInfo.UpdateScratchDataSizeInBytes == 0)
    {
        Shutdown();
        return false;
    }

    D3D12_HEAP_PROPERTIES defaultHeapProperties{};
    defaultHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resourceDescription{};

    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDescription.Height = 1;
    resourceDescription.DepthOrArraySize = 1;
    resourceDescription.MipLevels = 1;

    resourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    resourceDescription.SampleDesc.Count = 1;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    resourceDescription.Width =
        (std::max)(prebuildInfo.ScratchDataSizeInBytes, prebuildInfo.UpdateScratchDataSizeInBytes);
    HRESULT result =
        device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_scratchResource));

    if (FAILED(result))
    {
        Log::Error("Failed to allocate TLAS scratch resource. HRESULT: " + std::to_string(result));
        Shutdown();
        return false;
    }

    resourceDescription.Width = prebuildInfo.ResultDataMaxSizeInBytes;

    result = device->CreateCommittedResource(&defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                             IID_PPV_ARGS(&m_resource));

    if (FAILED(result))
    {
        Log::Error("Failed to allocate TLAS result resource. HRESULT: " + std::to_string(result));
        Shutdown();
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDescription{};
    buildDescription.Inputs = buildInputs;
    buildDescription.ScratchAccelerationStructureData = m_scratchResource->GetGPUVirtualAddress();

    buildDescription.DestAccelerationStructureData = m_resource->GetGPUVirtualAddress();

    // All optional allocations/pins precede native recording. Both initialization
    // uploads were memcpy'd from this exact image and have not been changed.
    auto capture = PrepareCapture(0, recordingIdentity, bottomLevelStructures);

    if(capture)
    {
        capture->build = buildDescription;
    }

    commandList->BuildRaytracingAccelerationStructure(&buildDescription, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_resource;

    commandList->ResourceBarrier(1, &barrier);

    m_instanceCount = description.instances.size();
    m_buildFlags = buildInputs.Flags;
    m_recordingIdentity = recordingIdentity;
    m_recordingSlot = 0;
    m_capture = std::move(capture);

    if(m_capture)
    {
        m_captureStatus = TLASCaptureStatus::Ready;
    }

    return true;
}

bool TopLevelAccelerationStructure::Update(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const std::vector<RayTracingInstanceDescription>& instances,
    const std::vector<std::size_t>& changedInstanceIndices,
    const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures)
{
    m_recordingIdentity = 0;
    m_recordingSlot = UINT_MAX;
    m_capture.reset();
    m_captureStatus = TLASCaptureStatus::Unavailable;
    const auto recordingIdentity = AllocateTLASRecordingIdentity();

    if (recordingIdentity == 0)
    {
        return false;
    }

    if (frameIndex >= FrameCount ||
        !commandList ||
        !m_resource ||
        !m_scratchResource)
    {
        return false;
    }

    if (!m_instanceBuffers[frameIndex] ||
        instances.size() != m_instanceCount ||
        bottomLevelStructures.empty())
    {
        return false;
    }

    // Update the persistent native DXR descriptors for the instances whose scene transforms changed.

    for (std::size_t instanceIndex : changedInstanceIndices)
    {
        if (instanceIndex >= instances.size() || instanceIndex >= m_instanceDescriptions.size())
        {
            return false;
        }

        const RayTracingInstanceDescription& sourceInstance = instances[instanceIndex];

        if (sourceInstance.bottomLevelIndex >= bottomLevelStructures.size())
        {
            return false;
        }

        const BottomLevelAccelerationStructure* bottomLevel = bottomLevelStructures[sourceInstance.bottomLevelIndex];

        if (!bottomLevel || bottomLevel->GetGPUAddress() == 0)
        {
            return false;
        }

        D3D12_RAYTRACING_INSTANCE_DESC& instance = m_instanceDescriptions[instanceIndex];

        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
            {
                instance.Transform[row][column] = sourceInstance.transform[row][column];
            }
        }

        instance.InstanceID = sourceInstance.instanceID;

        instance.InstanceMask = sourceInstance.mask;
        instance.InstanceContributionToHitGroupIndex = 0;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

        instance.AccelerationStructure = bottomLevel->GetGPUAddress();

        if(instanceIndex < m_instanceBuildIdentities.size())
        {
            m_instanceBuildIdentities[instanceIndex] = bottomLevel->GetCurrentBuildCapture().identity;
        }

        for (unsigned int pendingFrame = 0; pendingFrame < FrameCount; ++pendingFrame)
        {
            if (!m_pendingInstanceUpdates[pendingFrame][instanceIndex])
            {
                m_pendingInstanceUpdates[pendingFrame][instanceIndex] = true;

                m_pendingInstanceIndices[pendingFrame].push_back(instanceIndex);
            }
        }
    }

    auto capture = PrepareCapture(frameIndex, recordingIdentity, bottomLevelStructures);
    void* mappedData = nullptr;

    HRESULT result = m_instanceBuffers[frameIndex]->Map(0, nullptr, &mappedData);

    if (FAILED(result))
    {
        Log::Error("Failed to map TLAS instance buffer for update. HRESULT: " + std::to_string(result));
        return false;
    }

    auto* mappedInstances = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(mappedData);

    for (std::size_t instanceIndex : m_pendingInstanceIndices[frameIndex])
    {
        mappedInstances[instanceIndex] = m_instanceDescriptions[instanceIndex];

        m_pendingInstanceUpdates[frameIndex][instanceIndex] = false;
    }

    m_pendingInstanceIndices[frameIndex].clear();

    if(capture)
    {
        // Copy the COMPLETE actual flushed image, including untouched entries
        // and delayed writes carried from the other upload slot.
        std::memcpy(capture->instances.data(), mappedInstances,
            capture->instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    }

    m_instanceBuffers[frameIndex]->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS buildInputs{};

    buildInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    buildInputs.NumDescs = static_cast<UINT>(m_instanceDescriptions.size());
    buildInputs.InstanceDescs = m_instanceBuffers[frameIndex]->GetGPUVirtualAddress();

    buildInputs.Flags = m_buildFlags | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDescription{};

    buildDescription.Inputs = buildInputs;
    buildDescription.SourceAccelerationStructureData = m_resource->GetGPUVirtualAddress();
    buildDescription.DestAccelerationStructureData = m_resource->GetGPUVirtualAddress();
    buildDescription.ScratchAccelerationStructureData = m_scratchResource->GetGPUVirtualAddress();

    if(capture)
    {
        capture->build = buildDescription;
    }

    commandList->BuildRaytracingAccelerationStructure(&buildDescription, 0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_resource;

    commandList->ResourceBarrier(1, &barrier);

    m_recordingIdentity = recordingIdentity;
    m_recordingSlot = frameIndex;
    m_capture = std::move(capture);

    if(m_capture)
    {
        m_captureStatus = TLASCaptureStatus::Ready;
    }
    return true;
}

bool TopLevelAccelerationStructure::RecordedInstanceUsesBLAS(
    uint64_t recordingIdentity,
    size_t instanceIndex,
    D3D12_GPU_VIRTUAL_ADDRESS address) const
{
    // Successful Update flushed this slot before recording Build. Its pending
    // list must still be empty and the queried descriptor must remain current.

    if (recordingIdentity == 0 || recordingIdentity != m_recordingIdentity || m_recordingSlot >= FrameCount)
    {
        return false;
    }

    if (!m_resource || !m_instanceBuffers[m_recordingSlot] || address == 0)
    {
        return false;
    }

    if (instanceIndex >= m_instanceDescriptions.size() ||

        instanceIndex >= m_pendingInstanceUpdates[m_recordingSlot].size())
    {
        return false;
    }

    if (!m_pendingInstanceIndices[m_recordingSlot].empty() || m_pendingInstanceUpdates[m_recordingSlot][instanceIndex])
    {
        return false;
    }

    return m_instanceDescriptions[instanceIndex].AccelerationStructure == address;
}

bool TopLevelAccelerationStructure::RefreshBLASReferences(
    ID3D12GraphicsCommandList4* commandList,
    unsigned int frameIndex,
    const std::vector<RayTracingInstanceDescription>& instances,
    const std::vector<const BottomLevelAccelerationStructure*>& bottomLevelStructures)
{
    m_recordingIdentity = 0;
    m_recordingSlot = UINT_MAX;
    m_capture.reset();
    m_captureStatus = TLASCaptureStatus::Unavailable;

    if (!commandList || frameIndex >= FrameCount || instances.size() != m_instanceCount)
    {
        return false;
    }

    std::vector<std::size_t> changedInstanceIndices;
    changedInstanceIndices.reserve(instances.size());

    for (std::size_t i = 0; i < instances.size(); ++i)
    {
        changedInstanceIndices.push_back(i);
    }

    return Update(commandList, frameIndex, instances, changedInstanceIndices, bottomLevelStructures);
}

void TopLevelAccelerationStructure::Shutdown()
{
    // Same checked drain/recording-abandonment precondition as the native owner.
    // This ends lifecycle tracking, never invents Completed for uncertain work.
    // Weak slots keep escaped historical captures charged across reinitialize.
    for(auto& slot : m_captureSlots)
    {
        slot.unresolved.reset();
        slot.evidence = {};
    }

    m_capture.reset();
    m_captureStatus = TLASCaptureStatus::Unavailable;
    m_ownerLifetime = 0;
    m_instanceBuildIdentities.clear();
    m_recordingIdentity = 0;
    m_recordingSlot = UINT_MAX;

    for (unsigned int i = 0; i < FrameCount; ++i)
    {
        if (m_instanceBuffers[i])
        {
            m_instanceBuffers[i]->Release();
            m_instanceBuffers[i] = nullptr;
        }
    }

    if (m_scratchResource)
    {
        m_scratchResource->Release();
        m_scratchResource = nullptr;
    }

    if (m_resource)
    {
        m_resource->Release();

        m_resource = nullptr;
    }

    m_instanceDescriptions.clear();

    for (unsigned int frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        m_pendingInstanceUpdates[frameIndex].clear();

        m_pendingInstanceIndices[frameIndex].clear();
    }

    m_instanceCount = 0;

    m_buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
}

D3D12_GPU_VIRTUAL_ADDRESS TopLevelAccelerationStructure::GetGPUAddress() const
{
    return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
}

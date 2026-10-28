#pragma once

// R.9I.3A Stage 2: pure, dormant CPU contracts. No Renderer, descriptors or GPU allocations.
#include "../../../assets/Shaders/PathTraceCausticNumeric.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace CausticContracts
{
    constexpr std::uint32_t MaximumProposals = 1;
    constexpr std::uint32_t MaximumCertifiedPrimitives = 4096;
    constexpr std::uint32_t FrameSlots = 2;
    constexpr std::uint64_t MaximumResidentBytes = 1ull << 30;
    constexpr std::uint32_t MaximumDimension = 4096;
    constexpr std::uint32_t MaximumProgressiveSamples = 1u << 24;

    struct MemoryPlan
    {
        std::uint64_t sumBytes = 0;
        std::uint64_t resolveBytes = 0;
        std::uint64_t slotBytes = 0;
        std::uint64_t residentBytes = 0;
    };

    inline bool PlanMemory(std::uint32_t width, std::uint32_t height, std::uint32_t attempts, MemoryPlan& output)
    {
        if(width == 0 || height == 0 || width > MaximumDimension || height > MaximumDimension ||
            attempts == 0 || attempts > CausticMaximumAttempts)
        {
            return false;
        }

        const std::uint64_t pixels = std::uint64_t(width) * height;
        MemoryPlan plan;
        plan.sumBytes = pixels * CausticSumBytesPerPixel;
        plan.resolveBytes = pixels * 16; // RGBA32F: no half-float energy loss in resolve.
        // Certificate header + records, a 512-byte aligned dispatch CB, and 64-byte status.
        plan.slotBytes = plan.sumBytes + plan.resolveBytes + MaximumCertifiedPrimitives * 16ull + 64 + 512 + 64;
        plan.residentBytes = plan.slotBytes * FrameSlots;

        // Logical bytes only. Device allocation-info/heap alignment and retained old epochs must
        // pass a second total-residency check before any future allocation is accepted.
        if(plan.residentBytes > MaximumResidentBytes || plan.sumBytes > UINT32_MAX)
        {
            return false;
        }

        output = plan;
        return true;
    }

    struct Preparation
    {
        CausticClassificationConfig classification{};
        CausticPublication current{};
        std::uint32_t lightCount = 0;
        std::uint32_t selectedLightType = CausticInvalidIdentity;
        std::uint32_t attempts = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool resourcesPrepared = false;
        bool certificatesCurrent = false;
        bool proposalPrepared = false;
        bool recordingAccepted = false;
    };

    inline bool CanPrepareDiagnostic(const Preparation& input)
    {
        MemoryPlan plan;
        const auto& config = input.classification;
        const bool validThresholds = CausticFinite(config.reflectorRoughnessThreshold) &&
            CausticFinite(config.receiverRoughnessThreshold) && config.reflectorRoughnessThreshold > 0.0f &&
            config.reflectorRoughnessThreshold <= 1.0f && config.receiverRoughnessThreshold > CausticDeltaRoughness &&
            config.receiverRoughnessThreshold <= 1.0f;
        const bool validRange = config.registrationMode != CausticPrimitiveRange ||
            (config.firstPrimitive != CausticInvalidIdentity && config.primitiveCount > 0u &&
                config.primitiveCount <= CausticInvalidIdentity - config.firstPrimitive);

        return input.resourcesPrepared && input.certificatesCurrent && input.proposalPrepared &&
            input.recordingAccepted && validThresholds && validRange && input.classification.active == 1u &&
            input.classification.primary == CausticReferencePrimary && input.classification.maxPathDepth == 2u &&
            input.classification.unsupportedFeatures == 0u &&
            input.classification.receiverPolicy == CausticFullReceiverBSDF &&
            input.classification.registrationMode >= CausticWholeInstance &&
            input.classification.registrationMode <= CausticPrimitiveRange &&
            (input.classification.registrationMode != CausticPrimitiveRange || input.classification.primitiveCount > 0u) &&
            input.classification.registeredInstance != CausticInvalidIdentity &&
            input.lightCount <= 64u && input.classification.selectedPointLight < input.lightCount &&
            input.selectedLightType == CausticPointLight && CausticValidPublication(input.current) &&
            CausticSamePublication(input.current, input.classification.publication) &&
            PlanMemory(input.width, input.height, input.attempts, plan);
    }

    // Diagnostic readiness is deliberately NOT a camera-subtraction authorization.
    // This stage has no state that can enable contribution replacement.
    struct AccumulationIdentity
    {
        CausticPublication publication{};
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t featureMode = 0;
        std::uint32_t selectedLight = CausticInvalidIdentity;
        std::uint32_t instance = CausticInvalidIdentity;
        std::uint32_t registrationMode = 0;
        std::uint32_t firstPrimitive = 0;
        std::uint32_t primitiveCount = 0;
        float reflectorThreshold = 0;
        float receiverThreshold = 0;
    };

    inline bool SameAccumulationTarget(const AccumulationIdentity& left, const AccumulationIdentity& right)
    {
        return CausticSamePublication(left.publication, right.publication) && left.width == right.width &&
            left.height == right.height && left.featureMode == right.featureMode &&
            left.selectedLight == right.selectedLight && left.instance == right.instance &&
            left.registrationMode == right.registrationMode && left.firstPrimitive == right.firstPrimitive &&
            left.primitiveCount == right.primitiveCount && left.reflectorThreshold == right.reflectorThreshold &&
            left.receiverThreshold == right.receiverThreshold;
    }

    inline bool CombineBatch(double previousMean, double cameraSum, double rawLightSum,
        std::uint32_t attempts, std::uint32_t batch, std::uint32_t previousSamples, double& output)
    {
        if(attempts == 0 || attempts > CausticMaximumAttempts || batch == 0 || batch > 64 ||
            previousSamples > MaximumProgressiveSamples - batch || !std::isfinite(previousMean) ||
            !std::isfinite(cameraSum) || !std::isfinite(rawLightSum) ||
            previousMean < 0 || cameraSum < 0 || rawLightSum < 0)
        {
            return false;
        }

        // Real-arithmetic contract: add S*C once, with C = raw/N; counts K and N differ.
        // Caller uses the unpartitioned camera path when preparation/N is invalid.
        const double total = double(previousSamples) + batch;
        const double result = previousMean * (double(previousSamples) / total) + cameraSum / total +
            (rawLightSum / attempts) * (double(batch) / total);

        if(!std::isfinite(result))
        {
            return false;
        }

        output = result;
        return true;
    }
}

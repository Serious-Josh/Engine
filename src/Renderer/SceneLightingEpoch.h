#pragma once

#include <cstdint>
#include <limits>

// SHARC.4B scene-lighting epoch: identifies the radiance-affecting scene state a path-traced frame was recorded with,
// so a future world-space radiance cache can tell whether its contents still describe the scene. CPU-side only: it
// makes no GPU synchronization guarantee (a cache must still order its own clear before any access, see
// build/sharc-4a/REPORT.md section 4).
//
// The epoch advances at a latch (once per path-traced frame recording, Renderer::LatchSceneLighting) when an explicit
// invalidation was raised since the previous latch or the frame's radiance-affecting inputs differ from the previous
// latch's. Several invalidations before one latch advance it once. Camera, resolution and history-only resets never
// raise an invalidation and are not inputs.

// Why the radiance-affecting state changed (bit flags; the reasons of the latest advance are reported).
enum class SceneLightingInvalidation : std::uint32_t
{
    None = 0,
    // A scene's resources were initialized (a new scene lifetime).
    SceneLifetime = 1u << 0,
    // The scene's resources were released or a failed initialization was cleaned up: unusable until a later latch.
    SceneRelease = 1u << 1,
    // Mesh geometry or BLAS content replaced (ReplaceMeshGeometry, GenerateDisplacementMeshGPU).
    Geometry = 1u << 2,
    // A displacement update is being prepared or was finalized.
    Displacement = 1u << 3,
    // The environment map changed (SetEnvironment, SetEnvironmentImage).
    Environment = 1u << 4,
    // A new material-table revision is recorded for this frame.
    Material = 1u << 5,
    // Ray-tracing membership (objects, meshes, per-object material bindings) was adopted.
    Membership = 1u << 6,
    // The frame's radiance-affecting inputs differ from the previous latch's (lights, sun, TLAS, settings, ...).
    Inputs = 1u << 7,
    // No latch has happened yet in this renderer lifetime.
    Initial = 1u << 8,
};

constexpr SceneLightingInvalidation operator|(SceneLightingInvalidation left, SceneLightingInvalidation right)
{
    return static_cast<SceneLightingInvalidation>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

constexpr bool HasSceneLightingInvalidation(SceneLightingInvalidation reasons, SceneLightingInvalidation reason)
{
    return (static_cast<std::uint32_t>(reasons) & static_cast<std::uint32_t>(reason)) != 0u;
}

// What a radiance cache's contents mean: any difference makes existing contents uninterpretable. All zero means no
// cache is configured (no production cache exists yet); a future cache sets every field from its own configuration.
struct RadianceCacheConfigurationIdentity
{
    // The cache implementation and its stored layout (for example the D6 moment layout), each versioned by the cache.
    std::uint32_t implementationVersion = 0;
    std::uint32_t layoutVersion = 0;
    // The world-space key: cell edge in metres and the key encoding version.
    float cellSize = 0.0f;
    std::uint32_t keyVersion = 0;
    // The hash table: slot count and slots per bucket.
    std::uint32_t slotCount = 0;
    std::uint32_t bucketSlots = 0;

    bool operator==(const RadianceCacheConfigurationIdentity&) const = default;
};

class SceneLightingEpoch
{
public:
    struct Status
    {
        // The epoch of the latest latch; meaningful only while valid.
        std::uint64_t epoch = 0;
        // Whether the latest latch established a usable epoch that no later invalidation has superseded. False before
        // the first latch, from any invalidation until the next latch, for unusable inputs, and permanently once the
        // counter is exhausted.
        bool valid = false;
        // The reasons of the latest advance.
        SceneLightingInvalidation reasons = SceneLightingInvalidation::None;
        std::uint64_t latches = 0;
        std::uint64_t advances = 0;
    };

    explicit SceneLightingEpoch(std::uint64_t firstEpoch = 0) noexcept
        : m_nextEpoch(firstEpoch)
    {
    }

    // Records a radiance-affecting change. The latched epoch no longer describes the scene, so the status is invalid
    // until the next latch, which advances.
    void Invalidate(SceneLightingInvalidation reason) noexcept
    {
        m_pending = m_pending | reason;
        m_status.valid = false;
    }

    // inputsChanged: the frame's radiance-affecting inputs differ from the previous latch's, or none was latched.
    // inputsUsable: the inputs identify a usable scene state. Returns the frame's status.
    const Status& Latch(bool inputsChanged, bool inputsUsable) noexcept
    {
        ++m_status.latches;
        SceneLightingInvalidation reasons = m_pending;
        m_pending = SceneLightingInvalidation::None;

        if(inputsChanged)
        {
            reasons = reasons | SceneLightingInvalidation::Inputs;
        }

        if(m_exhausted)
        {
            m_status.valid = false;
            return m_status;
        }

        if(reasons != SceneLightingInvalidation::None)
        {
            // Exhaustion is permanent: an epoch value is never reused for a different state.
            if(m_nextEpoch == (std::numeric_limits<std::uint64_t>::max)())
            {
                m_exhausted = true;
                m_status.valid = false;
                return m_status;
            }

            m_status.epoch = m_nextEpoch++;
            m_status.reasons = reasons;
            ++m_status.advances;
        }

        // Usable only if this latch's epoch comes from usable inputs: either it advanced here, or it is the previous
        // latch's still-valid epoch (an invalidation would have cleared that and forced an advance above).
        m_status.valid = inputsUsable && (m_status.valid || reasons != SceneLightingInvalidation::None);

        // The next usable latch must issue a fresh epoch rather than resume one recorded with unusable inputs.
        if(!inputsUsable)
        {
            m_pending = m_pending | SceneLightingInvalidation::Inputs;
        }

        return m_status;
    }

    // Renderer shutdown or reinitialization: the next latch advances. The counter keeps increasing, so an epoch seen
    // before is never reissued within this object's lifetime.
    void Restart() noexcept
    {
        m_pending = m_pending | SceneLightingInvalidation::Initial;
        m_status.valid = false;
    }

    const Status& GetStatus() const noexcept
    {
        return m_status;
    }

private:
    std::uint64_t m_nextEpoch = 0;
    SceneLightingInvalidation m_pending = SceneLightingInvalidation::Initial;
    bool m_exhausted = false;
    Status m_status;
};

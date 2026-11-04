#pragma once

#include "Audio/AudioSystem.h"

#include <array>
#include <cmath>

// SA.2: the coordinate boundary between Engine/FMOD and Steam Audio, in one place.
//
// Engine and FMOD: left-handed, +X right, +Y up, +Z forward, metres (Audio::Attributes3D).
// Steam Audio:     right-handed, +X right, +Y up, -Z ahead, metres (IPLVector3, IPLCoordinateSpace3).
// Negating Z maps one onto the other for points, directions and velocities alike.
//
// Who converts what:
// * Direct HRTF (SA.2) needs no Engine-side conversion. The FMOD plugin reads each event's FMOD 3D attributes and
//   FMOD listener 0 itself and converts them exactly as ToSteamAudioCoordinateSpace does (steamaudio_fmod.cpp,
//   calcCoordinates). Engine keeps sending ordinary FMOD attributes; converting them here as well would invert Z twice.
// * Simulation inputs (later milestones: iplSourceSetInputs, iplSimulatorSetSharedInputs, geometry) are passed to the
//   C API by Engine and must go through these helpers, so they agree with the plugin's view of the same scene.
//
// SDK-free on purpose: the values are plain floats in IPL layout, so tests and callers need no Steam Audio headers.
namespace Audio::SteamAudio
{
using Vector = std::array<float, 3>;

struct CoordinateSpace
{
    Vector right{1.0f, 0.0f, 0.0f};
    Vector up{0.0f, 1.0f, 0.0f};
    Vector ahead{0.0f, 0.0f, -1.0f};
    Vector origin{0.0f, 0.0f, 0.0f};
};

// A point, direction or velocity from Engine/FMOD space to Steam Audio space.
constexpr Vector ToSteamAudio(const Vector& engine) noexcept
{
    return {engine[0], engine[1], -engine[2]};
}

// The inverse mapping (the conversion is its own inverse).
constexpr Vector FromSteamAudio(const Vector& steamAudio) noexcept
{
    return {steamAudio[0], steamAudio[1], -steamAudio[2]};
}

constexpr Vector Cross(const Vector& a, const Vector& b) noexcept
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

constexpr float Dot(const Vector& a, const Vector& b) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline Vector Normalize(const Vector& value) noexcept
{
    const float length = std::sqrt(Dot(value, value));

    if(length <= 0.0f)
    {
        return value;
    }

    return {value[0] / length, value[1] / length, value[2] / length};
}

// A listener or source pose as Steam Audio sees it. Engine's forward (+Z) becomes Steam Audio's ahead (-Z), so the
// converted ahead vector already points the right way; right = ahead x up in Steam Audio's right-handed frame, which
// for an identity Engine pose is +X, as in Engine. Matches the FMOD plugin's calcCoordinates.
inline CoordinateSpace ToSteamAudioCoordinateSpace(const Attributes3D& attributes) noexcept
{
    CoordinateSpace space;
    space.ahead = ToSteamAudio(attributes.forward);
    space.up = ToSteamAudio(attributes.up);
    space.right = Normalize(Cross(space.ahead, space.up));
    space.origin = ToSteamAudio(attributes.position);
    return space;
}

// A world transform in Engine's convention (column vectors, translation in column 3) as the same placement in Steam Audio
// space: Z M Z, with Z = diag(1, 1, -1), for geometry whose vertices went through ToSteamAudio. Steam Audio's matrices are
// also row-indexed with translation in column 3 (checked by the transformed-geometry tests).
constexpr AcousticTransform ToSteamAudioTransform(const AcousticTransform& engine) noexcept
{
    AcousticTransform result = engine;

    for(int row = 0; row < 3; ++row)
    {
        result[row][2] = -result[row][2];
        result[2][row] = -result[2][row];
    }

    result[2][3] = -engine[2][3];
    return result;
}

// Engine velocity (metres per second) in Steam Audio space. The FMOD plugin does not use velocity (no Doppler in the
// Steam Audio spatializer), so this exists for future simulation inputs only.
constexpr Vector ToSteamAudioVelocity(const Vector& engine) noexcept
{
    return ToSteamAudio(engine);
}
}

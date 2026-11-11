#include "Scene/VoxelComponents.h"

namespace
{
    bool Fail(std::string* error, std::string message)
    {
        if(error != nullptr)
        {
            *error = std::move(message);
        }
        return false;
    }
}

const char* VoxelParticipationToString(VoxelParticipation participation) noexcept
{
    switch(participation)
    {
    case VoxelParticipation::Automatic:
        return "automatic";
    case VoxelParticipation::Static:
        return "static";
    case VoxelParticipation::Excluded:
        return "excluded";
    }
    return "automatic";
}

bool TryParseVoxelParticipation(std::string_view text, VoxelParticipation& participation) noexcept
{
    for(const VoxelParticipation value : {VoxelParticipation::Automatic, VoxelParticipation::Static, VoxelParticipation::Excluded})
    {
        if(text == VoxelParticipationToString(value))
        {
            participation = value;
            return true;
        }
    }
    return false;
}

const char* VoxelOrientationToString(VoxelOrientation orientation) noexcept
{
    switch(orientation)
    {
    case VoxelOrientation::Automatic:
        return "automatic";
    case VoxelOrientation::AsAuthored:
        return "asAuthored";
    case VoxelOrientation::Outward:
        return "outward";
    }
    return "automatic";
}

bool TryParseVoxelOrientation(std::string_view text, VoxelOrientation& orientation) noexcept
{
    for(const VoxelOrientation value : {VoxelOrientation::Automatic, VoxelOrientation::AsAuthored, VoxelOrientation::Outward})
    {
        if(text == VoxelOrientationToString(value))
        {
            orientation = value;
            return true;
        }
    }
    return false;
}

bool ValidateVoxelSettings(const VoxelSettings& settings, std::string* error)
{
    if(settings.participation > VoxelParticipation::Excluded)
    {
        return Fail(error, "unknown voxel participation");
    }

    if(settings.orientation > VoxelOrientation::Outward)
    {
        return Fail(error, "unknown voxel orientation");
    }

    return true;
}

bool ValidateVoxelBakeSettings(const VoxelBakeSettings& settings, std::string* error)
{
    if(settings.baseExponent < -7 || settings.baseExponent > 4)
    {
        return Fail(error, "voxel size exponent must be between -7 and 4");
    }

    if(settings.regionBricks != 16 && settings.regionBricks != 32 && settings.regionBricks != 64)
    {
        return Fail(error, "bricks per region axis must be 16, 32 or 64");
    }

    if(settings.narrowBand < 1 || settings.narrowBand > 32)
    {
        return Fail(error, "narrow band must be between 1 and 32 voxels");
    }

    if(settings.coarseClamp < 1 || settings.coarseClamp > 16)
    {
        return Fail(error, "coarse clamp must be between 1 and 16 bricks");
    }

    if(!settings.manifestReference.empty() && settings.manifestReference.rfind("asset:", 0) != 0)
    {
        return Fail(error, "the voxel manifest must be a project asset reference");
    }

    return true;
}

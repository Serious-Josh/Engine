#pragma once

#include <cmath>
#include <compare>
#include <cstdint>

enum class MeshImportUnitMode : std::uint8_t
{
    Legacy,
    FileUnits,
    Manual
};

// Asset-local conversion, independent of authored object transforms. Missing registry data
// is Legacy; callers importing external FBX files explicitly select FileUnits.
struct MeshImportUnits
{
    MeshImportUnitMode mode = MeshImportUnitMode::Legacy;
    double scale = 1.0;

    bool IsValid() const noexcept
    {
        if(mode == MeshImportUnitMode::Manual)
        {
            return std::isfinite(scale) && scale > 0.0;
        }

        return (mode == MeshImportUnitMode::Legacy || mode == MeshImportUnitMode::FileUnits) && scale == 1.0;
    }

    bool operator==(const MeshImportUnits&) const = default;
    auto operator<=>(const MeshImportUnits&) const = default;
};

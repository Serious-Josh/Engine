#include "Scene/SceneAssetIdentity.h"

#include "assets/CookedSurface.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>

// Moved unchanged from BuildGame::SceneIdentity (W1.3); the golden value in SceneKindTests guards the mapping.
AssetID SceneAssetIdentity(std::string_view persistentID)
{
    if (persistentID.empty())
    {
        throw std::runtime_error("Current scene has no persistent identity.");
    }

    if (auto assetID = AssetID::Parse(persistentID))
    {
        return *assetID;
    }

    const std::string namespacedIdentity = "Engine.Scene:" + std::string(persistentID);
    auto digest = CookedSurface::Hash(
        std::as_bytes(std::span(namespacedIdentity.data(), namespacedIdentity.size())));
    digest[6] = std::byte((std::to_integer<unsigned>(digest[6]) & 15) | 0x50);
    digest[8] = std::byte((std::to_integer<unsigned>(digest[8]) & 63) | 0x80);

    constexpr char hex[] = "0123456789abcdef";
    std::string uuid;
    for (size_t index = 0; index < 16; ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
        {
            uuid += '-';
        }

        auto byte = std::to_integer<unsigned>(digest[index]);
        uuid += hex[byte >> 4];
        uuid += hex[byte & 15];
    }

    return *AssetID::Parse(uuid);
}

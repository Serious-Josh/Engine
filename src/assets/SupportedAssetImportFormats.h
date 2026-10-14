#pragma once

#include "assets/AssetRecord.h"

#include <array>
#include <optional>
#include <string_view>

// New external imports only. Existing registry assets and runtime loaders do not
// use this policy; their readable formats remain unchanged.
namespace SupportedAssetImportFormats
{
    struct Format
    {
        std::wstring_view extension;
        AssetType category;
    };

    inline constexpr std::array<Format, 2> Formats = {{{L".fbx", AssetType::Mesh}, {L".dds", AssetType::Texture}}};
    inline constexpr const char* UnsupportedMessage =
        "Unsupported import format. Use .fbx for meshes or .dds for textures.";

    inline std::optional<AssetType> Lookup(const std::filesystem::path& source)
    {
        auto extension = source.extension().wstring();

        for(auto& character : extension)
        {
            if(character >= L'A' && character <= L'Z')
            {
                character += L'a' - L'A';
            }
        }

        for(const auto& format : Formats)
        {
            if(extension == format.extension)
            {
                return format.category;
            }
        }

        return std::nullopt;
    }
}

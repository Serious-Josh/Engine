#include "assets/AssetID.h"

#include <Windows.h>
#include <objbase.h>

#include <cstdio>
#include <utility>

namespace
{
    constexpr std::string_view kAssetPrefix = "asset:";

    bool IsLowerHex(char character)
    {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    }

    bool IsCanonicalUUID(std::string_view value)
    {
        if (value.size() != 36)
        {
            return false;
        }

        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (value[i] != '-')
                {
                    return false;
                }
            }
            else if (!IsLowerHex(value[i]))
            {
                return false;
            }
        }

        return true;
    }
}

AssetID::AssetID(std::string value) : m_value(std::move(value))
{
}

std::optional<AssetID> AssetID::Generate()
{
    GUID guid{};

    if(FAILED(CoCreateGuid(&guid))){
        return std::nullopt;
    }

    char buffer[37]{};

    const int written = std::snprintf(
        buffer,
        sizeof(buffer),
        "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned long>(guid.Data1),
        static_cast<unsigned int>(guid.Data2),
        static_cast<unsigned int>(guid.Data3),
        static_cast<unsigned int>(guid.Data4[0]),
        static_cast<unsigned int>(guid.Data4[1]),
        static_cast<unsigned int>(guid.Data4[2]),
        static_cast<unsigned int>(guid.Data4[3]),
        static_cast<unsigned int>(guid.Data4[4]),
        static_cast<unsigned int>(guid.Data4[5]),
        static_cast<unsigned int>(guid.Data4[6]),
        static_cast<unsigned int>(guid.Data4[7]));

    if(written != 36){
        return std::nullopt;
    }

    return Parse(buffer);
}

std::optional<AssetID> AssetID::Parse(std::string_view value)
{
    if(!IsCanonicalUUID(value)){
        return std::nullopt;
    }

    return AssetID(std::string(value));
}

std::optional<AssetID> AssetID::ParseReference(std::string_view reference)
{
    if(!reference.starts_with(kAssetPrefix)){
        return std::nullopt;
    }

    return Parse(reference.substr(kAssetPrefix.size()));
}

const std::string& AssetID::ToString() const noexcept
{
    return m_value;
}

std::string AssetID::ToReference() const
{
    return std::string(kAssetPrefix) + m_value;
}
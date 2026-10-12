#pragma once

#include <optional>
#include <string>
#include <string_view>

class AssetID
{
    public:
        static std::optional<AssetID> Generate();
        static std::optional<AssetID> Parse(std::string_view value);
        static std::optional<AssetID> ParseReference(std::string_view reference);

        const std::string& ToString() const noexcept;
        std::string ToReference() const;

        bool operator==(const AssetID&) const = default;

    private:
        explicit AssetID(std::string value);

        std::string m_value;
};
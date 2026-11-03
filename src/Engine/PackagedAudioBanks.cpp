#include "Engine/PackagedAudioBanks.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ProjectAssetRegistry.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace PackagedAudioBanks
{
namespace
{
    bool IsMasterBank(const AssetRecord& record)
    {
        return record.relativePath.filename().wstring().starts_with(L"Master.");
    }

    std::uint32_t ReadLittleEndian32(std::span<const std::byte> bytes, std::size_t offset)
    {
        std::uint32_t value = 0;

        for(std::size_t index = 0; index < 4; ++index)
        {
            value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8 * index);
        }

        return value;
    }
}

std::vector<AssetRecord> LoadOrder(std::vector<AssetRecord> banks)
{
    std::stable_partition(banks.begin(), banks.end(), IsMasterBank);
    return banks;
}

std::vector<std::byte> EncodeList(const std::vector<AssetRecord>& banks)
{
    // A registry holding only the banks, so the list is the registry's own validated serialization.
    ProjectAssetRegistry list;

    for(const AssetRecord& bank : banks)
    {
        if(bank.type != AssetType::AudioBank || !list.Register(bank))
        {
            throw std::runtime_error("Audio bank " + bank.id.ToReference() +
                " is not a distinct AudioBank record and cannot be packaged.");
        }
    }

    const std::string text = AssetRegistrySerializer::Serialize(list).dump();

    if(text.size() > MaximumListBytes)
    {
        throw std::runtime_error("The packaged audio bank list exceeds 1 MiB.");
    }

    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::vector<AssetRecord> DecodeList(std::span<const std::byte> bytes)
{
    if(bytes.size() > MaximumListBytes)
    {
        throw std::runtime_error("The packaged audio bank list exceeds 1 MiB.");
    }

    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    nlohmann::json document;

    try
    {
        document = nlohmann::json::parse(text);
    }
    catch(const std::exception& exception)
    {
        throw std::runtime_error(std::string("The packaged audio bank list is not valid JSON: ") + exception.what());
    }

    ProjectAssetRegistry list;
    std::string error;

    if(!AssetRegistrySerializer::Deserialize(document, list, &error))
    {
        throw std::runtime_error("The packaged audio bank list is invalid: " + error);
    }

    std::vector<AssetRecord> banks = list.GetAssets();

    for(const AssetRecord& bank : banks)
    {
        if(bank.type != AssetType::AudioBank)
        {
            throw std::runtime_error("The packaged audio bank list names a non-bank asset " + bank.id.ToReference() + ".");
        }
    }

    return banks;
}

bool IsBank(std::span<const std::byte> bytes, std::string& error)
{
    constexpr std::size_t HeaderBytes = 12;

    if(bytes.size() < HeaderBytes || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "FEV ", 4) != 0)
    {
        error = "not an FMOD Studio bank (no RIFF \"FEV \" header)";
        return false;
    }

    const std::uint64_t declaredBytes = static_cast<std::uint64_t>(ReadLittleEndian32(bytes, 4)) + 8;

    if(declaredBytes != bytes.size())
    {
        error = "damaged FMOD Studio bank: its header declares " + std::to_string(declaredBytes) + " bytes, the file has " +
            std::to_string(bytes.size()) + " (truncated or padded)";
        return false;
    }

    error.clear();
    return true;
}
}

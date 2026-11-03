#pragma once

#include "assets/AssetRecord.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Audio.1D: FMOD Studio banks in packaged games. Each bank is its own Game.eap Resource entry under its AudioBank
// asset identity, carrying the .bank file's bytes unchanged. One further Resource entry at a reserved identity lists
// them: the AudioBank records (identity and project-relative path) in the asset registry's own serialization, in load
// order. Archives without that entry (built before Audio.1D, or projects without banks) have no audio banks.
namespace PackagedAudioBanks
{
    // Reserved like PackagedBootstrap's ...035/...036 and PackagedRenderingSettings' ...037.
    inline AssetID ListID()
    {
        return *AssetID::Parse("00000000-0000-0000-0000-000000000038");
    }

    // Every non-scene archive payload is written as version 1.
    constexpr std::uint32_t PayloadVersion = 1;
    constexpr std::size_t MaximumListBytes = 1024 * 1024;
    // EngineAssetPak's per-entry payload and read limit (Architecture §7.5).
    constexpr std::size_t MaximumBankBytes = 256u * 1024u * 1024u;

    // Master banks (Master.bank, Master.strings.bank) first, then the rest in their given order. FMOD resolves buses
    // and event paths through the master banks.
    std::vector<AssetRecord> LoadOrder(std::vector<AssetRecord> banks);

    // Throws std::runtime_error for anything but valid, distinct AudioBank records.
    std::vector<std::byte> EncodeList(const std::vector<AssetRecord>& banks);
    std::vector<AssetRecord> DecodeList(std::span<const std::byte> bytes);

    // Structural check without FMOD: an FMOD Studio bank is a RIFF container of form type "FEV " whose RIFF size
    // covers exactly the whole file. False, with the reason, for anything else (truncated, padded, foreign data).
    bool IsBank(std::span<const std::byte> bytes, std::string& error);
}

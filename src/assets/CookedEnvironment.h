#pragma once
#include "assets/EngineAssetPak.h"
#include "Renderer/ImageLoader.h"
#include <optional>
namespace CookedScene { struct ArchiveScene; }
namespace CookedEnvironment
{
inline constexpr std::uint32_t Version=1;
inline constexpr size_t MaxPixels=8*1024*1024, MaxBytes=72+MaxPixels*16;
HDRImageData ReadLoose(const std::filesystem::path&);
std::vector<std::byte> Cook(const std::filesystem::path&,const AssetID&,std::uint64_t generation);
HDRImageData Decode(std::span<const std::byte>,const AssetID&,std::uint64_t generation);
std::optional<HDRImageData> ReadArchive(const CookedScene::ArchiveScene&);
std::string ArchiveIdentity(const CookedScene::ArchiveScene&);
}

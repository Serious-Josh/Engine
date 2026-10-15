#include "assets/CookedEnvironment.h"
#include "assets/CookedScene.h"
#include "assets/CookedSurface.h"
#include <Windows.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace CookedEnvironment
{
namespace
{
void Check(bool ok,const char* error) { if(!ok) throw std::runtime_error(error); }
void Validate(const HDRImageData& image)
{
    Check(image.width && image.height && image.width<=8192 && image.height<=8192 &&
        std::uint64_t(image.width)*image.height<=MaxPixels && image.channels==4 &&
        image.pixels.size()==std::uint64_t(image.width)*image.height*4,"Invalid or oversized HDR environment dimensions/layout.");
    double energy=0;
    for(size_t i=0;i<image.pixels.size();++i) { const auto v=image.pixels[i]; Check(std::isfinite(v) && v>=0,"HDR environment contains invalid radiance."); if(i%4!=3) energy+=v; }
    Check(energy>0 && energy<=(std::numeric_limits<float>::max)(),"HDR environment has zero energy; existing importance sampling requires nonzero radiance.");
}
}
HDRImageData ReadLoose(const std::filesystem::path& path)
{
    Check(_wcsicmp(path.extension().c_str(),L".hdr")==0,"Assigned environment must be a Radiance .hdr asset.");
    Check(std::filesystem::file_size(path)<=64*1024*1024,"HDR source exceeds 64 MiB.");
    HDRImageData image; Check(ImageLoader::LoadHDR(path,image),"Cannot decode required HDR environment."); Validate(image); return image;
}
std::vector<std::byte> Cook(const std::filesystem::path& path,const AssetID& id,std::uint64_t generation)
{
    HANDLE pin=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    Check(pin!=INVALID_HANDLE_VALUE,"Cannot pin required HDR source.");
    struct Close { HANDLE h; ~Close(){CloseHandle(h);} } close{pin};
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(pin,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),"HDR source must be an ordinary file.");
    Check(generation!=0,"HDR content generation must be nonzero.");
    const auto image=ReadLoose(path);
    std::vector<std::byte> bytes; bytes.reserve(72+image.pixels.size()*4);
    for(auto c:std::string_view("ENGENV01")) bytes.push_back(std::byte(c));
    const auto put=[&](std::uint64_t v,size_t width) { for(size_t i=0;i<width;++i) bytes.push_back(std::byte(v>>(8*i))); };
    put(Version,4); put(generation,8); for(auto c:id.ToString()) bytes.push_back(std::byte(c));
    put(image.width,4); put(image.height,4); put(image.pixels.size()*4,8);
    static_assert(std::endian::native==std::endian::little && sizeof(float)==4);
    const auto pixels=std::as_bytes(std::span(image.pixels)); bytes.insert(bytes.end(),pixels.begin(),pixels.end()); return bytes;
}
HDRImageData Decode(std::span<const std::byte> bytes,const AssetID& id,std::uint64_t generation)
{
    Check(bytes.size()>=72 && bytes.size()<=MaxBytes,"Truncated/oversized cooked environment.");
    size_t cursor=0; const auto get=[&](size_t width) { Check(width<=bytes.size()-cursor,"Truncated environment header."); std::uint64_t v=0; for(size_t i=0;i<width;++i) v|=std::uint64_t(bytes[cursor++])<<(8*i); return v; };
    for(auto c:std::string_view("ENGENV01")) Check(get(1)==static_cast<unsigned char>(c),"Invalid cooked environment magic.");
    Check(get(4)==Version && get(8)==generation && generation,"Unsupported/stale cooked environment version/generation.");
    std::string identity; for(size_t i=0;i<36;++i) identity+=static_cast<char>(get(1)); Check(identity==id.ToString(),"Cooked environment identity mismatch.");
    HDRImageData image; image.width=static_cast<unsigned>(get(4)); image.height=static_cast<unsigned>(get(4)); image.channels=4;
    const auto size=get(8); Check(image.width && image.height && image.width<=8192 && image.height<=8192 &&
        std::uint64_t(image.width)*image.height<=MaxPixels && size==std::uint64_t(image.width)*image.height*16 && size==bytes.size()-cursor,"Invalid cooked environment pixel size.");
    image.pixels.resize(static_cast<size_t>(size/4)); std::memcpy(image.pixels.data(),bytes.data()+cursor,static_cast<size_t>(size)); Validate(image); return image;
}
std::optional<HDRImageData> ReadArchive(const CookedScene::ArchiveScene& scene)
{
    if(scene.metadata.document.environmentReference.empty()) return {};
    const auto id=AssetID::ParseReference(scene.metadata.document.environmentReference);
    Check(bool(id) && scene.metadata.environment.has_value(),"Missing authored environment binding.");
    auto bytes=scene.provider->ReadEntry(*id,EngineAssetPak::Type::Resource,Version,MaxBytes);
    return Decode(bytes,*id,scene.metadata.environment->generation);
}
std::string ArchiveIdentity(const CookedScene::ArchiveScene& scene)
{
    if(scene.metadata.document.environmentReference.empty()) return "neutral";
    return CookedSurface::Identity(*scene.provider,*AssetID::ParseReference(scene.metadata.document.environmentReference));
}
}

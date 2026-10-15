#include "assets/ProjectTexturePreparationService.h"
#include "assets/CookedSurface.h"
#include "assets/MaterialAssetSerializer.h"
#include "assets/ProjectAssetResolver.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <map>
#include <set>
#include <fstream>
#include <stdexcept>
#pragma comment(lib,"bcrypt.lib")
namespace CookedSurface
{
namespace
{
    void Check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
    struct SourcePin
    {
        HANDLE handle=INVALID_HANDLE_VALUE;
        explicit SourcePin(const std::filesystem::path& path)
        {
            handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            if(handle==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle,&info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
            { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); handle=INVALID_HANDLE_VALUE; throw std::runtime_error("Cannot pin regular surface cook input."); }
        }
        ~SourcePin() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
        SourcePin(const SourcePin&)=delete;
    };
    struct Write
    {
        size_t limit; std::vector<std::byte> bytes;
        void U(std::uint64_t n,size_t width) { Check(bytes.size()<=limit && width<=limit-bytes.size(),"Cooked surface byte limit."); for(size_t i=0;i<width;++i) bytes.push_back(std::byte((n>>(i*8))&255)); }
        void Raw(std::span<const std::byte> b) { Check(bytes.size()<=limit && b.size()<=limit-bytes.size(),"Cooked surface byte limit."); bytes.insert(bytes.end(),b.begin(),b.end()); }
        void Header(const char* magic,const AssetID& id,std::uint64_t generation)
        { Raw({reinterpret_cast<const std::byte*>(magic),8}); U(Version,4); U(generation,8); Raw({reinterpret_cast<const std::byte*>(id.ToString().data()),36}); }
    };
    struct Read
    {
        std::span<const std::byte> bytes; size_t cursor=0;
        std::span<const std::byte> Raw(size_t n) { Check(n<=bytes.size()-cursor,"Truncated cooked surface."); auto b=bytes.subspan(cursor,n); cursor+=n; return b; }
        std::uint64_t U(size_t width) { auto b=Raw(width); std::uint64_t n=0; for(size_t i=0;i<width;++i) n|=std::uint64_t(std::to_integer<unsigned char>(b[i]))<<(i*8); return n; }
        std::pair<AssetID,std::uint64_t> Header(const char* magic)
        {
            auto b=Raw(8); Check(std::equal(b.begin(),b.end(),reinterpret_cast<const std::byte*>(magic)),"Invalid cooked surface magic.");
            Check(U(4)==Version,"Unsupported cooked surface version."); const auto generation=U(8); b=Raw(36);
            auto id=AssetID::Parse({reinterpret_cast<const char*>(b.data()),b.size()}); Check(id && generation,"Invalid cooked surface identity/generation."); return {*id,generation};
        }
    };
    std::string Hex(const EngineAssetPak::Digest& hash)
    { std::string s; constexpr char digits[]="0123456789abcdef"; for(auto b:hash) { const auto n=std::to_integer<unsigned char>(b); s+=digits[n>>4]; s+=digits[n&15]; } return s; }
}
std::array<std::optional<std::string>,7> References(const MaterialAssetData& m)
{ return {m.baseColorTexture,m.normalTexture,m.roughnessTexture,m.metallicTexture,m.armTexture,m.aoTexture,m.heightTexture}; }
ProjectTextureInterpretation Interpretation(size_t slot)
{ return slot==0?ProjectTextureInterpretation::SRGBColor:slot==1?ProjectTextureInterpretation::NormalMap:ProjectTextureInterpretation::LinearData; }
EngineAssetPak::Digest Hash(std::span<const std::byte> bytes)
{
    Check(bytes.size()<=ULONG_MAX,"Surface hash limit."); BCRYPT_ALG_HANDLE h{}; Check(BCryptOpenAlgorithmProvider(&h,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 unavailable.");
    EngineAssetPak::Digest d{}; const auto status=BCryptHash(h,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),static_cast<ULONG>(bytes.size()),reinterpret_cast<PUCHAR>(d.data()),32);
    BCryptCloseAlgorithmProvider(h,0); Check(status>=0,"Surface SHA256 failed."); return d;
}
std::string Identity(const CookedMeshSource::ArchiveProvider& source,const AssetID& id)
{ auto* e=source.ArchiveEntry(id); Check(e!=nullptr,"Missing archive surface."); return source.Mapping().manifestSHA256+":"+Hex(e->sha256); }
bool Matches(const CookedMeshSource::ArchiveProvider& source,const AssetID& id,EngineAssetPak::Type type,std::uint64_t generation,std::string_view identity)
{ const auto* e=source.ArchiveEntry(id); return e && e->type==type && e->payloadVersion==Version && e->generation==generation && Identity(source,id)==identity; }
std::vector<std::byte> EncodeMaterial(const Material& m)
{
    std::string error; MaterialAssetData validated;
    const auto json=MaterialAssetSerializer::Serialize(m.parameters);
    Check(m.generation && MaterialAssetSerializer::Deserialize(json,validated,&error),"Invalid cooked material parameters.");
    Write w{MaterialLimit,{}}; w.Header("ENGMAT01",m.id,m.generation); const auto text=json.dump(); w.U(text.size(),4); w.Raw({reinterpret_cast<const std::byte*>(text.data()),text.size()});
    const auto refs=References(m.parameters);
    for(size_t i=0;i<7;++i)
    {
        Check(bool(refs[i])==bool(m.dependencies[i]),"Missing cooked material dependency evidence."); w.U(m.dependencies[i]?1:0,4);
        if(m.dependencies[i]) { Check(AssetID::ParseReference(*refs[i]) && m.dependencies[i]->generation && m.dependencies[i]->sha256!=EngineAssetPak::Digest{},"Invalid cooked texture dependency."); w.U(m.dependencies[i]->generation,8); w.Raw(m.dependencies[i]->sha256); }
    }
    return std::move(w.bytes);
}
Material DecodeMaterial(std::span<const std::byte> bytes)
{
    Check(bytes.size()<=MaterialLimit,"Cooked material byte limit."); Read r{bytes}; const auto [id,generation]=r.Header("ENGMAT01");
    const auto n=r.U(4); auto raw=r.Raw(static_cast<size_t>(n));
    const auto json=nlohmann::json::parse(reinterpret_cast<const char*>(raw.data()),reinterpret_cast<const char*>(raw.data())+raw.size()); Material m{id,generation,{},{}}; std::string error;
    Check(json.dump()==std::string(reinterpret_cast<const char*>(raw.data()),raw.size()),"Noncanonical material JSON/duplicate keys.");
    Check(MaterialAssetSerializer::Deserialize(json,m.parameters,&error) && MaterialAssetSerializer::Serialize(m.parameters)==json,"Invalid or noncanonical cooked material parameters.");
    const auto refs=References(m.parameters);
    for(size_t i=0;i<7;++i)
    {
        auto present=r.U(4); Check(present<=1 && bool(present)==bool(refs[i]),"Invalid cooked dependency flag.");
        if(present) { Dependency d{r.U(8),{}}; auto b=r.Raw(32); std::copy(b.begin(),b.end(),d.sha256.begin()); Check(d.generation && d.sha256!=EngineAssetPak::Digest{} && AssetID::ParseReference(*refs[i]),"Invalid dependency identity/generation."); m.dependencies[i]=d; }
    }
    Check(r.cursor==bytes.size(),"Trailing cooked material data."); return m;
}
std::vector<std::byte> EncodeTexture(const AssetID& id,std::uint64_t generation,std::span<const PreparedProjectTexture> variants)
{
    Check(generation && !variants.empty() && variants.size()<=3,"Invalid texture generation/variant count."); Write w{TextureLimit,{}}; w.Header("ENGTEX01",id,generation); w.U(variants.size(),4);
    int previous=-1;
    for(const auto& v:variants)
    {
        Check(ValidatePreparedProjectTexture(v) && static_cast<int>(v.interpretation)>previous,"Invalid or unsorted prepared texture variant."); previous=static_cast<int>(v.interpretation);
        w.U(static_cast<unsigned>(v.interpretation),4); w.U(v.width,4); w.U(v.height,4); w.U(v.resourceFormat,4); w.U(v.srvFormat,4); w.U(v.subresources.size(),4); w.U(v.pixels.size(),8);
        size_t end=0; for(const auto& mip:v.subresources) { Check(mip.offset==end,"Noncanonical texture offsets."); w.U(mip.offset,8); w.U(mip.rowPitch,8); w.U(mip.slicePitch,8); end+=mip.slicePitch; } Check(end==v.pixels.size(),"Trailing prepared texture pixels.");
        w.Raw({reinterpret_cast<const std::byte*>(v.pixels.data()),v.pixels.size()});
    }
    return std::move(w.bytes);
}
PreparedProjectTexture DecodeTexture(std::span<const std::byte> bytes,const AssetID& expected,std::uint64_t generation,ProjectTextureInterpretation interpretation)
{
    Check(bytes.size()<=TextureLimit,"Cooked texture byte limit."); Read r{bytes}; const auto [id,gen]=r.Header("ENGTEX01"); Check(id==expected && gen==generation,"Texture identity/generation mismatch.");
    const auto count=r.U(4); Check(count && count<=3,"Invalid texture variant count."); std::optional<PreparedProjectTexture> selected; int previous=-1;
    for(size_t i=0;i<count;++i)
    {
        PreparedProjectTexture v; const auto kind=r.U(4); Check(kind<=2 && static_cast<int>(kind)>previous,"Invalid texture interpretation/order."); previous=static_cast<int>(kind); v.interpretation=static_cast<ProjectTextureInterpretation>(kind);
        v.width=static_cast<unsigned>(r.U(4)); v.height=static_cast<unsigned>(r.U(4)); v.resourceFormat=static_cast<DXGI_FORMAT>(r.U(4)); v.srvFormat=static_cast<DXGI_FORMAT>(r.U(4));
        const auto mips=r.U(4),n=r.U(8); Check(mips && mips<=15 && n<=TextureLimit,"Invalid mip/pixel count.");
        size_t end=0;
        for(size_t j=0;j<mips;++j) { auto offset=r.U(8),row=r.U(8),slice=r.U(8); Check(offset==end && offset<=n && slice<=n-offset && row<=SIZE_MAX,"Invalid mip range."); v.subresources.push_back({static_cast<size_t>(offset),static_cast<size_t>(row),static_cast<size_t>(slice)}); end+=static_cast<size_t>(slice); }
        Check(end==n,"Invalid mip total."); auto pixels=r.Raw(static_cast<size_t>(n)); v.pixels.resize(static_cast<size_t>(n)); std::copy(pixels.begin(),pixels.end(),reinterpret_cast<std::byte*>(v.pixels.data()));
        Check(ValidatePreparedProjectTexture(v),"Unsupported cooked texture format/layout."); if(v.interpretation==interpretation) selected=std::move(v);
    }
    Check(r.cursor==bytes.size() && selected.has_value(),"Missing texture interpretation or trailing bytes."); return std::move(*selected);
}
std::vector<Asset> CookDependencies(const CookedScene::Metadata& scene,const ProjectPaths& paths,const ProjectAssetRegistry& registry,std::uint64_t generation)
{
    Check(generation,"Invalid surface cook generation."); std::set<std::string> required;
    Check(scene.document.objects.size()<=CookedScene::Policy{}.maxObjects && ValidateSceneDocument(scene.document),"Invalid surface cook scene.");
    for(const auto& o:scene.document.objects)
    {
        if(!o.materialReference.empty() && o.materialReference!="builtin:default")
        { auto id=AssetID::ParseReference(o.materialReference); Check(bool(id),"Unsupported material identity."); required.insert(id->ToString()); }
        for(const auto& [slot,ref]:o.materialSlotBindings) { auto id=AssetID::ParseReference(ref); Check(bool(id),"Invalid material identity."); required.insert(id->ToString()); }
    }
    Check(required.size()<=4096,"Material cook count limit."); std::map<std::string,MaterialAssetData> materials; std::map<std::string,std::set<ProjectTextureInterpretation>> textures; std::string error;
    for(const auto& text:required)
    {
        const auto id=*AssetID::Parse(text); const auto* record=registry.Find(id); const auto resolved=ProjectAssetResolver::Resolve(paths,registry,id);
        Check(record && record->type==AssetType::Material && resolved.status==AssetAvailability::Available,"Missing material source.");
        SourcePin pin(resolved.resolvedPath); Check(std::filesystem::file_size(resolved.resolvedPath)<=MaterialLimit,"Oversized material source.");
        std::ifstream stream(resolved.resolvedPath,std::ios::binary);
        const auto json=nlohmann::json::parse(stream);
        MaterialAssetData m; Check(MaterialAssetSerializer::Deserialize(json,m,&error),"Invalid source material.");
        const auto schema=MaterialAssetSerializer::Serialize(m);
        for(auto field=json.begin();field!=json.end();++field) Check(schema.contains(field.key()),"Unsupported source material field.");
        for(auto field=json.at("textures").begin();field!=json.at("textures").end();++field)
            Check(schema.at("textures").contains(field.key()),"Unsupported source texture dependency.");
        auto refs=References(m);
        for(size_t i=0;i<7;++i) if(refs[i]) { auto texture=AssetID::ParseReference(*refs[i]); Check(bool(texture),"Unsupported texture source reference."); textures[texture->ToString()].insert(Interpretation(i)); }
        materials.emplace(text,std::move(m));
    }
    Check(textures.size()<=16384,"Texture cook count limit."); std::vector<Asset> result; size_t total=0; std::map<std::string,Dependency> dependencies;
    for(const auto& [text,kinds]:textures)
    {
        const auto id=*AssetID::Parse(text); const auto* record=registry.Find(id); const auto resolved=ProjectAssetResolver::Resolve(paths,registry,id);
        Check(record && record->type==AssetType::Texture && resolved.status==AssetAvailability::Available,"Missing texture source.");
        SourcePin pin(resolved.resolvedPath);
        std::vector<PreparedProjectTexture> variants; size_t pixels=0;
        ProjectTexturePreparationService::EstimateForCooking(resolved.resolvedPath);
        for(auto kind:kinds)
        {
            PreparedProjectTexture image; Check(PrepareProjectTexture(resolved.resolvedPath,kind,image,&error),"Unsupported texture source.");
            Check(image.pixels.size()<=TextureLimit-pixels,"Combined texture variants exceed cooked byte admission.");
            pixels+=image.pixels.size(); variants.push_back(std::move(image));
        }
        auto bytes=EncodeTexture(id,generation,variants);
        Check(bytes.size()<=512ull*1024*1024-total,"Cooked surface batch exceeds 512 MiB admission."); total+=bytes.size();
        dependencies.emplace(text,Dependency{generation,Hash(bytes)}); result.push_back({id,EngineAssetPak::Type::Texture,generation,std::move(bytes)});
    }
    for(const auto& [text,parameters]:materials)
    {
        Material m{*AssetID::Parse(text),generation,parameters,{}}; auto refs=References(parameters);
        for(size_t i=0;i<7;++i) if(refs[i]) m.dependencies[i]=dependencies.at(AssetID::ParseReference(*refs[i])->ToString());
        auto bytes=EncodeMaterial(m); Check(bytes.size()<=512ull*1024*1024-total,"Cooked surface batch exceeds 512 MiB admission."); total+=bytes.size();
        result.push_back({m.id,EngineAssetPak::Type::Material,generation,std::move(bytes)});
    }
    return result;
}
}

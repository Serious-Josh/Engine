#include "assets/CookedScene.h"
#include "assets/CookedEnvironment.h"
#include "assets/CookedSurface.h"
#include "Scene/SceneSerializer.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace CookedScene
{
namespace
{
    constexpr char Magic[] = "ENGSCN01";
    constexpr size_t Header = 32;
    void Require(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
    bool MaterialReference(std::string_view s) { return s.empty() || s=="builtin:default" || (AssetID::ParseReference(s) && AssetID::ParseReference(s)->ToReference()==s); }
    struct Writer
    {
        const Policy& policy;
        std::vector<std::byte> bytes;
        void U(std::uint64_t v,size_t width)
        {
            Require(bytes.size()<=policy.maxBytes && width<=policy.maxBytes-bytes.size(),"Cooked scene exceeds byte limit.");
            for(size_t i=0;i<width;++i) bytes.push_back(std::byte((v>>(8*i))&255));
        }
        void Text(const std::string& s)
        {
            Require(s.size()<=policy.maxStringBytes && s.size()<=UINT32_MAX && s.find('\0')==std::string::npos,"Invalid cooked scene string.");
            U(s.size(),4); for(unsigned char c:s) U(c,1);
        }
        void F(float v) { U(std::bit_cast<std::uint32_t>(v),4); }
        void V(const Vector3& v) { F(v.x); F(v.y); F(v.z); }
    };
    struct Reader
    {
        std::span<const std::byte> bytes;
        const Policy& policy;
        size_t cursor=0;
        std::uint64_t U(size_t width)
        {
            Require(cursor<=bytes.size() && width<=bytes.size()-cursor,"Truncated cooked scene metadata.");
            std::uint64_t v=0; for(size_t i=0;i<width;++i) v|=std::uint64_t(std::to_integer<unsigned char>(bytes[cursor++]))<<(8*i); return v;
        }
        std::string Text()
        {
            const auto n=U(4); Require(n<=policy.maxStringBytes && n<=bytes.size()-cursor,"Invalid cooked scene string length.");
            std::string s(reinterpret_cast<const char*>(bytes.data()+cursor),static_cast<size_t>(n)); cursor+=static_cast<size_t>(n);
            Require(s.find('\0')==std::string::npos,"Embedded NUL in cooked scene string."); return s;
        }
        float F() { const float v=std::bit_cast<float>(static_cast<std::uint32_t>(U(4))); Require(std::isfinite(v),"Nonfinite cooked scene scalar."); return v; }
        Vector3 V() { const auto x=F(),y=F(),z=F(); return {x,y,z}; }
    };
    void Validate(const Metadata& m,const Policy& p)
    {
        std::string error;
        Require(ValidateSceneDocument(m.document,&error),"Invalid cooked scene document/settings/identity.");
        Require(m.document.objects.size()<=p.maxObjects && m.meshes.size()==m.document.objects.size(),"Invalid cooked scene object count.");
        for(size_t i=0;i<m.meshes.size();++i)
        {
            const auto& o=m.document.objects[i]; const auto& binding=m.meshes[i];
            Require(MaterialReference(o.materialReference),"Unsupported material reference.");
            Require(binding.has_value()==!o.meshReference.empty(),"Missing cooked mesh binding.");
            if(!binding) { Require(o.materialSlotBindings.empty(),"Empty mesh has slot bindings."); continue; }
            const auto id=AssetID::ParseReference(o.meshReference);
            Require(id && id->ToReference()==o.meshReference,"Cooked scene mesh must be a canonical asset identity.");
            Require(binding->generation && binding->payload.formatVersion==CookedMesh::V1::Version &&
                binding->payload.cookCompatibility==CookedMesh::V1::CookCompatibility && binding->payload.payloadBytes>=CookedMesh::V1::HeaderBytes &&
                binding->payload.payloadBytes<=CookedMesh::Policy{}.maxPayloadBytes && binding->payload.payloadSHA256!=std::array<std::uint8_t,32>{},"Invalid cooked mesh generation/evidence.");
            Require(!binding->slots.empty() && binding->slots.size()<=p.maxSlots,"Invalid cooked mesh slot count.");
            std::set<std::string> slots;
            for(const auto& slot:binding->slots) Require(!slot.id.empty() && !slot.name.empty() && slots.insert(slot.id).second,"Invalid/duplicate cooked mesh slot.");
            for(const auto& [slot,ref]:o.materialSlotBindings) Require(slots.contains(slot) && AssetID::ParseReference(ref) && AssetID::ParseReference(ref)->ToReference()==ref,"Unknown slot or invalid material assignment.");
        }
    }
    std::vector<std::byte> Encode(const Metadata& m,const Policy& p)
    {
        Validate(m,p); Writer w{p,{}};
        for(size_t i=0;i<8;++i) w.U(static_cast<unsigned char>(Magic[i]),1);
        w.U(Version,4); w.U(Header,4); w.U(0,8); w.U(m.document.objects.size(),4); w.U(m.environment?1:0,4);
        const auto& d=m.document; w.Text(d.id); w.Text(d.name);
        w.V(d.camera.position); w.V(d.camera.target); w.F(d.camera.verticalFov); w.F(d.camera.aspectRatio); w.F(d.camera.nearPlane); w.F(d.camera.farPlane);
        w.V(d.directionalLight.direction); w.V(d.directionalLight.color); w.F(d.directionalLight.intensity); w.F(d.directionalLight.ambientStrength); w.F(d.directionalLight.angularRadius);
        for(size_t i=0;i<d.objects.size();++i)
        {
            const auto& o=d.objects[i]; w.U(UINT32_MAX,4); // Existing scene semantics: every object is a root, transform is world-space.
            w.Text(o.id); w.Text(o.name); w.V(o.transform.position); w.V(o.transform.rotation); w.V(o.transform.scale);
            w.Text(o.meshReference); w.Text(o.materialReference); w.U(o.materialSlotBindings.size(),4);
            for(const auto& [slot,ref]:o.materialSlotBindings) { w.Text(slot); w.Text(ref); }
            w.U(m.meshes[i]?1:0,4);
            if(const auto& mesh=m.meshes[i])
            {
                w.U(mesh->generation,8); w.U(mesh->payload.payloadBytes,8); w.U(mesh->payload.formatVersion,4); w.U(mesh->payload.cookCompatibility,4);
                for(auto b:mesh->payload.payloadSHA256) w.U(b,1);
                w.U(mesh->slots.size(),4);
                for(const auto& slot:mesh->slots) { w.Text(slot.id); w.Text(slot.name); w.U(slot.sourceMaterialIndex,4); }
            }
        }
        if(m.environment)
        { w.Text(d.environmentReference); w.U(m.environment->generation,8); for(auto b:m.environment->sha256) w.U(std::to_integer<unsigned>(b),1); }
        for(size_t i=0;i<8;++i) w.bytes[16+i]=std::byte((static_cast<std::uint64_t>(w.bytes.size())>>(8*i))&255);
        return std::move(w.bytes);
    }
}
std::vector<std::byte> Cook(const SceneDocument& document,std::span<const MeshInput> meshes,const ProjectAssetRegistry& registry,const Policy& p,const EnvironmentInput* environment)
{
    Require(meshes.size()<=p.maxObjects && document.objects.size()<=p.maxObjects && document.objects.size()<=UINT32_MAX,"Scene/input object count exceeds admission.");
    Metadata m{document,{}}; m.meshes.reserve(document.objects.size());
    std::map<std::string,const MeshInput*> inputs;
    for(const auto& mesh:meshes) Require(mesh.generation && inputs.emplace(mesh.id.ToString(),&mesh).second,"Duplicate or invalid prepared mesh input.");
    std::map<std::string,MeshBinding> resolved;
    const auto material=[&](const std::string& ref)
    {
        Require(MaterialReference(ref),"Unsupported authored material reference.");
        if(auto id=AssetID::ParseReference(ref)) { auto* r=registry.Find(*id); Require(r && r->type==AssetType::Material,"Unresolved authored material AssetID/type."); }
    };
    for(const auto& o:document.objects)
    {
        material(o.materialReference); for(const auto& [slot,ref]:o.materialSlotBindings) material(ref);
        if(o.meshReference.empty()) { m.meshes.emplace_back(); continue; }
        const auto id=AssetID::ParseReference(o.meshReference);
        Require(id && id->ToReference()==o.meshReference,"Unsupported mesh reference; prepared cooked AssetID required.");
        const auto input=inputs.find(id->ToString()); Require(input!=inputs.end(),"Unresolved cooked mesh reference.");
        auto found=resolved.find(id->ToString());
        if(found==resolved.end())
        {
            auto metadata=CookedMesh::DecodeMetadata(input->second->payload,{});
            Require(metadata.status==CookedMesh::Status::Success,"Incompatible or invalid prepared cooked mesh.");
            found=resolved.emplace(id->ToString(),MeshBinding{metadata.snapshot->evidence,input->second->generation,metadata.snapshot->metadata.materialSlots}).first;
        }
        m.meshes.push_back(found->second);
    }
    if(!document.environmentReference.empty())
    {
        const auto id=AssetID::ParseReference(document.environmentReference);
        const auto* record=id?registry.Find(*id):nullptr;
        Require(id && record && record->type==AssetType::Texture && environment && environment->id==*id,"Missing authored HDR environment input/type.");
        CookedEnvironment::Decode(environment->payload,*id,environment->generation);
        m.environment=EnvironmentBinding{environment->generation,CookedSurface::Hash(environment->payload)};
    }
    else Require(!environment,"Unexpected environment input for unassigned scene.");
    return Encode(m,p);
}
std::vector<std::byte> CookFile(const std::filesystem::path& file,std::span<const MeshInput> meshes,const ProjectAssetRegistry& registry,const Policy& p)
{
    SceneDocument d; std::string error;
    if(!LoadSceneDocumentForCooking(file,d,p.maxBytes,&error)) throw std::runtime_error("Scene cooker: " + error);
    return Cook(d,meshes,registry,p);
}
Metadata Decode(std::span<const std::byte> bytes,const Policy& p)
{
    Require(bytes.size()>=Header && bytes.size()<=p.maxBytes,"Cooked scene byte limit or truncated header.");
    Reader r{bytes,p}; for(size_t i=0;i<8;++i) Require(r.U(1)==static_cast<unsigned char>(Magic[i]),"Invalid cooked scene magic.");
    Require(r.U(4)==Version,"Unsupported cooked scene version."); Require(r.U(4)==Header && r.U(8)==bytes.size(),"Invalid cooked scene header/size.");
    const auto count=r.U(4); Require(count<=p.maxObjects && count<=(bytes.size()-Header)/64,"Invalid cooked scene count."); const auto flags=r.U(4); Require(flags<=1,"Unsupported cooked scene flags.");
    Metadata m; auto& d=m.document; d.id=r.Text(); d.name=r.Text();
    d.camera.position=r.V(); d.camera.target=r.V(); d.camera.verticalFov=r.F(); d.camera.aspectRatio=r.F(); d.camera.nearPlane=r.F(); d.camera.farPlane=r.F();
    d.directionalLight.direction=r.V(); d.directionalLight.color=r.V(); d.directionalLight.intensity=r.F(); d.directionalLight.ambientStrength=r.F(); d.directionalLight.angularRadius=r.F();
    d.objects.reserve(static_cast<size_t>(count)); m.meshes.reserve(static_cast<size_t>(count));
    for(size_t i=0;i<count;++i)
    {
        Require(r.U(4)==UINT32_MAX,"Unsupported non-root hierarchy; current runtime uses flat world transforms.");
        SceneObjectDocument o; o.id=r.Text(); o.name=r.Text(); o.transform.position=r.V(); o.transform.rotation=r.V(); o.transform.scale=r.V();
        o.meshReference=r.Text(); o.materialReference=r.Text();
        const auto bindings=r.U(4); Require(bindings<=p.maxSlots && bindings<=(bytes.size()-r.cursor)/8,"Invalid slot binding count.");
        std::string previous;
        for(size_t j=0;j<bindings;++j) { auto slot=r.Text(); auto ref=r.Text(); Require(slot>previous,"Unsorted/duplicate slot binding."); previous=slot; o.materialSlotBindings.emplace(std::move(slot),std::move(ref)); }
        const auto present=r.U(4); Require(present<=1,"Invalid mesh presence flag.");
        std::optional<MeshBinding> mesh;
        if(present)
        {
            mesh.emplace(); mesh->generation=r.U(8); mesh->payload.payloadBytes=r.U(8);
            const auto version=r.U(4); Require(version<=UINT16_MAX,"Invalid mesh payload version."); mesh->payload.formatVersion=static_cast<std::uint16_t>(version);
            mesh->payload.cookCompatibility=static_cast<std::uint32_t>(r.U(4)); for(auto& b:mesh->payload.payloadSHA256) b=static_cast<std::uint8_t>(r.U(1));
            const auto slots=r.U(4); Require(slots<=p.maxSlots && slots<=(bytes.size()-r.cursor)/12,"Invalid mesh slot count.");
            mesh->slots.reserve(static_cast<size_t>(slots));
            for(size_t j=0;j<slots;++j) { MeshMaterialSlot slot; slot.id=r.Text(); slot.name=r.Text(); slot.sourceMaterialIndex=static_cast<std::uint32_t>(r.U(4)); mesh->slots.push_back(std::move(slot)); }
        }
        d.objects.push_back(std::move(o)); m.meshes.push_back(std::move(mesh));
    }
    if(flags&1) { d.environmentReference=r.Text(); EnvironmentBinding env; env.generation=r.U(8); for(auto& b:env.sha256) b=std::byte(r.U(1)); Require(!d.environmentReference.empty() && env.generation,"Invalid environment binding."); m.environment=env; }
    Require(r.cursor==bytes.size(),"Trailing cooked scene bytes."); Validate(m,p); return m;
}
bool ArchiveScene::ValidateReferences(std::string* error) const
{
    const auto fail=[&](const char* message) { if(error) *error=message; return false; };
    if(metadata.meshes.size()!=metadata.document.objects.size()) return fail("Archive scene mesh/object count mismatch.");
    if(!provider || !generation) return fail("Archive scene ownership/generation missing.");
    const auto* scene=provider->ArchiveEntry(sceneAsset);
    if(!scene || scene->type!=EngineAssetPak::Type::Scene || scene->payloadVersion!=Version || scene->generation!=generation)
        return fail("Stale archive scene entry/type/version/generation.");
    if(!metadata.document.environmentReference.empty())
    {
        const auto id=AssetID::ParseReference(metadata.document.environmentReference);
        const auto* entry=id?provider->ArchiveEntry(*id):nullptr;
        if(!metadata.environment || !entry || entry->type!=EngineAssetPak::Type::Resource || entry->payloadVersion!=CookedEnvironment::Version ||
            entry->generation!=metadata.environment->generation || entry->sha256!=metadata.environment->sha256)
            return fail("Missing/stale authored environment entry, version, digest or generation.");
    }
    else if(metadata.environment) return fail("Unexpected environment binding.");
    for(size_t i=0;i<metadata.meshes.size();++i)
    {
        const auto& mesh=metadata.meshes[i]; if(!mesh) continue;
        const auto id=AssetID::ParseReference(metadata.document.objects[i].meshReference);
        if(!id) return fail("Invalid archive scene mesh identity.");
        const auto found=provider->Lookup(*id);
        if(found.status!=CookedMeshSource::Status::Success || found.evidence->payload!=mesh->payload || found.evidence->contentGeneration!=mesh->generation)
            return fail("Missing/incompatible archive mesh identity, payload or content generation.");
    }
    return true;
}
std::shared_ptr<const ArchiveScene> OpenArchive(const std::filesystem::path& toc,const std::filesystem::path& pak,const AssetID& id,const Policy& p)
{
    const auto opened=CookedMeshSource::ArchiveProvider::OpenArchive(toc,pak);
    Require(bool(opened.provider),"Cannot open cooked scene archive pair.");
    const auto* entry=opened.provider->ArchiveEntry(id);
    Require(entry && entry->type==EngineAssetPak::Type::Scene && entry->payloadVersion==Version,"Missing or unsupported cooked scene entry.");
    auto bytes=opened.provider->ReadEntry(id,EngineAssetPak::Type::Scene,Version,p.maxBytes);
    auto result=std::shared_ptr<const ArchiveScene>(new ArchiveScene(id,entry->generation,opened.provider,Decode(bytes,p)));
    std::string error; if(!result->ValidateReferences(&error)) throw std::runtime_error("Cooked scene reference validation: "+error); return result;
}
}

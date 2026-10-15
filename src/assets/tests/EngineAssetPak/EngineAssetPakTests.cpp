#include "assets/EngineAssetPak.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace EngineAssetPak;
namespace fs = std::filesystem;
void Check(bool ok) { if(!ok) throw std::runtime_error("Test assertion failed."); }
std::vector<std::byte> Load(const fs::path& p)
{
    std::ifstream f(p,std::ios::binary | std::ios::ate); Check(bool(f));
    std::vector<std::byte> b(static_cast<size_t>(f.tellg())); f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()),b.size()); Check(bool(f)); return b;
}
void Save(const fs::path& p, const std::vector<std::byte>& b)
{
    std::ofstream f(p,std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(b.data()),b.size()); Check(bool(f));
}
template<class F> void Reject(F f)
{
    bool failed = false; try { f(); } catch(const std::runtime_error&) { failed = true; } Check(failed);
}
void Set(std::vector<std::byte>& b,size_t o,std::uint64_t v,size_t n)
{ for(size_t i=0;i<n;++i) b[o+i]=std::byte((v>>(8*i))&255); }
void Resign(std::vector<std::byte>& toc,std::vector<std::byte>& pak)
{
    std::fill_n(toc.begin()+32,32,std::byte{});
    BCRYPT_ALG_HANDLE h{}; Check(BCryptOpenAlgorithmProvider(&h,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0);
    Digest d{}; auto status = BCryptHash(h,nullptr,0,reinterpret_cast<PUCHAR>(toc.data()),static_cast<ULONG>(toc.size()),reinterpret_cast<PUCHAR>(d.data()),32);
    BCryptCloseAlgorithmProvider(h,0); Check(status>=0);
    std::copy(d.begin(),d.end(),toc.begin()+32); std::copy(d.begin(),d.end(),pak.begin()+32);
}
int main()
{
    // Unique test directory, no recursive cleanup of caller-owned directories.
    fs::path root = fs::current_path() / ("pak-verification-"+std::to_string(GetCurrentProcessId()));
    try
    {
        Check(fs::create_directory(root));
        const auto first = *AssetID::Parse("00000000-0000-0000-0000-000000000001");
        const auto second = *AssetID::Parse("00000000-0000-0000-0000-000000000002");
        std::vector<std::byte> a{std::byte{1},std::byte{2},std::byte{3}}, b{std::byte{9}};
        std::vector<PreparedAsset> inputs{{second,Type::Texture,7,12,b,64},{first,Type::Mesh,1,4,a,16}};
        Write(root/"a.toc",root/"a.eap",inputs);
        {
            auto reader = Reader::Open(root/"a.toc",root/"a.eap");
            Check(reader.Entries().size()==2 && reader.Entries()[0].id==first);
            Check(reader.Lookup(second)->generation==12 && reader.Lookup(second)->payloadVersion==7);
            Check(reader.Read(first)==a && reader.Read(second)==b);
            auto shared = reader; Check(shared.Read(first)==a);
        }
        std::reverse(inputs.begin(),inputs.end()); Write(root/"b.toc",root/"b.eap",inputs);
        auto toc=Load(root/"a.toc"), pak=Load(root/"a.eap");
        Check(toc==Load(root/"b.toc") && pak==Load(root/"b.eap"));
        auto malformed = [&](auto edit,bool resign=true)
        {
            auto t=toc,p=pak; edit(t,p); if(resign) Resign(t,p);
            Save(root/"bad.toc",t); Save(root/"bad.eap",p);
            Reject([&]{ Reader::Open(root/"bad.toc",root/"bad.eap"); });
        };
        malformed([](auto& t,auto&){t[0]=std::byte{};});
        malformed([](auto& t,auto&){Set(t,8,2,4);});
        malformed([](auto& t,auto&){Set(t,24,UINT64_MAX,8);});
        malformed([](auto& t,auto&){t.pop_back();});
        malformed([](auto& t,auto&){Set(t,64+56,UINT64_MAX,8);});
        malformed([](auto& t,auto&){Set(t,192+56,64,8);});
        malformed([](auto& t,auto&){Set(t,64+44,3,4);});
        malformed([](auto& t,auto&){std::copy_n(t.begin()+64,36,t.begin()+192);});
        malformed([](auto& t,auto&){Set(t,64+36,99,4);});
        malformed([](auto& t,auto&){Set(t,64+72,4,8);});
        malformed([](auto& t,auto&){Set(t,64+64,UINT64_MAX,8);});
        malformed([](auto& t,auto&){Set(t,64+48,0,8);});
        malformed([](auto& t,auto&){Set(t,64+112,1,4);});
        malformed([](auto& t,auto&){t[64]=std::byte{'z'};});
        malformed([](auto& t,auto&){std::fill_n(t.begin()+32,32,std::byte{});},false);
        malformed([](auto&,auto& p){p[32]^=std::byte{1};},false);
        malformed([](auto&,auto& p){p.pop_back();});
        malformed([](auto& t,auto&){t[80]^=std::byte{1};},false);
        auto corrupt=pak; corrupt[64]^=std::byte{1}; Save(root/"bad.eap",corrupt);
        {
            auto r=Reader::Open(root/"a.toc",root/"bad.eap"); Reject([&]{r.Read(first);});
        }
        Limits limits; limits.entries=1; Reject([&]{Reader::Open(root/"a.toc",root/"a.eap",limits);});
        limits={}; limits.tocBytes=64; Reject([&]{Reader::Open(root/"a.toc",root/"a.eap",limits);});
        limits={}; limits.payloadBytes=2; Reject([&]{Reader::Open(root/"a.toc",root/"a.eap",limits);});
        limits={}; limits.archiveBytes=64; Reject([&]{Reader::Open(root/"a.toc",root/"a.eap",limits);});
        limits={}; limits.readBytes=2;
        {auto r=Reader::Open(root/"a.toc",root/"a.eap",limits); Reject([&]{r.Read(first);});}
        inputs.push_back(inputs[0]); Reject([&]{Write(root/"x.toc",root/"x.eap",inputs);}); inputs.pop_back();
        Write(root/"empty.toc",root/"empty.eap",{});
        {auto r=Reader::Open(root/"empty.toc",root/"empty.eap"); Check(r.Entries().empty());}
        inputs[0].bytes={}; Write(root/"zero.toc",root/"zero.eap",inputs);
        {auto r=Reader::Open(root/"zero.toc",root/"zero.eap"); Check(r.Read(first).empty());}
        // Mismatched complete generations must fail before enumeration.
        Reject([&]{Reader::Open(root/"a.toc",root/"zero.eap");});
        for(const auto& file : fs::directory_iterator(root)) fs::remove(file.path()); fs::remove(root);
        std::cout << "Archive round-trip, determinism, corruption, bounds and empty-payload checks passed.\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<" (artifacts: "<<root<<")\n"; return 1; }
}

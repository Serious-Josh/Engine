#include "ShaderLibrary.h"
#include "Core/Log.h"
#include <stdexcept>
#include <cstring>
#include <array>
#include <cstdint>

bool ShaderLibrary::Initialize(const std::filesystem::path& shaderRoot)
{
    m_packaged = false;
    m_shaderRoot = shaderRoot;

    if (!m_compiler.Initialize())
    {
        return false;
    }

    return true;
}

void ShaderLibrary::Shutdown()
{
    for (auto& [key, shader] : m_shaders)
    {
        shader->Unload();
    }

    m_shaders.clear();
    m_packagedBytecode.clear();

    m_compiler.Shutdown();
}

Shader* ShaderLibrary::Load(const ShaderDescription& description)
{
    if (!description.path || !description.entryPoint)
    {
        return nullptr;
    }

    ShaderKey key{};
    key.path = description.path;

    key.entryPoint = description.entryPoint;
    key.stage = description.stage;

    auto existingShader = m_shaders.find(key);

    if (existingShader != m_shaders.end())
    {
        return existingShader->second.get();
    }

    const std::filesystem::path shaderPath = m_shaderRoot / description.path;

    const std::wstring shaderPathString = shaderPath.wstring();
    const std::wstring entryPointString = std::filesystem::path(description.entryPoint).wstring();

    ShaderCompileDescription compileDescription{};
    compileDescription.path = shaderPathString.c_str();
    compileDescription.entryPoint = entryPointString.c_str();
    compileDescription.stage = description.stage;

    ShaderCompileResult compileResult{};

    if (m_packaged)
    {
        const auto found = m_packagedBytecode.find(key.path + ":" + key.entryPoint + ":" + std::to_string(static_cast<int>(key.stage)));
        if(found == m_packagedBytecode.end()) { Log::Error("Missing packaged shader: " + key.path + ":" + key.entryPoint); return nullptr; }
        compileResult.bytecode = found->second;
    }
    else if (!m_compiler.Compile(compileDescription, compileResult))
    {
        return nullptr;
    }

    std::unique_ptr<Shader> shader = std::make_unique<Shader>();

    if (!shader->Initialize(compileResult.bytecode.data(), compileResult.bytecode.size()))
    {
        return nullptr;
    }

    Shader* shaderPointer = shader.get();

    m_shaders.emplace(std::move(key), std::move(shader));

    return shaderPointer;
}

bool ShaderLibrary::CompileLibrary(
    const std::filesystem::path& path,
    ShaderLibraryCompileResult& output)
{
    if(m_packaged)
    {
        const auto found = m_packagedBytecode.find(path.generic_string() + ":library");
        if(found == m_packagedBytecode.end()) { Log::Error("Missing packaged DXR library: " + path.generic_string()); return false; }
        output.bytecode = found->second; return true;
    }
    const std::filesystem::path shaderPath = m_shaderRoot / path;
    const std::wstring shaderPathString = shaderPath.wstring();

    ShaderLibraryCompileDescription description{};
    description.path = shaderPathString.c_str();

    return m_compiler.CompileLibrary(description, output);
}

namespace
{
    // Names are logical keys, never runtime file paths. Update this manifest when adding a pass.
    constexpr const char* raster[] = {"BRDFIntegration", "DeferredLighting", "DiffuseIrradiance", "EnvironmentMip", "EquirectangularToCubemap", "GGXAverageAlbedo", "GGXMultiscatterIrradiance", "SelectionMask", "ToneMap", "Triangle"};
    constexpr const char* compute[] = {"AODenoise", "AOResolve", "AOTemporal", "DisplacementIndices", "DisplacementVertices", "GISignedCorrection", "GISignedReconstruct", "GISpatialDenoise", "NrdDiffuseResolve", "NrdGIPrepare", "NrdReflectionPrepare", "NrdReflectionResolve", "RayTracedReflectionsDenoise", "RayTracedReflectionTemporal", "ReflectionContributionLimit", "ShadowDenoise"};
    constexpr const char* libraries[] = {"RayTracedAO", "RayTracedGI", "RayTracedReflection", "RayTracedShadow"};
    void Put(std::vector<std::byte>& out, std::uint32_t value) { for(int i=0;i<4;++i) out.push_back(std::byte(value>>(8*i))); }
}
std::vector<std::byte> ShaderLibrary::CookBootstrap(const std::filesystem::path& root)
{
    ShaderCompiler compiler;
    if(!compiler.Initialize()) throw std::runtime_error("Cannot initialize offline DXC.");
    struct Cleanup { ShaderCompiler& c; ~Cleanup(){c.Shutdown();} } cleanup{compiler};
    std::map<std::string,std::vector<char>> entries;
    const auto stage = [&](std::string file, const char* entry, ShaderStage kind)
    {
        file += ".hlsl"; const auto path=(root/file).wstring(); const auto name=std::filesystem::path(entry).wstring();
        ShaderCompileResult result;
        if(!compiler.Compile({path.c_str(),name.c_str(),kind},result)) throw std::runtime_error("Offline shader compilation failed: "+file+":"+entry);
        entries.emplace(file+":"+entry+":"+std::to_string(static_cast<int>(kind)),std::move(result.bytecode));
    };
    for(auto file:raster) { stage(file,"VSMain",ShaderStage::Vertex); stage(file,"PSMain",ShaderStage::Pixel); }
    for(auto file:compute) stage(file,"CSMain",ShaderStage::Compute);
    for(auto entry:{"SphereVS","LightingVS"}) stage("MaterialPreview",entry,ShaderStage::Vertex);
    for(auto entry:{"SpherePS","LightingPS"}) stage("MaterialPreview",entry,ShaderStage::Pixel);
    for(auto name:libraries)
    {
        const std::string file=std::string(name)+".hlsl"; const auto path=(root/file).wstring(); ShaderLibraryCompileResult result;
        if(!compiler.CompileLibrary({path.c_str()},result)) throw std::runtime_error("Offline DXR compilation failed: "+file);
        entries.emplace(file+":library",std::move(result.bytecode));
    }
    std::vector<std::byte> out; Put(out,0x31424853); Put(out,1); Put(out,static_cast<std::uint32_t>(entries.size()));
    for(const auto& [key,bytes]:entries)
    {
        Put(out,static_cast<std::uint32_t>(key.size())); Put(out,static_cast<std::uint32_t>(bytes.size()));
        for(auto c:key) out.push_back(std::byte(c));
        for(auto c:bytes) out.push_back(std::byte(c));
    }
    return out;
}
bool ShaderLibrary::InitializePackaged(std::span<const std::byte> payload)
{
    m_packaged = true; m_shaderRoot.clear(); m_packagedBytecode.clear();
    try
    {
        if(payload.size()>64*1024*1024) throw std::runtime_error("Shader bundle exceeds 64 MiB.");
        size_t cursor=0;
        const auto get=[&]() { if(payload.size()-cursor<4) throw std::runtime_error("Truncated shader bundle."); std::uint32_t v=0; for(int i=0;i<4;++i) v|=std::uint32_t(payload[cursor++])<<(8*i); return v; };
        if(get()!=0x31424853 || get()!=1) throw std::runtime_error("Unsupported shader bootstrap format.");
        const auto count=get(); if(count==0 || count>256) throw std::runtime_error("Invalid shader count.");
        for(std::uint32_t i=0;i<count;++i)
        {
            const auto name=get(), bytes=get();
            if(name==0 || name>256 || bytes<4 || bytes>16*1024*1024 || std::uint64_t(name)+bytes>payload.size()-cursor) throw std::runtime_error("Invalid shader record.");
            std::string key(reinterpret_cast<const char*>(payload.data()+cursor),name); cursor+=name;
            const auto* begin=reinterpret_cast<const char*>(payload.data()+cursor);
            if(std::memcmp(begin,"DXBC",4)!=0 || !m_packagedBytecode.emplace(key,std::vector<char>(begin,begin+bytes)).second) throw std::runtime_error("Invalid/duplicate DXIL record.");
            cursor+=bytes;
        }
        if(cursor!=payload.size()) throw std::runtime_error("Trailing shader bundle bytes.");
        return true;
    }
    catch(const std::exception& e) { Log::Error(std::string("Packaged shader bootstrap: ")+e.what()); m_packagedBytecode.clear(); return false; }
}

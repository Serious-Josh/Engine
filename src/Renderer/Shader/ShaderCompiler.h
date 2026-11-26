#pragma once

#include "Shader.h"

#include <Windows.h>
#include <dxcapi.h>
#include <filesystem>
#include <string>
#include <vector>

struct ShaderCompileDescription
{
    const wchar_t* path = nullptr;
    const wchar_t* entryPoint = nullptr;
    ShaderStage stage = ShaderStage::Vertex;
    // M0c.5 Stage 5.4: runtime ShaderLibrary instances opt in to the persistent DXIL cache (DxilLibraryCache), keyed relative
    // to this shader root. Offline cooking and direct compiler clients stay uncached.
    const wchar_t* shaderRoot = nullptr;
    bool usePersistentCache = false;
};

struct ShaderCompileResult
{
    std::vector<char> bytecode;
    bool cacheHit = false;
    bool dxcInvoked = false;
    bool cacheWritten = false;
    double cacheReadMilliseconds = 0.0;
    double dxcMilliseconds = 0.0;
};

// The DXIL library target. Every library keeps lib_6_6 unless ShaderCompiler::GetLibraryProfile maps its
// path to another profile.
enum class ShaderLibraryProfile
{
    Library66,
    // The experimental SM 6.9 wrappers only (SER S1; the SER variant from S3A).
    Library69,
    // RayQuery 2M E3: a path-tracing compute shader (cs_6_6, entry ShaderCompiler::ComputeLibraryEntryPoint) handled like
    // a library: DirectLighting/, the ReSTIR DI direct pass. Its bytecode is a compute shader, not a DXIL library.
    Compute66
};

struct ShaderLibraryCompileDescription
{
    const wchar_t* path = nullptr;
    ShaderLibraryProfile profile = ShaderLibraryProfile::Library66;
    // The shader root. Added as a second -I only when it differs from the source's directory, so variants in
    // subdirectories (GI6/, PsrK2/) resolve the RTXDI SDK's angled includes like their originals. Also where
    // ShaderVariants looks up a path that names a variant rather than a file.
    const wchar_t* includeDirectory = nullptr;
    // Runtime callers opt in explicitly. Offline cooking and direct compiler clients remain uncached.
    bool usePersistentCache = false;
};

// One source file a compilation read, or an include lookup that found no file. Together with the profile
// and the fixed compiler arguments, the dependencies of a compilation are every input that decides its
// bytecode.
struct ShaderSourceDependency
{
    std::filesystem::path path;
    bool exists = false;
    std::string contents;
};

struct ShaderLibraryCompileResult
{
    enum class CacheStatus
    {
        Disabled,
        Miss,
        Hit
    };
    CacheStatus cacheStatus = CacheStatus::Disabled;
    bool cacheWritten = false;
    bool dxcInvoked = false;
    double cacheReadMilliseconds = 0.0;
    double dxcMilliseconds = 0.0;
    double cacheWriteMilliseconds = 0.0;
    std::vector<char> bytecode;
    // The profile the bytecode was compiled for; reuse requires it to match the library's mapped profile.
    ShaderLibraryProfile profile = ShaderLibraryProfile::Library66;
    // Empty for packaged bytecode, which cannot change during a session.
    std::vector<ShaderSourceDependency> dependencies;
};

class ShaderCompiler
{
    public:
    ShaderCompiler() = default;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    // Creates the compiler and requires the loaded dxcompiler.dll to be the pinned DXC release. A failure is
    // logged; the second form returns it in diagnostics instead, for a worker thread.
    bool Initialize();
    bool Initialize(std::string& diagnostics);
    void Shutdown();

    bool Compile(
        const ShaderCompileDescription& description,
        ShaderCompileResult& output);
    bool CompileLibrary(
        const ShaderLibraryCompileDescription& description,
        ShaderLibraryCompileResult& output);
    // Identical compilation and bytecode, but every error message is returned in diagnostics instead of
    // logged, so a worker thread can compile without touching Engine logging.
    bool CompileLibrary(
        const ShaderLibraryCompileDescription& description,
        ShaderLibraryCompileResult& output,
        std::string& diagnostics);

    // True when every recorded dependency still has the recorded contents (or is still absent).
    static bool DependenciesUnchanged(const std::vector<ShaderSourceDependency>& dependencies);

    // The one mapping from a library's key (for example "PathTraceReferenceSM69.hlsl") to its profile, used
    // by synchronous, asynchronous and offline compilation alike. A profile is a pure function of the key, so
    // path-keyed identity (cache entries, worker jobs, cooked records) never mixes profiles.
    static ShaderLibraryProfile GetLibraryProfile(const std::filesystem::path& libraryPath);

    // The DXC target of a library profile ("lib_6_6", "lib_6_9" or "cs_6_6").
    static const char* GetLibraryTarget(ShaderLibraryProfile profile);

    // The entry point of every Compute66 "library" (DirectLighting/ shaders).
    static constexpr const wchar_t* ComputeLibraryEntryPoint = L"PathTraceReSTIRDIDirectCS";

    // NVIDIA SHARC (Documentation/Rendering/NVIDIA_SHARC_Integration_A.md): shaders under a SharcNv/ directory compile with
    // -enable-16bit-types, which SharcTypes.h requires, and so do ReSTIR PT's SHARC twins (ReSTIRPT/...Sharc.hlsl, the
    // kernels with the SHARC radiance cache at x3 compiled in). Like the profile, a pure function of the path; every other
    // shader keeps its arguments, so no other DXIL changes.
    static bool UsesNative16BitTypes(const std::filesystem::path& shaderPath);

    // Cooked-game port (2026-11-26): an offline cook (ShaderLibrary::CookBootstrap) compiles the production arguments only.
    // While a scope lives, this thread's compilations ignore the ReSTIRPT/ tree's development environment switches below
    // (and ENGINE_RESTIR_PT_LIST_PROTOTYPE), so a packaged game's DXIL never depends on the Editor's environment.
    class ProductionArgumentsScope
    {
        public:
        ProductionArgumentsScope();
        ~ProductionArgumentsScope();
        ProductionArgumentsScope(const ProductionArgumentsScope&) = delete;
        ProductionArgumentsScope& operator=(const ProductionArgumentsScope&) = delete;

        private:
        bool m_previous = false;
    };
    // M5.1 T-2.1 (ReSTIR PT Enhanced, configuration B): shaders under a ReSTIRPT/ directory may compile with -Gis (strict
    // IEEE: no reassociation or contraction), so a shift evaluates exactly what capture cached. Since optimization A1 this
    // applies only with ENGINE_RESTIR_PT_STRICT_FP=1 (default relaxed). Every other shader keeps its arguments.
    static bool UsesStrictFloatingPoint(const std::filesystem::path& shaderPath);
    // M5.6 B3a: ReSTIRPT/ shaders compile with RESTIR_PT_UNFUSED_BSDF (pre-B3a evaluation) unless ENGINE_RESTIR_PT_FUSED_BSDF=1.
    static bool UsesUnfusedBsdf(const std::filesystem::path& shaderPath);
    // Audit T1-c: ReSTIRPT/ shaders compile their diagnostic counters only for ENGINE_RESTIR_PT_COUNTERS=1 or
    // ENGINE_RESTIR_PT_DIAGNOSTIC_COUNTERS=1.
    static bool UsesDiagnosticCounters(const std::filesystem::path& shaderPath);
    // Audit T1-b: ReSTIRPT/ shaders compile with RESTIR_PT_NO_SMALL_LIGHTS (no small-light NEE path) only for
    // ENGINE_RESTIR_PT_SMALL_LIGHTS=0.
    static bool DisablesSmallLightPath(const std::filesystem::path& shaderPath);

    private:
    // Logs through Engine when diagnostics is null; otherwise appends to it.
    bool Initialize(std::string* diagnostics);

    // Logs through Engine when diagnostics is null; otherwise appends to it.
    bool CompileLibrary(
        const ShaderLibraryCompileDescription& description,
        ShaderLibraryCompileResult& output,
        std::string* diagnostics);

    const wchar_t* GetShaderProfile(ShaderStage stage) const;

    IDxcUtils* m_utils = nullptr;

    IDxcCompiler3* m_compiler = nullptr;
};

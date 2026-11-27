#include "Renderer/PipelineStateCache.h"
#include "Platform/Windows/AtomicFileWrite.h"

#include <bcrypt.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string_view>

namespace
{
    using Microsoft::WRL::ComPtr;
    using Digest = std::array<unsigned char, 32>;
    constexpr char Magic[4] = {'E', 'P', 'S', 'O'};
    constexpr std::uint32_t FormatVersion = 1;
    constexpr std::size_t MaximumBlobBytes = 64 * 1024 * 1024;
    constexpr std::size_t HeaderBytes = sizeof(Magic) + 4 + 32 + 4 + 32;

    // SHA-256 over several byte ranges; false when the provider is unavailable (the cache is then not used).
    bool Hash(std::initializer_list<std::string_view> parts, Digest& digest)
    {
        BCRYPT_HASH_HANDLE hash = nullptr;

        if(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            return false;
        }

        bool hashed = true;

        for(const std::string_view part : parts)
        {
            if(part.size() > (std::numeric_limits<ULONG>::max)() ||
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(part.data())), static_cast<ULONG>(part.size()),
                    0) < 0)
            {
                hashed = false;
                break;
            }
        }

        hashed = hashed && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
        BCryptDestroyHash(hash);
        return hashed;
    }

    std::string Hex(const Digest& digest)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string text;

        for(const unsigned char byte : digest)
        {
            text.push_back(digits[byte >> 4]);
            text.push_back(digits[byte & 15]);
        }

        return text;
    }

    void PutInteger(std::string& bytes, std::uint32_t value)
    {
        for(unsigned int shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<char>(value >> shift));
        }
    }

    std::uint32_t GetInteger(const char* bytes)
    {
        std::uint32_t value = 0;

        for(unsigned int index = 0; index < 4; ++index)
        {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index])) << (8 * index);
        }

        return value;
    }

    // The cache directory: ENGINE_PSO_CACHE_DIR, "off" for none, by default %LOCALAPPDATA%\Engine\ShaderCache\PSO\v1. Read
    // once per process.
    const std::filesystem::path& CacheDirectory()
    {
        static const std::filesystem::path directory = []() -> std::filesystem::path
        {
            wchar_t value[MAX_PATH]{};
            DWORD length = GetEnvironmentVariableW(L"ENGINE_PSO_CACHE_DIR", value, MAX_PATH);

            if(length > 0 && length < MAX_PATH)
            {
                if(_wcsicmp(value, L"off") == 0)
                {
                    return {};
                }

                return std::filesystem::path(value);
            }

            length = GetEnvironmentVariableW(L"LOCALAPPDATA", value, MAX_PATH);

            if(length == 0 || length >= MAX_PATH)
            {
                return {};
            }

            return std::filesystem::path(value) / L"Engine/ShaderCache/PSO/v1";
        }();
        return directory;
    }

    // The adapter and driver part of every key: vendor, device, subsystem and revision IDs and the user-mode driver version,
    // looked up once per adapter LUID (a recreated device on the same adapter reuses it). Empty when they cannot be read
    // (the cache is then not used, so a blob never meets a driver it was not keyed for).
    std::string AdapterIdentity(ID3D12Device* device)
    {
        static std::mutex mutex;
        static LUID cachedLuid{};
        static std::string cachedIdentity;
        static bool cached = false;
        const LUID luid = device->GetAdapterLuid();
        const std::lock_guard<std::mutex> lock(mutex);

        if(cached && luid.LowPart == cachedLuid.LowPart && luid.HighPart == cachedLuid.HighPart)
        {
            return cachedIdentity;
        }

        std::string identity;
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 description{};
        LARGE_INTEGER driverVersion{};

        if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && SUCCEEDED(adapter->GetDesc1(&description)) &&
            SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driverVersion)))
        {
            for(const std::uint32_t value : {static_cast<std::uint32_t>(description.VendorId),
                static_cast<std::uint32_t>(description.DeviceId), static_cast<std::uint32_t>(description.SubSysId),
                static_cast<std::uint32_t>(description.Revision), static_cast<std::uint32_t>(driverVersion.LowPart),
                static_cast<std::uint32_t>(driverVersion.HighPart)})
            {
                PutInteger(identity, value);
            }
        }

        cachedLuid = luid;
        cachedIdentity = identity;
        cached = true;
        return identity;
    }

    // An entry: "EPSO", format, key digest, blob size, blob digest, blob. The blob only when every field checks out.
    bool ReadEntry(const std::filesystem::path& file, const Digest& key, std::string& blob)
    {
        std::ifstream stream(file, std::ios::binary | std::ios::ate);

        if(!stream)
        {
            return false;
        }

        const std::streamoff size = stream.tellg();

        if(size < static_cast<std::streamoff>(HeaderBytes) ||
            static_cast<std::uint64_t>(size) > HeaderBytes + MaximumBlobBytes)
        {
            return false;
        }

        std::string bytes(static_cast<std::size_t>(size), '\0');
        stream.seekg(0);

        if(!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        {
            return false;
        }

        const char* cursor = bytes.data();

        if(std::memcmp(cursor, Magic, sizeof(Magic)) != 0 || GetInteger(cursor + 4) != FormatVersion ||
            std::memcmp(cursor + 8, key.data(), key.size()) != 0)
        {
            return false;
        }

        const std::uint32_t blobSize = GetInteger(cursor + 40);

        if(blobSize == 0 || HeaderBytes + blobSize != bytes.size())
        {
            return false;
        }

        const std::string_view stored(bytes.data() + HeaderBytes, blobSize);
        Digest digest{};

        if(!Hash({stored}, digest) || std::memcmp(cursor + 44, digest.data(), digest.size()) != 0)
        {
            return false;
        }

        blob.assign(stored);
        return true;
    }

    bool WriteEntry(const std::filesystem::path& file, const Digest& key, ID3D12PipelineState* pipelineState)
    {
        ComPtr<ID3DBlob> cachedBlob;

        if(FAILED(pipelineState->GetCachedBlob(&cachedBlob)) || !cachedBlob || cachedBlob->GetBufferSize() == 0 ||
            cachedBlob->GetBufferSize() > MaximumBlobBytes)
        {
            return false;
        }

        const std::string_view blob(static_cast<const char*>(cachedBlob->GetBufferPointer()), cachedBlob->GetBufferSize());
        Digest digest{};

        if(!Hash({blob}, digest))
        {
            return false;
        }

        std::string bytes(Magic, sizeof(Magic));
        PutInteger(bytes, FormatVersion);
        bytes.append(reinterpret_cast<const char*>(key.data()), key.size());
        PutInteger(bytes, static_cast<std::uint32_t>(blob.size()));
        bytes.append(reinterpret_cast<const char*>(digest.data()), digest.size());
        bytes.append(blob);

        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        std::string failure;
        return AtomicFileWrite::Write(file, bytes, failure);
    }
}

HRESULT PipelineStateCache::CreateComputePipelineState(ID3D12Device* device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC& description, const void* rootSignature, std::size_t rootSignatureSize,
    ID3D12PipelineState** pipelineState, Outcome* outcome)
{
    Outcome result{};
    const std::filesystem::path& directory = CacheDirectory();
    std::filesystem::path file;
    Digest key{};

    // The key: format, adapter and driver, root signature, DXIL (each length-prefixed so no two inputs run together).
    if(!directory.empty() && rootSignature && rootSignatureSize > 0 && description.CS.pShaderBytecode &&
        description.CS.BytecodeLength > 0 && !description.CachedPSO.pCachedBlob)
    {
        const std::string adapter = AdapterIdentity(device);
        std::string lengths;
        PutInteger(lengths, FormatVersion);
        PutInteger(lengths, static_cast<std::uint32_t>(adapter.size()));
        PutInteger(lengths, static_cast<std::uint32_t>(rootSignatureSize));
        PutInteger(lengths, static_cast<std::uint32_t>(description.CS.BytecodeLength));
        PutInteger(lengths, description.NodeMask);
        PutInteger(lengths, static_cast<std::uint32_t>(description.Flags));

        if(!adapter.empty() && Hash({lengths, adapter, std::string_view(static_cast<const char*>(rootSignature),
            rootSignatureSize), std::string_view(static_cast<const char*>(description.CS.pShaderBytecode),
            description.CS.BytecodeLength)}, key))
        {
            file = directory / (Hex(key) + ".pso");
        }
    }

    HRESULT created = E_FAIL;
    std::string blob;

    if(!file.empty() && ReadEntry(file, key, blob))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cached = description;
        cached.CachedPSO.pCachedBlob = blob.data();
        cached.CachedPSO.CachedBlobSizeInBytes = blob.size();
        created = device->CreateComputePipelineState(&cached, IID_PPV_ARGS(pipelineState));
        result.hit = SUCCEEDED(created);
        result.rejected = FAILED(created);
    }

    if(!result.hit)
    {
        created = device->CreateComputePipelineState(&description, IID_PPV_ARGS(pipelineState));

        if(SUCCEEDED(created) && !file.empty())
        {
            result.stored = WriteEntry(file, key, *pipelineState);
        }
    }

    if(outcome)
    {
        *outcome = result;
    }

    return created;
}

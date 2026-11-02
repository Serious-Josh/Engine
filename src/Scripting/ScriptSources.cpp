#include "Scripting/ScriptSources.h"
#include "Platform/Windows/WindowsText.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <cstdint>

namespace Scripting
{
namespace
{
void Require(bool condition, const char* message)
{
    if(!condition)
    {
        throw std::runtime_error(message);
    }
}
}

std::string CanonicalScriptSection(std::string_view name)
{
    Require(!name.empty() && name.size() <= 1024 && name.find('\0') == std::string_view::npos,
        "Invalid script section length/encoding.");
    const auto wide = WindowsText::FromUTF8(name);
    Require(name.find_first_of("\\:<>\"|?*") == std::string_view::npos &&
        name.starts_with("Content/Scripts/") &&
        std::none_of(name.begin(), name.end(), [](unsigned char value) { return value < 32; }),
        "Scripts must use canonical project-relative Content/Scripts paths.");
    const std::filesystem::path path(wide);

    for(const auto& part : path)
    {
        const auto value = part.native();
        Require(!value.empty() && value != L"." && value != L".." &&
            value.back() != L'.' && value.back() != L' ', "Script traversal or path alias rejected.");
        const auto stem = value.substr(0, value.find(L'.'));
        const bool device = _wcsicmp(stem.c_str(), L"CON") == 0 || _wcsicmp(stem.c_str(), L"PRN") == 0 ||
            _wcsicmp(stem.c_str(), L"AUX") == 0 || _wcsicmp(stem.c_str(), L"NUL") == 0 ||
            (stem.size() == 4 && (stem[3] >= L'1' && stem[3] <= L'9') &&
                (_wcsnicmp(stem.c_str(), L"COM", 3) == 0 || _wcsnicmp(stem.c_str(), L"LPT", 3) == 0));
        Require(!device, "Windows device-name script alias rejected.");
    }

    Require(name.find("//") == std::string_view::npos &&
        _wcsicmp(path.extension().c_str(), L".as") == 0, "Expected an ordinary .as section.");
    return std::string(name);
}

std::string ResolveScriptInclude(std::string_view from, std::string_view include)
{
    Require(!include.empty() && include.front() != '/' && include.find(':') == std::string_view::npos,
        "Absolute script includes are forbidden.");
    const auto slash = from.find_last_of('/');
    Require(slash != std::string_view::npos, "Include has no canonical parent section.");
    return CanonicalScriptSection(std::string(from.substr(0, slash + 1)) + std::string(include));
}

bool ScriptNamesEqual(std::string_view left, std::string_view right)
{
    const auto wideLeft = WindowsText::FromUTF8(left);
    const auto wideRight = WindowsText::FromUTF8(right);
    return CompareStringOrdinal(wideLeft.c_str(), -1, wideRight.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::string SourceClosureRevision(const SourceSnapshot& snapshot)
{
    Require(snapshot.sections.size() <= 64 && snapshot.rootSection.size() <= 1024 && snapshot.rootAsset.size() <= 256,
        "Invalid snapshot hash input bounds.");
    std::size_t totalBytes = 0;

    for(const auto& section : snapshot.sections)
    {
        Require(section.name.size() <= 1024 && section.assetReference.size() <= 256 &&
            section.bytes.size() <= 1024 * 1024 && section.bytes.size() <= 8 * 1024 * 1024 - totalBytes,
            "Invalid snapshot source bounds.");
        totalBytes += section.bytes.size();
    }

    struct HashOwner
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        ~HashOwner()
        {
            if(hash)
            {
                BCryptDestroyHash(hash);
            }

            if(algorithm)
            {
                BCryptCloseAlgorithmProvider(algorithm, 0);
            }
        }
    } owner;

    Require(BCryptOpenAlgorithmProvider(&owner.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(owner.algorithm, &owner.hash, nullptr, 0, nullptr, 0, 0) >= 0,
        "Cannot initialize script revision hash.");
    const auto append = [&](std::string_view bytes)
    {
        std::array<unsigned char, 8> length{};

        for(std::size_t index = 0; index < length.size(); ++index)
        {
            length[index] = static_cast<unsigned char>((static_cast<std::uint64_t>(bytes.size()) >> (index * 8)) & 255);
        }

        Require(BCryptHashData(owner.hash, length.data(), static_cast<ULONG>(length.size()), 0) >= 0 &&
            BCryptHashData(owner.hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                static_cast<ULONG>(bytes.size()), 0) >= 0, "Cannot hash script source bytes.");
    };

    append("Engine.ScriptSource.1/AngelScript.2.38.0/AS3.ValidationOnly");
    append(snapshot.rootSection);
    append(snapshot.rootAsset);
    append(snapshot.complete ? "complete" : "partial");

    std::vector<const SourceSection*> ordered;

    for(const auto& section : snapshot.sections)
    {
        ordered.push_back(&section);
    }

    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right)
    {
        return left->name < right->name;
    });

    for(const auto* section : ordered)
    {
        append(section->name);
        append(section->assetReference);
        append(section->bytes);
    }

    std::array<unsigned char, 32> digest{};
    Require(BCryptFinishHash(owner.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
        "Cannot finish script source hash.");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;

    for(const auto byte : digest)
    {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }

    return result;
}
namespace
{
constexpr std::uint32_t SnapshotMagic = 0x4E534145; // "EASN"
constexpr std::uint32_t SnapshotVersion = 1;

void PutUnsigned(std::vector<std::byte>& out, std::uint32_t value)
{
    for(int shift = 0; shift < 32; shift += 8)
    {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

void PutText(std::vector<std::byte>& out, std::string_view text)
{
    PutUnsigned(out, static_cast<std::uint32_t>(text.size()));

    for(const char character : text)
    {
        out.push_back(static_cast<std::byte>(character));
    }
}

struct SnapshotReader
{
    std::span<const std::byte> bytes;
    std::size_t cursor = 0;

    std::uint32_t Unsigned()
    {
        Require(bytes.size() - cursor >= 4, "Truncated script snapshot.");
        std::uint32_t value = 0;

        for(int shift = 0; shift < 32; shift += 8)
        {
            value |= static_cast<std::uint32_t>(bytes[cursor++]) << shift;
        }

        return value;
    }

    std::string Text(std::size_t limit)
    {
        const auto length = Unsigned();
        Require(length <= limit && bytes.size() - cursor >= length, "Invalid script snapshot string.");
        std::string text(reinterpret_cast<const char*>(bytes.data() + cursor), length);
        cursor += length;
        return text;
    }
};
}

std::vector<std::byte> EncodeSnapshot(const SourceSnapshot& snapshot)
{
    Require(snapshot.complete && !snapshot.sections.empty(), "Only a complete script closure can be packaged.");
    std::vector<std::byte> out;
    PutUnsigned(out, SnapshotMagic);
    PutUnsigned(out, SnapshotVersion);
    PutText(out, snapshot.rootSection);
    PutText(out, snapshot.rootAsset);
    PutText(out, snapshot.revision);
    PutUnsigned(out, static_cast<std::uint32_t>(snapshot.sections.size()));

    for(const auto& section : snapshot.sections)
    {
        PutText(out, section.name);
        PutText(out, section.assetReference);
        PutText(out, section.bytes);
    }

    return out;
}

SourceSnapshot DecodeSnapshot(std::span<const std::byte> bytes, SourceLimits limits)
{
    SnapshotReader reader{bytes};
    Require(reader.Unsigned() == SnapshotMagic && reader.Unsigned() == SnapshotVersion,
        "Unsupported script snapshot format.");
    SourceSnapshot snapshot;
    snapshot.rootSection = reader.Text(1024);
    snapshot.rootAsset = reader.Text(256);
    snapshot.revision = reader.Text(64);
    const auto count = reader.Unsigned();
    Require(count != 0 && count <= limits.sections, "Invalid script snapshot section count.");
    std::size_t total = 0;

    for(std::uint32_t index = 0; index < count; ++index)
    {
        SourceSection section;
        section.name = reader.Text(1024);
        section.assetReference = reader.Text(256);
        section.bytes = reader.Text(limits.sectionBytes);
        total += section.bytes.size();
        Require(total <= limits.totalBytes, "Script snapshot exceeds the closure size limit.");
        snapshot.sections.push_back(std::move(section));
    }

    Require(reader.cursor == bytes.size(), "Trailing script snapshot bytes.");
    snapshot.complete = true;
    return snapshot;
}

}

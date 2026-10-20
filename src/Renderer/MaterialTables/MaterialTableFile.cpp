#include "Renderer/MaterialTables/MaterialTableFile.h"

#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace MaterialTables
{
    namespace
    {
        bool Fail(std::string* error, const std::string& message)
        {
            if(error)
            {
                *error = message;
            }

            return false;
        }

        // Bilinear read of an RG table with coordinates clamped to texel centres (runtime convention).
        std::array<double, 2> SampleClamped(const std::vector<float>& texels, std::uint32_t width,
            std::uint32_t height, double x, double y)
        {
            const double texelX = std::clamp(x * width - 0.5, 0.0, static_cast<double>(width - 1));
            const double texelY = std::clamp(y * height - 0.5, 0.0, static_cast<double>(height - 1));
            const auto baseX = static_cast<std::uint32_t>(std::floor(texelX));
            const auto baseY = static_cast<std::uint32_t>(std::floor(texelY));
            const std::uint32_t nextX = std::min(baseX + 1, width - 1);
            const std::uint32_t nextY = std::min(baseY + 1, height - 1);
            const double blendX = texelX - baseX;
            const double blendY = texelY - baseY;
            std::array<double, 2> result{};

            for(std::uint32_t channel = 0; channel < 2; ++channel)
            {
                const auto texel = [&](std::uint32_t column, std::uint32_t row)
                {
                    return static_cast<double>(texels[(static_cast<size_t>(row) * width + column) * 2 + channel]);
                };
                const double top = texel(baseX, baseY) + (texel(nextX, baseY) - texel(baseX, baseY)) * blendX;
                const double bottom = texel(baseX, nextY) + (texel(nextX, nextY) - texel(baseX, nextY)) * blendX;
                result[channel] = top + (bottom - top) * blendY;
            }

            return result;
        }
    }

    bool ValidateMaterialTableSet(const MaterialTableSet& tables, std::string* error)
    {
        const MaterialTableFileHeader& header = tables.header;

        if(header.magic != MaterialTableFileHeader{}.magic || header.version != FileVersion ||
            header.headerBytes != sizeof(MaterialTableFileHeader) || header.channels != 2)
        {
            return Fail(error, "Material table header is not version 1 RG data.");
        }

        if(header.cosineMapping > static_cast<std::uint32_t>(CosineMapping::Squared))
        {
            return Fail(error, "Material table cosine mapping is unknown.");
        }

        const bool sizesValid = header.directionalWidth >= 2 && header.directionalHeight >= 2 &&
            header.directionalWidth <= MaximumTableSize && header.directionalHeight <= MaximumTableSize &&
            header.averageWidth == header.directionalHeight;

        if(!sizesValid)
        {
            return Fail(error, "Material table dimensions are invalid.");
        }

        if(!(header.minimumRoughness > 0.0f && header.minimumRoughness < 1.0f) || header.latticeSize == 0)
        {
            return Fail(error, "Material table generation parameters are invalid.");
        }

        const size_t directionalFloats = static_cast<size_t>(header.directionalWidth) * header.directionalHeight * 2;
        const size_t averageFloats = static_cast<size_t>(header.averageWidth) * 2;

        if(tables.directional.size() != directionalFloats || tables.average.size() != averageFloats)
        {
            return Fail(error, "Material table payload size does not match its header.");
        }

        // S = 1 - M - B must be nonnegative; a small allowance covers float rounding of the stored pair.
        constexpr double allowance = 1.0e-6;

        const auto validPair = [&](float missing, float bias)
        {
            return std::isfinite(missing) && std::isfinite(bias) && missing >= 0.0f && bias >= 0.0f &&
                static_cast<double>(missing) + bias <= 1.0 + allowance;
        };

        for(size_t index = 0; index < directionalFloats; index += 2)
        {
            if(!validPair(tables.directional[index], tables.directional[index + 1]))
            {
                return Fail(error, "Material table directional texel is non-finite, negative or exceeds 1.");
            }
        }

        for(size_t index = 0; index < averageFloats; index += 2)
        {
            if(!validPair(tables.average[index], tables.average[index + 1]))
            {
                return Fail(error, "Material table average texel is non-finite, negative or exceeds 1.");
            }
        }

        return true;
    }

    bool WriteMaterialTableFile(const std::filesystem::path& path, const MaterialTableSet& tables, std::string* error)
    {
        if(!ValidateMaterialTableSet(tables, error))
        {
            return false;
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);

        if(!file)
        {
            return Fail(error, "Cannot create material table file: " + path.string());
        }

        // Every platform Engine targets is little-endian; the layout is written as in memory.
        file.write(reinterpret_cast<const char*>(&tables.header), sizeof(tables.header));
        file.write(reinterpret_cast<const char*>(tables.directional.data()),
            static_cast<std::streamsize>(tables.directional.size() * sizeof(float)));
        file.write(reinterpret_cast<const char*>(tables.average.data()),
            static_cast<std::streamsize>(tables.average.size() * sizeof(float)));

        if(!file.good())
        {
            return Fail(error, "Failed to write material table file: " + path.string());
        }

        return true;
    }

    bool ParseMaterialTableBytes(std::span<const std::byte> bytes, MaterialTableSet& tables, std::string* error)
    {
        MaterialTableSet candidate;

        if(bytes.size() < sizeof(candidate.header))
        {
            return Fail(error, "Material table file is truncated in its header.");
        }

        std::memcpy(&candidate.header, bytes.data(), sizeof(candidate.header));
        const MaterialTableFileHeader& header = candidate.header;

        if(header.directionalWidth > MaximumTableSize || header.directionalHeight > MaximumTableSize ||
            header.averageWidth > MaximumTableSize)
        {
            return Fail(error, "Material table dimensions exceed the supported maximum.");
        }

        candidate.directional.resize(static_cast<size_t>(header.directionalWidth) * header.directionalHeight * 2);
        candidate.average.resize(static_cast<size_t>(header.averageWidth) * 2);
        const size_t directionalBytes = candidate.directional.size() * sizeof(float);
        const size_t averageBytes = candidate.average.size() * sizeof(float);
        const size_t expectedBytes = sizeof(candidate.header) + directionalBytes + averageBytes;

        if(bytes.size() < expectedBytes)
        {
            return Fail(error, "Material table file is truncated in its payload.");
        }

        if(bytes.size() > expectedBytes)
        {
            return Fail(error, "Material table file has trailing bytes.");
        }

        std::memcpy(candidate.directional.data(), bytes.data() + sizeof(candidate.header), directionalBytes);
        std::memcpy(candidate.average.data(), bytes.data() + sizeof(candidate.header) + directionalBytes, averageBytes);

        if(!ValidateMaterialTableSet(candidate, error))
        {
            return false;
        }

        tables = std::move(candidate);
        return true;
    }

    bool ReadMaterialTableBytes(const std::filesystem::path& path, std::vector<std::byte>& bytes, std::string* error)
    {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(path, sizeError);

        if(sizeError)
        {
            return Fail(error, "Cannot open material table file: " + path.string());
        }

        if(size > MaximumFileBytes)
        {
            return Fail(error, "Material table file exceeds the supported size.");
        }

        std::ifstream file(path, std::ios::binary);
        std::vector<std::byte> candidate(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(candidate.data()), static_cast<std::streamsize>(candidate.size()));

        if(!file.good())
        {
            return Fail(error, "Failed to read material table file: " + path.string());
        }

        bytes = std::move(candidate);
        return true;
    }

    bool ReadMaterialTableFile(const std::filesystem::path& path, MaterialTableSet& tables, std::string* error)
    {
        std::vector<std::byte> bytes;
        return ReadMaterialTableBytes(path, bytes, error) && ParseMaterialTableBytes(bytes, tables, error);
    }

    bool ComputeSha256(std::span<const std::byte> bytes, std::string& hexadecimal)
    {
        if(bytes.size() > 0xFFFFFFFFull)
        {
            return false;
        }

        BCRYPT_ALG_HANDLE algorithm = nullptr;

        if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        {
            return false;
        }

        unsigned char digest[32]{};
        const NTSTATUS status = BCryptHash(algorithm, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()),
            digest, sizeof(digest));
        BCryptCloseAlgorithmProvider(algorithm, 0);

        if(!BCRYPT_SUCCESS(status))
        {
            return false;
        }

        std::string result;
        result.reserve(64);

        for(unsigned char value : digest)
        {
            char pair[3]{};
            std::snprintf(pair, sizeof(pair), "%02x", value);
            result += pair;
        }

        hexadecimal = std::move(result);
        return true;
    }

    bool ValidateBundledMaterialTables(std::span<const std::byte> bytes, MaterialTableSet& tables, std::string* error)
    {
        std::string hash;

        if(!ComputeSha256(bytes, hash))
        {
            return Fail(error, "Cannot compute the material table SHA-256.");
        }

        if(hash != BundledTableSha256)
        {
            return Fail(error, "Material table SHA-256 " + hash + " differs from the bundled " +
                BundledTableSha256 + ".");
        }

        MaterialTableSet candidate;

        if(!ParseMaterialTableBytes(bytes, candidate, error))
        {
            return false;
        }

        const MaterialTableFileHeader& header = candidate.header;
        const bool compatible = header.cosineMapping == static_cast<std::uint32_t>(CosineMapping::Squared) &&
            header.minimumRoughness == 0.045f && header.generatorVersion == BundledGeneratorVersion &&
            header.directionalWidth == header.directionalHeight;

        if(!compatible)
        {
            return Fail(error, "Material table layout is not the one the path-tracing shaders read.");
        }

        tables = std::move(candidate);
        return true;
    }

    double CosineAtColumn(const MaterialTableFileHeader& header, std::uint32_t column)
    {
        const double coordinate = (column + 0.5) / header.directionalWidth;
        return header.cosineMapping == static_cast<std::uint32_t>(CosineMapping::Squared) ? coordinate * coordinate :
            coordinate;
    }

    double CosineCoordinate(const MaterialTableFileHeader& header, double cosine)
    {
        const double clamped = std::clamp(cosine, 0.0, 1.0);
        return header.cosineMapping == static_cast<std::uint32_t>(CosineMapping::Squared) ? std::sqrt(clamped) :
            clamped;
    }

    std::array<double, 2> SampleDirectional(const MaterialTableSet& tables, double cosine, double roughness)
    {
        return SampleClamped(tables.directional, tables.header.directionalWidth, tables.header.directionalHeight,
            CosineCoordinate(tables.header, cosine), std::clamp(roughness, 0.0, 1.0));
    }

    std::array<double, 2> SampleAverage(const MaterialTableSet& tables, double roughness)
    {
        return SampleClamped(tables.average, tables.header.averageWidth, 1, std::clamp(roughness, 0.0, 1.0), 0.5);
    }
}

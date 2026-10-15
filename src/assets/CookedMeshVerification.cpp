#include "assets/CookedMesh.h"
#include "assets/ModelImporter.h"
#include "assets/MeshCookValidation.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include "Renderer/TangentGenerator.h"

#include <bit>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void Require(bool condition, const char* message)
    {
        if(!condition)
        {
            throw std::runtime_error(message);
        }
    }

    using MeshCookValidation::Same;

    std::uint64_t Read(const std::vector<std::byte>& bytes, size_t offset, size_t width = 4)
    {
        std::uint64_t value = 0;

        for(size_t i = 0; i < width; ++i)
        {
            value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + i])) << (8 * i);
        }

        return value;
    }

    void Write(std::vector<std::byte>& bytes, size_t offset, std::uint64_t value, size_t width = 4)
    {
        for(size_t i = 0; i < width; ++i)
        {
            bytes[offset + i] = std::byte((value >> (8 * i)) & 255);
        }
    }

    std::vector<std::byte> RoundTrip(const ImportedMesh& source, const char* label)
    {
        using namespace CookedMesh;
        const Policy policy;
        auto encoded = Encode(source, policy);

        if(encoded.status != Status::Success)
        {
            std::cerr << label << ": " << encoded.message << '\n';
        }

        Require(encoded.status == Status::Success, "Existing mesh is incompatible with v1.");
        const auto again = Encode(source, policy);
        Require(again.status == Status::Success && again.payload == encoded.payload &&
            again.evidence == encoded.evidence, "Encoding is nondeterministic.");

        const auto decoded = Decode(encoded.payload, policy);
        const auto metadata = DecodeMetadata(encoded.payload, policy);
        Require(decoded.status == Status::Success && metadata.status == Status::Success, "Round-trip decoding failed.");
        Require(MeshCookValidation::ExactlyEqual(source, *decoded.geometry), "Geometry/metadata round trip differs.");
        Require(metadata.snapshot->metadata == source.metadata && Same(metadata.snapshot->boundsMin, source.boundsMin) &&
            Same(metadata.snapshot->boundsMax, source.boundsMax) && metadata.snapshot->vertexCount == source.vertices.size() &&
            metadata.snapshot->indexCount == source.indices.size(), "Metadata-only round trip differs.");

        Require(decoded.evidence == encoded.evidence && metadata.snapshot->evidence == encoded.evidence,
            "Encoder/full/metadata payload evidence differs.");
        std::cout << label << ": exact round trip, deterministic bytes and metadata evidence passed ("
            << source.vertices.size() << " vertices, " << source.metadata.submeshes.size() << " submeshes, "
            << source.metadata.materialSlots.size() << " slots).\n";
        return std::move(encoded.payload);
    }

    void Malformed(const std::vector<std::byte>& valid)
    {
        using namespace CookedMesh;
        using namespace V1::Header;
        const Policy policy;
        size_t low = 0;
        size_t high = policy.maxDecodedBytes;

        while(low < high)
        {
            const auto middle = low + (high - low) / 2;
            auto metadataPolicy = policy;
            metadataPolicy.maxDecodedBytes = middle;

            if(DecodeMetadata(valid, metadataPolicy).status == Status::Success)
            {
                high = middle;
            }
            else
            {
                low = middle + 1;
            }
        }

        auto metadataPolicy = policy;
        metadataPolicy.maxDecodedBytes = low;
        Require(DecodeMetadata(valid, metadataPolicy).status == Status::Success &&
            Decode(valid, metadataPolicy).status == Status::AdmissionLimit,
            "Metadata-only decoding unexpectedly requires geometry allocation budget.");

        const auto submesh = static_cast<size_t>(Read(valid, Submeshes, 8));
        const auto slot = static_cast<size_t>(Read(valid, MaterialSlots, 8));
        const auto indices = static_cast<size_t>(Read(valid, Indices, 8));

        const auto check = [&](size_t offset, std::uint64_t value, Status expected, bool metadataRejects = true, size_t width = 4)
        {
            auto bytes = valid;
            Write(bytes, offset, value, width);
            Require(Decode(bytes, policy).status == expected, "Malformed full-decode classification differs.");
            Require(DecodeMetadata(bytes, policy).status == (metadataRejects ? expected : Status::Success),
                "Malformed metadata-decode classification differs.");
        };

        Require(Decode(std::span(valid).first(127), policy).status == Status::Corrupt, "Truncated header accepted.");
        Require(Decode(std::span(valid).first(valid.size() - 1), policy).status == Status::Corrupt, "Truncated payload accepted.");
        check(Submeshes, UINT64_MAX, Status::Corrupt, true, 8);
        check(SubmeshCount, 65'537, Status::AdmissionLimit);
        check(submesh + 12, UINT32_MAX, Status::Corrupt);
        check(Version, 2, Status::Unsupported, true, 2);

        check(CookCompatibility, 2, Status::IncompatibleCook);
        check(indices, Read(valid, VertexCount), Status::Corrupt, false);
        check(submesh + 8, Read(valid, MaterialSlotCount), Status::Corrupt);
        check(Reserved, 1, Status::Corrupt);
        check(slot + 20, 1, Status::Corrupt);

        check(submesh + 20, 1, Status::Corrupt);
        check(Flags, 1, Status::Unsupported);
        check(V1::Header::VertexLayout, 2, Status::Unsupported);
        const auto strings = static_cast<size_t>(Read(valid, Strings, 8));
        const auto stringEnd = strings + static_cast<size_t>(Read(valid, StringBytes));
        const auto vertices = static_cast<size_t>(Read(valid, Vertices, 8));
        Require(stringEnd < vertices, "Verification fixture must exercise alignment padding.");
        check(stringEnd, 1, Status::Corrupt, true, 1);
        check(strings, 0xff, Status::Corrupt, true, 1);

        auto changed = valid;
        Write(changed, vertices, 0x7fc00000); // NaN position; metadata evidence still hashes it.
        const auto originalMetadata = DecodeMetadata(valid, policy);
        const auto changedMetadata = DecodeMetadata(changed, policy);
        Require(Decode(changed, policy).status == Status::Corrupt && changedMetadata.status == Status::Success &&
            changedMetadata.snapshot->evidence != originalMetadata.snapshot->evidence,
            "Metadata validation incorrectly implies geometry validity or ignores changed bytes.");

        std::cout << "Malformed structure/features/geometry, padding, UTF-8 and admission checks passed.\n";
    }
}

// Invoked only by the Debug Engine verification switch, before GPU initialization.
int VerifyCookedMesh()
{
    try
    {
        const auto root = std::filesystem::path(ENGINE_SOURCE_ROOT);
        auto imported = ModelImporter::LoadFlattened(root / "assets/Models/CornellBox/cornell_box.fbx", MeshUVOrientation::FlipV);
        RoundTrip(imported, "cornell_box.fbx");
        auto tank = ModelImporter::LoadFlattened(root / "assets/Models/CornellBox/tank_sample.fbx", MeshUVOrientation::FlipV);
        RoundTrip(tank, "tank_sample.fbx");

        // Small multi-slot fixture exercises signed zero, UTF-8 and padding.
        ImportedMesh fixture;
        fixture.vertices = {{{0, 0, 0}, {0, 0, 1}, {-0.0f, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
        fixture.indices = {0, 1, 2, 2, 1, 0};
        fixture.metadata = {{{0, 3, 0, "Front"}, {3, 3, 1, "Back"}},
            {{"material:2", "Stone", 2}, {"material:7", "M\xc3\xa9tal", 7}}};
        fixture.boundsMax = {1, 1, 0};
        auto bytes = RoundTrip(fixture, "multi-slot fixture");
        Malformed(bytes);

        auto invalid = fixture;
        invalid.vertices[0].normal[0] = std::numeric_limits<float>::infinity();
        Require(CookedMesh::Encode(invalid, {}).status == CookedMesh::Status::Corrupt, "Invalid encoder geometry accepted.");
        invalid = fixture;
        invalid.metadata.materialSlots[1].id = invalid.metadata.materialSlots[0].id;
        Require(CookedMesh::Encode(invalid, {}).status == CookedMesh::Status::Corrupt, "Duplicate encoder slot accepted.");
        auto policy = CookedMesh::Policy{};
        policy.maxPayloadBytes = 128;
        Require(CookedMesh::Encode(fixture, policy).status == CookedMesh::Status::AdmissionLimit, "Encoder payload limit ignored.");
        policy = {};
        policy.maxDecodedBytes = 1;
        Require(CookedMesh::Encode(fixture, policy).status == CookedMesh::Status::AdmissionLimit, "Encoder decoded limit ignored.");

        auto sphere = SphereGenerator::Generate(1.0f, 32, 64);
        TangentGenerator::GenerateTangents(sphere.vertices, sphere.indices);
        ImportedMesh primitive;
        primitive.vertices = std::move(sphere.vertices);
        primitive.indices = std::move(sphere.indices);
        primitive.metadata = DefaultMeshMetadata(static_cast<std::uint32_t>(primitive.indices.size()));

        primitive.boundsMin = {-1, -1, -1};
        primitive.boundsMax = {1, 1, 1};
        const auto encodedSphere = CookedMesh::Encode(primitive, {});
        Require(encodedSphere.status == CookedMesh::Status::Corrupt && encodedSphere.message == "Degenerate triangle.",
            "Built-in sphere validation result changed; inspect primitive compatibility.");
        std::cout << "builtin:sphere: confirmed v1 rejection of existing zero-area pole triangles; geometry unchanged.\n";
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "Cooked mesh verification failed: " << error.what() << '\n';
        return 1;
    }
}

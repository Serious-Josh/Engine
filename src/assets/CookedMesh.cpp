#include "assets/CookedMesh.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace CookedMesh
{
    namespace
    {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

        struct Outcome
        {
            Status status = Status::Success;
            std::string_view message;

            explicit operator bool() const { return status == Status::Success; }
        };

        Outcome Corrupt(std::string_view message) { return {Status::Corrupt, message}; }
        Outcome Limit() { return {Status::AdmissionLimit, "Cooked mesh exceeds the admission policy."}; }

        bool Add(std::uint64_t a, std::uint64_t b, std::uint64_t& result)
        {
            if(b > UINT64_MAX - a)
            {
                return false;
            }

            result = a + b;
            return true;
        }

        bool Multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result)
        {
            if(a && b > UINT64_MAX / a)
            {
                return false;
            }

            result = a * b;
            return true;
        }

        // All calls follow header/section range validation. Reads are bytewise,
        // including floats: neither native alignment nor host endianness is used.
        class Reader
        {
        public:
            explicit Reader(std::span<const std::byte> bytes) : m_bytes(bytes) {}

            std::uint32_t U32(size_t offset) const
            {
                std::uint32_t result = 0;

                for(size_t i = 0; i < 4; ++i)
                {
                    result |= std::uint32_t(std::to_integer<std::uint8_t>(m_bytes[offset + i])) << (8 * i);
                }

                return result;
            }

            std::uint16_t U16(size_t offset) const
            {
                return std::uint16_t(std::to_integer<std::uint8_t>(m_bytes[offset])) |
                    std::uint16_t(std::to_integer<std::uint8_t>(m_bytes[offset + 1]) << 8);
            }

            std::uint64_t U64(size_t offset) const
            {
                return std::uint64_t(U32(offset)) | (std::uint64_t(U32(offset + 4)) << 32);
            }

            float F32(size_t offset) const { return std::bit_cast<float>(U32(offset)); }

            Vector3 Vector(size_t offset) const { return {F32(offset), F32(offset + 4), F32(offset + 8)}; }

        private:
            std::span<const std::byte> m_bytes;
        };

        struct Header
        {
            std::uint32_t vertices = 0;
            std::uint32_t indices = 0;
            std::uint32_t submeshes = 0;
            std::uint32_t slots = 0;

            size_t submeshOffset = 0;
            size_t slotOffset = 0;
            size_t stringOffset = 0;
            size_t vertexOffset = 0;

            size_t indexOffset = 0;
            size_t stringBytes = 0;
            Vector3 boundsMin{};
            Vector3 boundsMax{};
        };

        bool ValidBounds(const Vector3& low, const Vector3& high)
        {
            return std::isfinite(low.x) && std::isfinite(low.y) && std::isfinite(low.z) &&
                std::isfinite(high.x) && std::isfinite(high.y) && std::isfinite(high.z) &&
                low.x <= high.x && low.y <= high.y && low.z <= high.z;
        }

        Outcome ParseHeader(std::span<const std::byte> bytes, const Policy& policy, Header& header)
        {
            if(bytes.size() > policy.maxPayloadBytes)
            {
                return Limit();
            }

            if(bytes.size() < V1::HeaderBytes)
            {
                return Corrupt("Truncated cooked mesh header.");
            }

            for(size_t i = 0; i < V1::Magic.size(); ++i)
            {
                if(std::to_integer<std::uint8_t>(bytes[i]) != V1::Magic[i])
                {
                    return Corrupt("Invalid cooked mesh magic.");
                }
            }

            const Reader reader(bytes);
            using namespace V1::Header;

            if(reader.U16(Version) != V1::Version)
            {
                return {Status::Unsupported, "Unsupported cooked mesh format version."};
            }

            if(reader.U16(HeaderBytes) != V1::HeaderBytes || reader.U32(Reserved) != 0)
            {
                return Corrupt("Invalid v1 header size or reserved field.");
            }

            if(reader.U32(AssetType) != V1::MeshType || reader.U32(VertexLayout) != V1::VertexLayout ||
                reader.U32(IndexBytes) != V1::IndexBytes || (reader.U32(Flags) & ~V1::SupportedFlags) != 0)
            {
                return {Status::Unsupported, "Unsupported cooked mesh type, layout, index width or feature."};
            }

            if(reader.U32(CookCompatibility) != V1::CookCompatibility)
            {
                return {Status::IncompatibleCook, "Unsupported mesh cook compatibility identity; recook required."};
            }

            if(reader.U64(PayloadBytes) != bytes.size())
            {
                return Corrupt("Declared payload size differs from the bounded input.");
            }

            header.vertices = reader.U32(VertexCount);
            header.indices = reader.U32(IndexCount);
            header.submeshes = reader.U32(SubmeshCount);
            header.slots = reader.U32(MaterialSlotCount);

            header.stringBytes = reader.U32(StringBytes);
            header.boundsMin = reader.Vector(BoundsMin);
            header.boundsMax = reader.Vector(BoundsMax);

            if(!header.vertices || !header.indices || header.indices % 3 || !header.submeshes || !header.slots ||
                !ValidBounds(header.boundsMin, header.boundsMax))
            {
                return Corrupt("Empty/nontriangle mesh or invalid bounds.");
            }

            if(header.submeshes > policy.maxSubmeshes || header.slots > policy.maxMaterialSlots ||
                header.stringBytes > policy.maxStringTableBytes ||
                header.vertices > UINT32_MAX / V1::VertexBytes || header.indices > UINT32_MAX / V1::IndexBytes)
            {
                return Limit();
            }

            std::uint64_t end = V1::HeaderBytes;

            const auto section = [&](size_t field, std::uint64_t count, std::uint64_t stride,
                std::uint64_t alignment, size_t& offset) -> bool
            {
                std::uint64_t aligned = 0;
                std::uint64_t sectionBytes = 0;
                std::uint64_t next = 0;

                if(!Add(end, alignment - 1, aligned))
                {
                    return false;
                }

                aligned &= ~(alignment - 1);

                if(reader.U64(field) != aligned || !Multiply(count, stride, sectionBytes) ||
                    !Add(aligned, sectionBytes, next) || next > bytes.size() ||
                    next > (std::numeric_limits<size_t>::max)())
                {
                    return false;
                }

                for(auto i = end; i < aligned; ++i)
                {
                    if(bytes[static_cast<size_t>(i)] != std::byte{0})
                    {
                        return false;
                    }
                }

                offset = static_cast<size_t>(aligned);
                end = next;
                return true;
            };

            if(!section(Submeshes, header.submeshes, V1::SubmeshBytes, V1::SectionAlignment, header.submeshOffset) ||
                !section(MaterialSlots, header.slots, V1::MaterialSlotBytes, V1::SectionAlignment, header.slotOffset) ||
                !section(Strings, header.stringBytes, 1, V1::SectionAlignment, header.stringOffset) ||
                !section(Vertices, header.vertices, V1::VertexBytes, V1::VertexAlignment, header.vertexOffset) ||
                !section(Indices, header.indices, V1::IndexBytes, V1::SectionAlignment, header.indexOffset) || end != bytes.size())
            {
                return Corrupt("Noncanonical sections, truncation, nonzero padding or trailing bytes.");
            }

            return {};
        }

        bool ValidUTF8(std::span<const std::byte> bytes)
        {
            size_t i = 0;

            while(i < bytes.size())
            {
                const auto first = std::to_integer<std::uint8_t>(bytes[i++]);

                if(first == 0)
                {
                    return false;
                }

                if(first < 0x80)
                {
                    continue;
                }

                const size_t following = first >= 0xc2 && first <= 0xdf ? 1 :
                    first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;

                if(!following || following > bytes.size() - i)
                {
                    return false;
                }

                const auto second = std::to_integer<std::uint8_t>(bytes[i]);

                if((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
                    (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90))
                {
                    return false;
                }

                for(size_t j = 0; j < following; ++j)
                {
                    if((std::to_integer<std::uint8_t>(bytes[i++]) & 0xc0) != 0x80)
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        // Conservative admission estimate, including allocator/control-block
        // overhead, string capacity growth/SSO and metadata validation scratch.
        // Actual retained capacities are checked separately before publication.
        class Budget
        {
        public:
            explicit Budget(size_t limit) : m_limit(limit) {}

            bool Charge(std::uint64_t count, std::uint64_t stride, std::uint64_t overhead = 32)
            {
                std::uint64_t amount = 0;

                return Multiply(count, stride, amount) && Add(amount, overhead, amount) &&
                    Add(m_bytes, amount, m_bytes) && m_bytes <= m_limit;
            }

        private:
            std::uint64_t m_bytes = 0;
            size_t m_limit;
        };

        Outcome StringReference(std::span<const std::byte> bytes, const Header& header,
            const Reader& reader, size_t field, const Policy& policy, std::string_view& text)
        {
            const auto offset = reader.U32(field);
            const auto length = reader.U32(field + 4);

            if(!length || offset > header.stringBytes || length > header.stringBytes - offset)
            {
                return Corrupt("Invalid or empty metadata string reference.");
            }

            if(length > policy.maxStringBytes)
            {
                return Limit();
            }

            const auto value = bytes.subspan(header.stringOffset + offset, length);

            if(!ValidUTF8(value))
            {
                return Corrupt("Metadata string is invalid UTF-8 or contains NUL.");
            }

            text = {reinterpret_cast<const char*>(value.data()), value.size()};
            return {};
        }

        bool AdmitBase(Budget& budget, const Header& header, bool full)
        {
            if(!budget.Charge(1, sizeof(ImportedMesh) + sizeof(MetadataSnapshot), 256) ||
                !budget.Charge(header.submeshes, sizeof(MeshSubmesh)) ||
                !budget.Charge(header.slots, sizeof(MeshMaterialSlot)) ||
                !budget.Charge(header.slots, 128))
            {
                return false;
            }

            if(full && (!budget.Charge(header.vertices, sizeof(Vertex)) ||
                !budget.Charge(header.indices, sizeof(std::uint32_t))))
            {
                return false;
            }

            return true;
        }

        Outcome Admit(std::span<const std::byte> bytes, const Header& header, const Policy& policy, bool full)
        {
            Budget budget(policy.maxDecodedBytes);
            const Reader reader(bytes);

            if(!AdmitBase(budget, header, full))
            {
                return Limit();
            }

            const auto string = [&](size_t field, bool slotID) -> Outcome
            {
                std::string_view text;
                const auto result = StringReference(bytes, header, reader, field, policy, text);

                if(!result)
                {
                    return result;
                }

                // Duplicate IDs are copied by ValidateMeshMetadata's set.
                if(!budget.Charge(text.size() + 1, slotID ? 4 : 2, 64))
                {
                    return Limit();
                }

                return {};
            };

            for(size_t i = 0; i < header.submeshes; ++i)
            {
                const auto record = header.submeshOffset + i * V1::SubmeshBytes;

                if(reader.U32(record + 20) != 0)
                {
                    return Corrupt("Nonzero submesh reserved field.");
                }

                if(const auto result = string(record + 12, false); !result)
                {
                    return result;
                }
            }

            for(size_t i = 0; i < header.slots; ++i)
            {
                const auto record = header.slotOffset + i * V1::MaterialSlotBytes;

                if(reader.U32(record + 20) != 0)
                {
                    return Corrupt("Nonzero material-slot reserved field.");
                }

                if(const auto result = string(record, true); !result)
                {
                    return result;
                }

                if(const auto result = string(record + 8, false); !result)
                {
                    return result;
                }
            }

            return {};
        }

        MeshMetadata ReadMetadata(std::span<const std::byte> bytes, const Header& header)
        {
            const Reader reader(bytes);
            MeshMetadata metadata;
            metadata.submeshes.resize(header.submeshes);
            metadata.materialSlots.resize(header.slots);

            const auto string = [&](size_t field)
            {
                return std::string(reinterpret_cast<const char*>(bytes.data() + header.stringOffset + reader.U32(field)),
                    reader.U32(field + 4));
            };

            for(size_t i = 0; i < metadata.submeshes.size(); ++i)
            {
                auto& submesh = metadata.submeshes[i];
                const auto record = header.submeshOffset + i * V1::SubmeshBytes;
                submesh.firstIndex = reader.U32(record);
                submesh.indexCount = reader.U32(record + 4);

                submesh.materialSlot = reader.U32(record + 8);
                submesh.name = string(record + 12);
            }

            for(size_t i = 0; i < metadata.materialSlots.size(); ++i)
            {
                auto& slot = metadata.materialSlots[i];
                const auto record = header.slotOffset + i * V1::MaterialSlotBytes;
                slot.id = string(record);
                slot.name = string(record + 8);

                slot.sourceMaterialIndex = reader.U32(record + 16);
            }

            return metadata;
        }

        bool RetainedFits(const MeshMetadata& metadata, size_t vertexCapacity, size_t indexCapacity, const Policy& policy)
        {
            Budget budget(policy.maxDecodedBytes);

            if(!budget.Charge(1, sizeof(ImportedMesh) + sizeof(MetadataSnapshot), 256) ||
                !budget.Charge(vertexCapacity, sizeof(Vertex)) || !budget.Charge(indexCapacity, sizeof(std::uint32_t)) ||
                !budget.Charge(metadata.submeshes.capacity(), sizeof(MeshSubmesh)) ||
                !budget.Charge(metadata.materialSlots.capacity(), sizeof(MeshMaterialSlot)) ||
                !budget.Charge(metadata.materialSlots.size(), 128))
            {
                return false;
            }

            for(const auto& submesh : metadata.submeshes)
            {
                if(!budget.Charge(submesh.name.capacity() + 1, 1))
                {
                    return false;
                }
            }

            for(const auto& slot : metadata.materialSlots)
            {
                if(!budget.Charge(slot.id.capacity() + 1, 2) || !budget.Charge(slot.name.capacity() + 1, 1))
                {
                    return false;
                }
            }

            return true;
        }

        Outcome ValidateMetadata(const MeshMetadata& metadata, const Header& header)
        {
            std::string error;

            if(!ValidateMeshMetadata(metadata, header.indices, error))
            {
                return Corrupt("Invalid submesh ranges or material slots.");
            }

            return {};
        }

        bool UnitVector(const float* values)
        {
            const double lengthSquared = double(values[0]) * values[0] + double(values[1]) * values[1] +
                double(values[2]) * values[2];

            return std::abs(lengthSquared - 1.0) <= V1::UnitLengthSquaredTolerance;
        }

        Outcome ValidateGeometry(const ImportedMesh& mesh)
        {
            for(const auto& vertex : mesh.vertices)
            {
                const auto finite = [](const auto& values)
                {
                    return std::all_of(std::begin(values), std::end(values), [](float value) { return std::isfinite(value); });
                };

                if(!finite(vertex.position) || !finite(vertex.normal) || !finite(vertex.uv) || !finite(vertex.tangent) ||
                    !UnitVector(vertex.normal) || !UnitVector(vertex.tangent) ||
                    (vertex.tangent[3] != -1.0f && vertex.tangent[3] != 1.0f))
                {
                    return Corrupt("Invalid vertex attributes, normal, tangent or handedness.");
                }

                if(vertex.position[0] < mesh.boundsMin.x || vertex.position[0] > mesh.boundsMax.x ||
                    vertex.position[1] < mesh.boundsMin.y || vertex.position[1] > mesh.boundsMax.y ||
                    vertex.position[2] < mesh.boundsMin.z || vertex.position[2] > mesh.boundsMax.z)
                {
                    return Corrupt("Local bounds do not enclose geometry.");
                }
            }

            for(size_t i = 0; i < mesh.indices.size(); i += 3)
            {
                const auto a = mesh.indices[i];
                const auto b = mesh.indices[i + 1];
                const auto c = mesh.indices[i + 2];

                if(a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size())
                {
                    return Corrupt("Index references an out-of-range vertex.");
                }

                const auto& p = mesh.vertices[a].position;
                const auto& q = mesh.vertices[b].position;
                const auto& r = mesh.vertices[c].position;
                const double ux = double(q[0]) - p[0];

                const double uy = double(q[1]) - p[1];
                const double uz = double(q[2]) - p[2];
                const double vx = double(r[0]) - p[0];
                const double vy = double(r[1]) - p[1];

                const double vz = double(r[2]) - p[2];
                const double area = std::hypot(uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx);

                // Matches ModelImporter: no epsilon that rejects tiny valid triangles.
                if(!std::isfinite(area) || area <= 0.0)
                {
                    return Corrupt("Degenerate triangle.");
                }
            }

            return {};
        }

        Outcome PlanEncoding(const ImportedMesh& mesh, const Policy& policy, Header& header, size_t& total)
        {
            if(mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 ||
                mesh.metadata.submeshes.empty() || mesh.metadata.materialSlots.empty() ||
                !ValidBounds(mesh.boundsMin, mesh.boundsMax))
            {
                return Corrupt("Empty/nontriangle mesh or invalid bounds.");
            }

            if(mesh.vertices.size() > UINT32_MAX / V1::VertexBytes || mesh.indices.size() > UINT32_MAX / V1::IndexBytes ||
                mesh.metadata.submeshes.size() > UINT32_MAX || mesh.metadata.materialSlots.size() > UINT32_MAX ||
                mesh.metadata.submeshes.size() > policy.maxSubmeshes || mesh.metadata.materialSlots.size() > policy.maxMaterialSlots)
            {
                return Limit();
            }

            header.vertices = static_cast<std::uint32_t>(mesh.vertices.size());
            header.indices = static_cast<std::uint32_t>(mesh.indices.size());
            header.submeshes = static_cast<std::uint32_t>(mesh.metadata.submeshes.size());
            header.slots = static_cast<std::uint32_t>(mesh.metadata.materialSlots.size());

            Budget budget(policy.maxDecodedBytes);

            if(!AdmitBase(budget, header, true))
            {
                return Limit();
            }

            std::uint64_t stringBytes = 0;

            const auto string = [&](const std::string& value, bool slotID) -> Outcome
            {
                if(value.empty())
                {
                    return Corrupt("Empty metadata string.");
                }

                if(value.size() > policy.maxStringBytes || !Add(stringBytes, value.size(), stringBytes) ||
                    stringBytes > UINT32_MAX || stringBytes > policy.maxStringTableBytes ||
                    !budget.Charge(value.size() + 1, slotID ? 4 : 2, 64))
                {
                    return Limit();
                }

                if(!ValidUTF8(std::as_bytes(std::span(value.data(), value.size()))))
                {
                    return Corrupt("Metadata string is invalid UTF-8 or contains NUL.");
                }

                return {};
            };

            for(const auto& submesh : mesh.metadata.submeshes)
            {
                if(const auto result = string(submesh.name, false); !result)
                {
                    return result;
                }
            }

            for(const auto& slot : mesh.metadata.materialSlots)
            {
                if(const auto result = string(slot.id, true); !result)
                {
                    return result;
                }

                if(const auto result = string(slot.name, false); !result)
                {
                    return result;
                }
            }

            header.stringBytes = static_cast<size_t>(stringBytes);
            std::uint64_t end = V1::HeaderBytes;

            const auto section = [&](std::uint64_t count, std::uint64_t stride, std::uint64_t alignment, size_t& offset)
            {
                std::uint64_t aligned = 0;
                std::uint64_t size = 0;

                if(!Add(end, alignment - 1, aligned))
                {
                    return false;
                }

                aligned &= ~(alignment - 1);

                if(!Multiply(count, stride, size) || !Add(aligned, size, end) || end > policy.maxPayloadBytes ||
                    end > (std::numeric_limits<size_t>::max)())
                {
                    return false;
                }

                offset = static_cast<size_t>(aligned);
                return true;
            };

            if(!section(header.submeshes, V1::SubmeshBytes, V1::SectionAlignment, header.submeshOffset) ||
                !section(header.slots, V1::MaterialSlotBytes, V1::SectionAlignment, header.slotOffset) ||
                !section(stringBytes, 1, V1::SectionAlignment, header.stringOffset) ||
                !section(header.vertices, V1::VertexBytes, V1::VertexAlignment, header.vertexOffset) ||
                !section(header.indices, V1::IndexBytes, V1::SectionAlignment, header.indexOffset))
            {
                return Limit();
            }

            total = static_cast<size_t>(end);
            return ValidateMetadata(mesh.metadata, header);
        }

        class Writer
        {
        public:
            explicit Writer(std::span<std::byte> bytes) : m_bytes(bytes) {}

            void Integer(size_t offset, std::uint64_t value, size_t width)
            {
                for(size_t i = 0; i < width; ++i)
                {
                    m_bytes[offset + i] = std::byte((value >> (8 * i)) & 0xff);
                }
            }

            void U32(size_t offset, std::uint32_t value) { Integer(offset, value, 4); }
            void F32(size_t offset, float value) { U32(offset, std::bit_cast<std::uint32_t>(value)); }

            void Vector(size_t offset, const Vector3& value)
            {
                F32(offset, value.x);
                F32(offset + 4, value.y);
                F32(offset + 8, value.z);
            }

            void String(size_t field, const std::string& value, const Header& header, size_t& cursor)
            {
                U32(field, static_cast<std::uint32_t>(cursor));
                U32(field + 4, static_cast<std::uint32_t>(value.size()));

                for(char byte : value)
                {
                    m_bytes[header.stringOffset + cursor++] = std::byte(static_cast<unsigned char>(byte));
                }
            }

        private:
            std::span<std::byte> m_bytes;
        };

        void WritePayload(std::span<std::byte> bytes, const ImportedMesh& mesh, const Header& header)
        {
            Writer writer(bytes);
            using namespace V1::Header;

            for(size_t i = 0; i < V1::Magic.size(); ++i)
            {
                bytes[i] = std::byte(V1::Magic[i]);
            }

            writer.Integer(Version, V1::Version, 2);
            writer.Integer(HeaderBytes, V1::HeaderBytes, 2);
            writer.U32(AssetType, V1::MeshType);
            writer.Integer(PayloadBytes, bytes.size(), 8);

            writer.U32(Flags, V1::SupportedFlags);
            writer.U32(VertexLayout, V1::VertexLayout);
            writer.U32(VertexCount, header.vertices);
            writer.U32(IndexCount, header.indices);

            writer.U32(SubmeshCount, header.submeshes);
            writer.U32(MaterialSlotCount, header.slots);
            writer.U32(IndexBytes, V1::IndexBytes);
            writer.U32(CookCompatibility, V1::CookCompatibility);

            writer.Vector(BoundsMin, mesh.boundsMin);
            writer.Vector(BoundsMax, mesh.boundsMax);
            writer.Integer(Submeshes, header.submeshOffset, 8);
            writer.Integer(MaterialSlots, header.slotOffset, 8);

            writer.Integer(Strings, header.stringOffset, 8);
            writer.Integer(Vertices, header.vertexOffset, 8);
            writer.Integer(Indices, header.indexOffset, 8);
            writer.U32(StringBytes, static_cast<std::uint32_t>(header.stringBytes));

            writer.U32(Reserved, 0);
            size_t cursor = 0;

            for(size_t i = 0; i < mesh.metadata.submeshes.size(); ++i)
            {
                const auto& submesh = mesh.metadata.submeshes[i];
                const auto record = header.submeshOffset + i * V1::SubmeshBytes;
                writer.U32(record, submesh.firstIndex);
                writer.U32(record + 4, submesh.indexCount);

                writer.U32(record + 8, submesh.materialSlot);
                writer.String(record + 12, submesh.name, header, cursor);
                writer.U32(record + 20, 0);
            }

            for(size_t i = 0; i < mesh.metadata.materialSlots.size(); ++i)
            {
                const auto& slot = mesh.metadata.materialSlots[i];
                const auto record = header.slotOffset + i * V1::MaterialSlotBytes;
                writer.String(record, slot.id, header, cursor);
                writer.String(record + 8, slot.name, header, cursor);

                writer.U32(record + 16, slot.sourceMaterialIndex);
                writer.U32(record + 20, 0);
            }

            for(size_t i = 0; i < mesh.vertices.size(); ++i)
            {
                const auto& vertex = mesh.vertices[i];
                const auto record = header.vertexOffset + i * V1::VertexBytes;

                for(size_t j = 0; j < 3; ++j)
                {
                    writer.F32(record + j * 4, vertex.position[j]);
                    writer.F32(record + 12 + j * 4, vertex.normal[j]);
                }

                for(size_t j = 0; j < 2; ++j)
                {
                    writer.F32(record + 24 + j * 4, vertex.uv[j]);
                }

                for(size_t j = 0; j < 4; ++j)
                {
                    writer.F32(record + 32 + j * 4, vertex.tangent[j]);
                }
            }

            for(size_t i = 0; i < mesh.indices.size(); ++i)
            {
                writer.U32(header.indexOffset + i * V1::IndexBytes, mesh.indices[i]);
            }
        }

        Outcome Identify(std::span<const std::byte> bytes, Evidence& evidence)
        {
            BCRYPT_HASH_HANDLE hash = nullptr;

            if(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            {
                return {Status::IdentityFailure, "Cannot create cooked payload SHA-256 hash."};
            }

            size_t offset = 0;
            bool success = true;

            while(offset < bytes.size() && success)
            {
                const auto count = static_cast<ULONG>((std::min)(bytes.size() - offset, size_t(1024 * 1024)));
                success = BCryptHashData(hash, const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data() + offset)), count, 0) >= 0;
                offset += count;
            }

            success = success && BCryptFinishHash(hash, evidence.payloadSHA256.data(),
                static_cast<ULONG>(evidence.payloadSHA256.size()), 0) >= 0;
            BCryptDestroyHash(hash);

            if(!success)
            {
                return {Status::IdentityFailure, "Cannot hash cooked payload."};
            }

            evidence.payloadBytes = bytes.size();
            evidence.cookCompatibility = V1::CookCompatibility;
            evidence.formatVersion = V1::Version;
            return {};
        }
    }

    EncodeResult Encode(const ImportedMesh& geometry, const Policy& policy)
    {
        try
        {
            Header header;
            size_t total = 0;

            if(const auto result = PlanEncoding(geometry, policy, header, total); !result)
            {
                return {result.status, result.message};
            }

            if(const auto result = ValidateGeometry(geometry); !result)
            {
                return {result.status, result.message};
            }

            // Value-initialization covers every reserved field and alignment byte.
            std::vector<std::byte> payload(total, std::byte{0});
            WritePayload(payload, geometry, header);
            Evidence evidence;

            if(const auto result = Identify(payload, evidence); !result)
            {
                return {result.status, result.message};
            }

            return {Status::Success, {}, std::move(payload), evidence};
        }
        catch(const std::bad_alloc&)
        {
            return {Status::AllocationFailure, "Cooked mesh encoding allocation failed."};
        }
        catch(const std::length_error&)
        {
            return {Status::AdmissionLimit, "Cooked mesh encoding exceeds container capacity."};
        }
    }

    DecodeResult Decode(std::span<const std::byte> payload, const Policy& policy)
    {
        try
        {
            Header header;

            if(const auto result = ParseHeader(payload, policy, header); !result)
            {
                return {result.status, result.message};
            }

            if(const auto result = Admit(payload, header, policy, true); !result)
            {
                return {result.status, result.message};
            }

            auto geometry = std::make_shared<ImportedMesh>();
            geometry->metadata = ReadMetadata(payload, header);

            if(!RetainedFits(geometry->metadata, header.vertices, header.indices, policy))
            {
                return {Status::AdmissionLimit, "Decoded metadata exceeds the allocation budget."};
            }

            if(const auto result = ValidateMetadata(geometry->metadata, header); !result)
            {
                return {result.status, result.message};
            }

            geometry->vertices.resize(header.vertices);
            geometry->indices.resize(header.indices);

            if(!RetainedFits(geometry->metadata, geometry->vertices.capacity(), geometry->indices.capacity(), policy))
            {
                return {Status::AdmissionLimit, "Decoded vector capacities exceed the allocation budget."};
            }

            geometry->boundsMin = header.boundsMin;
            geometry->boundsMax = header.boundsMax;
            const Reader reader(payload);

            for(size_t i = 0; i < geometry->vertices.size(); ++i)
            {
                auto& vertex = geometry->vertices[i];
                const auto record = header.vertexOffset + i * V1::VertexBytes;

                for(size_t j = 0; j < 3; ++j)
                {
                    vertex.position[j] = reader.F32(record + j * 4);
                    vertex.normal[j] = reader.F32(record + 12 + j * 4);
                }

                for(size_t j = 0; j < 2; ++j)
                {
                    vertex.uv[j] = reader.F32(record + 24 + j * 4);
                }

                for(size_t j = 0; j < 4; ++j)
                {
                    vertex.tangent[j] = reader.F32(record + 32 + j * 4);
                }
            }

            for(size_t i = 0; i < geometry->indices.size(); ++i)
            {
                geometry->indices[i] = reader.U32(header.indexOffset + i * V1::IndexBytes);
            }

            if(const auto result = ValidateGeometry(*geometry); !result)
            {
                return {result.status, result.message};
            }

            Evidence evidence;

            if(const auto result = Identify(payload, evidence); !result)
            {
                return {result.status, result.message};
            }

            return {Status::Success, {}, std::move(geometry), evidence};
        }
        catch(const std::bad_alloc&)
        {
            return {Status::AllocationFailure, "Cooked mesh allocation failed."};
        }
        catch(const std::length_error&)
        {
            return {Status::AdmissionLimit, "Cooked mesh exceeds container capacity."};
        }
    }

    MetadataResult DecodeMetadata(std::span<const std::byte> payload, const Policy& policy)
    {
        try
        {
            Header header;

            if(const auto result = ParseHeader(payload, policy, header); !result)
            {
                return {result.status, result.message};
            }

            if(const auto result = Admit(payload, header, policy, false); !result)
            {
                return {result.status, result.message};
            }

            auto snapshot = std::make_shared<MetadataSnapshot>();
            snapshot->metadata = ReadMetadata(payload, header);

            if(!RetainedFits(snapshot->metadata, 0, 0, policy))
            {
                return {Status::AdmissionLimit, "Decoded metadata capacities exceed the allocation budget."};
            }

            if(const auto result = ValidateMetadata(snapshot->metadata, header); !result)
            {
                return {result.status, result.message};
            }

            snapshot->boundsMin = header.boundsMin;
            snapshot->boundsMax = header.boundsMax;
            snapshot->vertexCount = header.vertices;
            snapshot->indexCount = header.indices;

            if(const auto result = Identify(payload, snapshot->evidence); !result)
            {
                return {result.status, result.message};
            }

            return {Status::Success, {}, std::move(snapshot)};
        }
        catch(const std::bad_alloc&)
        {
            return {Status::AllocationFailure, "Cooked metadata allocation failed."};
        }
        catch(const std::length_error&)
        {
            return {Status::AdmissionLimit, "Cooked metadata exceeds container capacity."};
        }
    }
}

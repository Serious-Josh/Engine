#include "Engine/AcousticGeometry.h"

#include <cmath>
#include <set>

namespace AcousticGeometry
{
namespace
{
    Audio::AcousticMaterialValues ToValues(const AcousticMaterialData& material)
    {
        return Audio::AcousticMaterialValues{material.absorption, material.scattering, material.transmission};
    }

    bool IsDegenerate(const Vertex& a, const Vertex& b, const Vertex& c)
    {
        const double abX = b.position[0] - a.position[0];
        const double abY = b.position[1] - a.position[1];
        const double abZ = b.position[2] - a.position[2];
        const double acX = c.position[0] - a.position[0];
        const double acY = c.position[1] - a.position[1];
        const double acZ = c.position[2] - a.position[2];
        const double crossX = abY * acZ - abZ * acY;
        const double crossY = abZ * acX - abX * acZ;
        const double crossZ = abX * acY - abY * acX;
        return crossX * crossX + crossY * crossY + crossZ * crossZ <= 0.0;
    }
}

std::string SubmeshKey(const MeshSubmesh& submesh, std::size_t index)
{
    return std::to_string(index) + ":" + submesh.name;
}

std::shared_ptr<const Audio::AcousticMeshData> Build(const ImportedMesh& mesh, const AcousticSettings& settings,
    const MaterialResolver& resolveMaterial, BuildReport& report)
{
    std::string metadataError;

    if(mesh.vertices.empty() || mesh.indices.empty() || mesh.metadata.submeshes.empty() ||
        !ValidateMeshMetadata(mesh.metadata, mesh.indices.size(), metadataError))
    {
        report.error = metadataError.empty() ? "the mesh has no triangles" : metadataError;
        return nullptr;
    }

    auto data = std::make_shared<Audio::AcousticMeshData>();
    data->positions.reserve(mesh.vertices.size());

    for(const Vertex& vertex : mesh.vertices)
    {
        if(!std::isfinite(vertex.position[0]) || !std::isfinite(vertex.position[1]) || !std::isfinite(vertex.position[2]))
        {
            report.error = "the mesh has non-finite vertex positions";
            return nullptr;
        }

        data->positions.push_back({vertex.position[0], vertex.position[1], vertex.position[2]});
    }

    std::set<std::string> usedKeys;

    for(std::size_t submeshIndex = 0; submeshIndex < mesh.metadata.submeshes.size(); ++submeshIndex)
    {
        const MeshSubmesh& submesh = mesh.metadata.submeshes[submeshIndex];
        const std::string key = SubmeshKey(submesh, submeshIndex);
        AcousticMaterialData material = AcousticMaterials::DefaultMaterial();
        const auto assigned = settings.submeshMaterials.find(key);

        if(assigned == settings.submeshMaterials.end())
        {
            report.warnings.push_back("submesh '" + key + "' has no acoustic material; it uses the default");
        }
        else
        {
            usedKeys.insert(key);
            std::string materialError;
            const std::optional<AcousticMaterialData> resolved = resolveMaterial(assigned->second, &materialError);

            if(resolved)
            {
                material = *resolved;
            }
            else
            {
                report.warnings.push_back("submesh '" + key + "': acoustic material " + assigned->second + ": " + materialError +
                    "; it uses the default");
            }
        }

        const auto materialIndex = static_cast<std::int32_t>(data->materials.size());
        data->materials.push_back(ToValues(material));

        for(std::uint32_t index = submesh.firstIndex; index < submesh.firstIndex + submesh.indexCount; index += 3)
        {
            const std::uint32_t first = mesh.indices[index];
            const std::uint32_t second = mesh.indices[index + 1];
            const std::uint32_t third = mesh.indices[index + 2];

            if(first >= mesh.vertices.size() || second >= mesh.vertices.size() || third >= mesh.vertices.size())
            {
                report.error = "the mesh has an index out of range";
                return nullptr;
            }

            if(IsDegenerate(mesh.vertices[first], mesh.vertices[second], mesh.vertices[third]))
            {
                continue;
            }

            data->triangles.push_back({first, second, third});
            data->triangleMaterials.push_back(materialIndex);
        }
    }

    for(const auto& [key, reference] : settings.submeshMaterials)
    {
        if(!usedKeys.contains(key))
        {
            report.warnings.push_back("acoustic material assignment '" + key + "' matches no submesh of this mesh; ignored");
        }
    }

    if(data->triangles.empty())
    {
        report.error = "the mesh has only degenerate triangles";
        return nullptr;
    }

    return data;
}
}

#include "assets/ModelImporter.h"

#include "Renderer/TangentGenerator.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/material.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace
{
    // Keep buffer byte sizes representable by the existing 32-bit Mesh views.
    constexpr size_t MaxVertices = UINT32_MAX / sizeof(Vertex);
    constexpr size_t MaxIndices = UINT32_MAX / sizeof(uint32_t);

    void Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Finite(const aiVector3D& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    aiVector3D Normalize(const aiVector3D& value)
    {
        const double length = std::hypot(double(value.x), double(value.y), double(value.z));
        Require(Finite(value) && std::isfinite(length) && length > 0.0,
            "Normal is nonfinite or has zero length.");

        return {ai_real(value.x / length), ai_real(value.y / length), ai_real(value.z / length)};
    }

    aiMatrix3x3 NormalTransform(const aiMatrix4x4& transform, bool& mirrored)
    {
        for (unsigned int row = 0; row < 4; ++row)
        {
            for (unsigned int column = 0; column < 4; ++column)
            {
                Require(std::isfinite(transform[row][column]), "Node transform is nonfinite.");
            }
        }

        Require(transform.d1 == 0 && transform.d2 == 0 && transform.d3 == 0 && transform.d4 == 1,
            "Node transform must be affine.");

        aiMatrix3x3 normalTransform(transform);
        const ai_real determinant = normalTransform.Determinant();
        Require(std::isfinite(determinant) && determinant != 0,
            "Node transform is singular or its determinant is unrepresentable.");

        mirrored = determinant < 0;
        normalTransform.Inverse().Transpose();

        for (unsigned int row = 0; row < 3; ++row)
        {
            for (unsigned int column = 0; column < 3; ++column)
            {
                Require(std::isfinite(normalTransform[row][column]), "Normal transform is nonfinite.");
            }
        }

        return normalTransform;
    }

    void CalculateBounds(ImportedMesh& mesh)
    {
        const auto& first = mesh.vertices.front();
        mesh.boundsMin = {first.position[0], first.position[1], first.position[2]};
        mesh.boundsMax = mesh.boundsMin;

        for (const Vertex& vertex : mesh.vertices)
        {
            mesh.boundsMin.x = (std::min)(mesh.boundsMin.x, vertex.position[0]);
            mesh.boundsMin.y = (std::min)(mesh.boundsMin.y, vertex.position[1]);
            mesh.boundsMin.z = (std::min)(mesh.boundsMin.z, vertex.position[2]);

            mesh.boundsMax.x = (std::max)(mesh.boundsMax.x, vertex.position[0]);
            mesh.boundsMax.y = (std::max)(mesh.boundsMax.y, vertex.position[1]);
            mesh.boundsMax.z = (std::max)(mesh.boundsMax.z, vertex.position[2]);
        }
    }

    ImportedMesh ConvertMesh(const aiMesh* source, const aiMatrix4x4& transform,
        MeshUVOrientation uvOrientation)
    {
        Require(source != nullptr, "Mesh pointer is null.");
        Require(source->mVertices && source->mNumVertices > 0, "Mesh has no vertices.");
        Require(source->mFaces && source->mNumFaces > 0, "Mesh has no triangle faces.");
        Require(source->mNormals != nullptr, "Normals are absent even after Assimp normal generation.");
        Require(source->mNumVertices <= MaxVertices && source->mNumFaces <= MaxIndices / 3,
            "Mesh exceeds supported vertex/index buffer sizes.");

        bool mirrored = false;
        const aiMatrix3x3 normalTransform = NormalTransform(transform, mirrored);
        ImportedMesh result;
        result.vertices.reserve(source->mNumVertices);
        result.indices.reserve(size_t(source->mNumFaces) * 3);

        for (unsigned int i = 0; i < source->mNumVertices; ++i)
        {
            Require(Finite(source->mVertices[i]), "Vertex " + std::to_string(i) + " has a nonfinite position.");
            const aiVector3D position = transform * source->mVertices[i];
            Require(Finite(position), "Transformed vertex " + std::to_string(i) + " is nonfinite.");

            Vertex vertex{};
            vertex.position[0] = position.x;
            vertex.position[1] = position.y;
            vertex.position[2] = position.z;

            if (source->mNormals)
            {
                const aiVector3D normal = Normalize(normalTransform * Normalize(source->mNormals[i]));
                vertex.normal[0] = normal.x;
                vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
            }

            if (source->mTextureCoords[0])
            {
                const aiVector3D uv = source->mTextureCoords[0][i];
                Require(std::isfinite(uv.x) && std::isfinite(uv.y),
                    "Vertex " + std::to_string(i) + " has nonfinite UV coordinates.");
                vertex.uv[0] = uv.x;
                vertex.uv[1] = uv.y;
            }

            if(uvOrientation == MeshUVOrientation::FlipV)
            {
                vertex.uv[1] = 1.0f - vertex.uv[1];
            }

            result.vertices.push_back(vertex);
        }

        for (unsigned int i = 0; i < source->mNumFaces; ++i)
        {
            const aiFace& face = source->mFaces[i];
            Require(face.mIndices && face.mNumIndices == 3,
                "Face " + std::to_string(i) + " is not a triangle after triangulation.");

            for (unsigned int corner = 0; corner < 3; ++corner)
            {
                Require(face.mIndices[corner] < result.vertices.size(),
                    "Face " + std::to_string(i) + " references an out-of-range vertex.");
            }

            const uint32_t a = face.mIndices[0];
            const uint32_t b = face.mIndices[mirrored ? 2 : 1];
            const uint32_t c = face.mIndices[mirrored ? 1 : 2];
            const auto& p = result.vertices[a].position;
            const auto& q = result.vertices[b].position;
            const auto& r = result.vertices[c].position;

            const double ux = double(q[0]) - p[0];
            const double uy = double(q[1]) - p[1];
            const double uz = double(q[2]) - p[2];

            const double vx = double(r[0]) - p[0];
            const double vy = double(r[1]) - p[1];
            const double vz = double(r[2]) - p[2];
            const double area = std::hypot(uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx);
            Require(std::isfinite(area) && area > 0,
                "Face " + std::to_string(i) + " is degenerate after transform baking.");

            result.indices.push_back(a);
            result.indices.push_back(b);
            result.indices.push_back(c);
        }

        // TangentGenerator may split every vertex once at mirrored UV seams.
        Require(result.vertices.size() <= MaxVertices / 2,
            "Mesh is too large for safe tangent seam splitting.");
        TangentGenerator::GenerateTangents(result.vertices, result.indices);

        for (const Vertex& vertex : result.vertices)
        {
            Require(std::isfinite(vertex.tangent[0]) && std::isfinite(vertex.tangent[1]) &&
                std::isfinite(vertex.tangent[2]) && std::isfinite(vertex.tangent[3]),
                "Tangent generation produced a nonfinite tangent.");
        }

        CalculateBounds(result);
        return result;
    }

    std::vector<ImportedMesh> Import(const std::filesystem::path& path, MeshUVOrientation uvOrientation)
    {
        Require(uvOrientation == MeshUVOrientation::PreserveSource || uvOrientation == MeshUVOrientation::FlipV,
            "Unsupported mesh UV orientation policy.");

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path.string(),
            aiProcess_Triangulate | aiProcess_ValidateDataStructure | aiProcess_GenSmoothNormals);
        Require(scene != nullptr, std::string("Assimp: ") + importer.GetErrorString());
        Require(scene->mRootNode && scene->mMeshes && scene->mNumMeshes > 0 &&
            !(scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE), "Scene has no complete mesh hierarchy.");

        struct PendingNode
        {
            const aiNode* node;
            aiMatrix4x4 parentTransform;
        };

        std::vector<PendingNode> pending{{scene->mRootNode, aiMatrix4x4{}}};
        std::unordered_set<const aiNode*> visited;
        std::vector<ImportedMesh> meshes;

        while (!pending.empty())
        {
            const PendingNode entry = pending.back();
            pending.pop_back();
            const aiNode* node = entry.node;
            Require(node && visited.insert(node).second, "Scene contains a null, repeated, or cyclic node.");
            const std::string context = "Node '" + std::string(node->mName.C_Str()) + "': ";

            try
            {
                bool mirrored = false;
                NormalTransform(node->mTransformation, mirrored);
                const aiMatrix4x4 transform = entry.parentTransform * node->mTransformation;
                NormalTransform(transform, mirrored);
                Require(node->mNumMeshes == 0 || node->mMeshes, "Node mesh array is null.");
                Require(node->mNumChildren == 0 || node->mChildren, "Node child array is null.");

                for (unsigned int i = 0; i < node->mNumMeshes; ++i)
                {
                    const unsigned int meshIndex = node->mMeshes[i];
                    Require(meshIndex < scene->mNumMeshes, "Mesh reference is out of range.");

                    try
                    {
                        const aiMesh* source = scene->mMeshes[meshIndex];
                        ImportedMesh mesh = ConvertMesh(source, transform, uvOrientation);
                        Require(scene->mMaterials && source->mMaterialIndex < scene->mNumMaterials &&
                            scene->mMaterials[source->mMaterialIndex], "Mesh references an invalid source material.");

                        aiString materialName;
                        scene->mMaterials[source->mMaterialIndex]->Get(AI_MATKEY_NAME, materialName);
                        const std::string slotName = materialName.length ? materialName.C_Str() :
                            "Material " + std::to_string(source->mMaterialIndex);
                        const std::string meshName = source->mName.length ? source->mName.C_Str() :
                            node->mName.length ? node->mName.C_Str() : "Mesh " + std::to_string(meshIndex);

                        mesh.metadata = {{{0, static_cast<uint32_t>(mesh.indices.size()), 0, meshName}},
                            {{"default", slotName, source->mMaterialIndex}}};
                        std::string metadataError;
                        Require(ValidateMeshMetadata(mesh.metadata, mesh.indices.size(), metadataError), metadataError);
                        meshes.push_back(std::move(mesh));
                    }
                    catch (const std::exception& error)
                    {
                        throw std::runtime_error("Mesh " + std::to_string(meshIndex) + ": " + error.what());
                    }
                }

                // Reverse push preserves the legacy depth-first mesh ordering.
                for (unsigned int i = node->mNumChildren; i > 0; --i)
                {
                    pending.push_back({node->mChildren[i - 1], transform});
                }
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error(context + error.what());
            }
        }

        Require(!meshes.empty(), "Scene contains no usable triangle geometry.");
        return meshes;
    }
}

std::vector<ImportedMesh> ModelImporter::Load(const std::filesystem::path& path, MeshUVOrientation uvOrientation)
{
    try
    {
        return Import(path, uvOrientation);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Failed to import model '" + path.string() + "': " + error.what());
    }
}

ImportedMesh ModelImporter::LoadFlattened(const std::filesystem::path& path, MeshUVOrientation uvOrientation)
{
    try
    {
        const auto meshes = Import(path, uvOrientation);
        size_t vertexCount = 0;
        size_t indexCount = 0;

        for (const ImportedMesh& mesh : meshes)
        {
            Require(mesh.vertices.size() <= MaxVertices - vertexCount &&
                mesh.indices.size() <= MaxIndices - indexCount, "Flattened mesh exceeds supported buffer sizes.");
            vertexCount += mesh.vertices.size();
            indexCount += mesh.indices.size();
        }

        ImportedMesh result;
        result.vertices.reserve(vertexCount);
        result.indices.reserve(indexCount);

        for (const ImportedMesh& mesh : meshes)
        {
            const uint32_t baseVertex = static_cast<uint32_t>(result.vertices.size());
            const uint32_t firstIndex = static_cast<uint32_t>(result.indices.size());
            result.vertices.insert(result.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());

            const auto& sourceSlot = mesh.metadata.materialSlots.front();
            auto slot = std::find_if(result.metadata.materialSlots.begin(), result.metadata.materialSlots.end(),
                [&](const MeshMaterialSlot& existing)
                {
                    return existing.sourceMaterialIndex == sourceSlot.sourceMaterialIndex;
                });

            if(slot == result.metadata.materialSlots.end())
            {
                result.metadata.materialSlots.push_back({"material:" + std::to_string(sourceSlot.sourceMaterialIndex),
                    sourceSlot.name, sourceSlot.sourceMaterialIndex});
                slot = result.metadata.materialSlots.end() - 1;
            }

            const auto materialSlot = static_cast<uint32_t>(slot - result.metadata.materialSlots.begin());
            result.metadata.submeshes.push_back({firstIndex, static_cast<uint32_t>(mesh.indices.size()),
                materialSlot, mesh.metadata.submeshes.front().name});

            for (uint32_t index : mesh.indices)
            {
                Require(index < mesh.vertices.size() && index <= UINT32_MAX - baseVertex,
                    "Flattened mesh index rebasing overflow.");
                result.indices.push_back(baseVertex + index);
            }
        }

        if(result.metadata.materialSlots.size() == 1)
        {
            result.metadata.materialSlots.front().id = "default";
        }

        std::string metadataError;
        Require(ValidateMeshMetadata(result.metadata, result.indices.size(), metadataError), metadataError);
        CalculateBounds(result);
        return result;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Failed to flatten model '" + path.string() + "': " + error.what());
    }
}

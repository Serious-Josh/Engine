#include "assets/ModelImporter.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include "assimp/scene.h"

#include <stdexcept>
#include <string>

static ImportedMesh ConvertMesh(const aiMesh* sourceMesh, const aiMatrix4x4& transform)
{
    aiMatrix3x3 normalTransform(transform);
    normalTransform.Inverse();
    normalTransform.Transpose();

    ImportedMesh importedMesh;

    importedMesh.vertices.reserve(sourceMesh->mNumVertices);
    importedMesh.indices.reserve(sourceMesh->mNumFaces * 3);

    for (unsigned int vertexIndex = 0; vertexIndex < sourceMesh->mNumVertices; ++vertexIndex)
    {
        const aiVector3D position = transform * sourceMesh->mVertices[vertexIndex];

        aiVector3D normal = normalTransform * sourceMesh->mNormals[vertexIndex];

        normal.Normalize();

        const aiVector3D& uv = sourceMesh->mTextureCoords[0][vertexIndex];

        Vertex vertex{};

        vertex.position[0] = position.x;
        vertex.position[1] = position.y;
        vertex.position[2] = position.z;

        vertex.normal[0] = normal.x;
        vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;

        vertex.uv[0] = uv.x;
        vertex.uv[1] = uv.y;

        importedMesh.vertices.push_back(vertex);
    }

    for (unsigned int faceIndex = 0;faceIndex < sourceMesh->mNumFaces; ++faceIndex)
    {
        const aiFace& face = sourceMesh->mFaces[faceIndex];

        if (face.mNumIndices != 3)
        {
            throw std::runtime_error("Assimp produced a non-triangle face after triangulation.");
        }

        importedMesh.indices.push_back(face.mIndices[0]);
        importedMesh.indices.push_back(face.mIndices[1]);
        importedMesh.indices.push_back(face.mIndices[2]);
    }

    return importedMesh;
}

static void ProcessNode(
    const aiScene* scene,
    const aiNode* node,
    const aiMatrix4x4& parentTransform,
    std::vector<ImportedMesh>& importedMeshes)
{
    const aiMatrix4x4 accumulatedTransform = parentTransform * node->mTransformation;

    for (unsigned int i = 0; i < node->mNumMeshes; ++i)
    {
        const unsigned int meshIndex = node->mMeshes[i];
        const aiMesh* sourceMesh = scene->mMeshes[meshIndex];

        importedMeshes.push_back(ConvertMesh(sourceMesh, accumulatedTransform));
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i)
    {
        ProcessNode(scene, node->mChildren[i], accumulatedTransform, importedMeshes);
    }
}

std::vector<ImportedMesh> ModelImporter::Load(const std::filesystem::path& path)
{
    Assimp::Importer importer;

    const aiScene* scene = importer.ReadFile(path.string(), aiProcess_Triangulate);

    if(scene == nullptr){
        throw std::runtime_error("Failed to import model '" + path.string() + "': " + importer.GetErrorString());
    }

    std::vector<ImportedMesh> importedMeshes;
    importedMeshes.reserve(scene->mNumMeshes);

    const aiMatrix4x4 identity;

    ProcessNode(
        scene,
        scene->mRootNode,
        identity,
        importedMeshes);

    return importedMeshes;
}
#include "assets/PrimitiveMeshAssets.h"
#include "assets/ProjectMeshImportService.h"
#include "assets/ProjectAssetRegistry.h"
#include "assets/ProjectAssetResolver.h"
#include "assets/ModelImporter.h"
#include "assets/CookedMesh.h"
#include "Project/ProjectPaths.h"
#include "Renderer/TangentGenerator.h"
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/material.h>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace PrimitiveMeshAssets
{
const char* Name(ScenePrimitive primitive)
{
    switch(primitive)
    {
    case ScenePrimitive::Cube: return "Cube";
    case ScenePrimitive::Sphere: return "Sphere";
    }
    throw std::invalid_argument("Unsupported scene primitive.");
}

ImportedMesh Generate(ScenePrimitive primitive)
{
    ImportedMesh geometry;
    const auto vertex = [&](Vector3 position, Vector3 normal, float u, float v)
    {
        Vertex value{};
        value.position[0]=position.x; value.position[1]=position.y; value.position[2]=position.z;
        value.normal[0]=normal.x; value.normal[1]=normal.y; value.normal[2]=normal.z;
        value.uv[0]=u; value.uv[1]=v;
        geometry.vertices.push_back(value);
    };
    
    if(primitive == ScenePrimitive::Cube)
    {
        // Each face has its own normals and UV island; cross(u,v) points outward.
        const Vector3 normals[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        const Vector3 axesU[]={{0,0,-1},{0,0,1},{1,0,0},{1,0,0},{1,0,0},{-1,0,0}};
        const Vector3 axesV[]={{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
        for(unsigned int face=0; face<6; ++face)
        {
            const auto base=static_cast<uint32_t>(geometry.vertices.size());
            for(unsigned int corner=0; corner<4; ++corner)
            {
                const float u=(corner==1 || corner==2) ? 1.0f : 0.0f;
                const float v=corner>=2 ? 1.0f : 0.0f;
                vertex(normals[face]*0.5f + axesU[face]*(u-0.5f) + axesV[face]*(v-0.5f), normals[face], u, v);
            }
            geometry.indices.insert(geometry.indices.end(), {base,base+1,base+2,base,base+2,base+3});
        }
    }
    else if(primitive == ScenePrimitive::Sphere)
    {
        constexpr unsigned int latitudeSegments=24, longitudeSegments=48;
        for(unsigned int latitude=0; latitude<=latitudeSegments; ++latitude)
        {
            const float v=static_cast<float>(latitude)/latitudeSegments;
            const float theta=v*std::numbers::pi_v<float>;
            for(unsigned int longitude=0; longitude<=longitudeSegments; ++longitude)
            {
                const float u=static_cast<float>(longitude)/longitudeSegments;
                const float phi=(longitude==longitudeSegments ? 0.0f : u*2.0f*std::numbers::pi_v<float>);
                // Exact poles/seam avoid cracks; one triangle per pole wedge avoids zero-area faces.
                const Vector3 normal=latitude==0 ? Vector3{0,1,0} : latitude==latitudeSegments ? Vector3{0,-1,0} :
                    Vector3{std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi)};
                vertex(normal*0.5f, normal, u, v);
            }
        }
        for(unsigned int latitude=0; latitude<latitudeSegments; ++latitude)
            for(unsigned int longitude=0; longitude<longitudeSegments; ++longitude)
            {
                const uint32_t first=latitude*(longitudeSegments+1)+longitude;
                const uint32_t second=first+longitudeSegments+1;
                if(latitude!=0) geometry.indices.insert(geometry.indices.end(),{first,first+1,second});
                if(latitude!=latitudeSegments-1) geometry.indices.insert(geometry.indices.end(),{second,first+1,second+1});
            }
    }
    else throw std::invalid_argument("Unsupported scene primitive.");
    TangentGenerator::GenerateTangents(geometry.vertices,geometry.indices);
    geometry.boundsMin={-0.5f,-0.5f,-0.5f}; geometry.boundsMax={0.5f,0.5f,0.5f};
    return geometry;
}

namespace
{
void ExportSource(const ImportedMesh& geometry, const std::filesystem::path& path)
{
    aiScene scene;
    scene.mRootNode=new aiNode("Primitive");
    scene.mRootNode->mNumMeshes=1; scene.mRootNode->mMeshes=new unsigned int[1]{0};
    scene.mNumMaterials=1; scene.mMaterials=new aiMaterial*[1]{new aiMaterial};
    scene.mNumMeshes=1; scene.mMeshes=new aiMesh*[1]{new aiMesh};
    auto& mesh=*scene.mMeshes[0];
    mesh.mName=aiString("Primitive"); mesh.mPrimitiveTypes=aiPrimitiveType_TRIANGLE;
    mesh.mNumVertices=static_cast<unsigned int>(geometry.vertices.size());
    mesh.mVertices=new aiVector3D[mesh.mNumVertices]; mesh.mNormals=new aiVector3D[mesh.mNumVertices];
    mesh.mTextureCoords[0]=new aiVector3D[mesh.mNumVertices]; mesh.mNumUVComponents[0]=2;
    for(unsigned int index=0; index<mesh.mNumVertices; ++index)
    {
        const auto& v=geometry.vertices[index];
        mesh.mVertices[index]={v.position[0],v.position[1],v.position[2]};
        mesh.mNormals[index]={v.normal[0],v.normal[1],v.normal[2]};
        mesh.mTextureCoords[0][index]={v.uv[0],v.uv[1],0};
    }
    mesh.mNumFaces=static_cast<unsigned int>(geometry.indices.size()/3);
    mesh.mFaces=new aiFace[mesh.mNumFaces];
    for(unsigned int index=0; index<mesh.mNumFaces; ++index)
    {
        mesh.mFaces[index].mNumIndices=3;
        mesh.mFaces[index].mIndices=new unsigned int[3]{geometry.indices[index*3],geometry.indices[index*3+1],geometry.indices[index*3+2]};
    }
    Assimp::Exporter exporter;
    if(exporter.Export(&scene,"fbxa",path.string())!=aiReturn_SUCCESS)
        throw std::runtime_error(std::string("Primitive FBX export failed: ")+exporter.GetErrorString());
}
}

bool Ensure(const ProjectPaths& paths, ProjectAssetRegistry& registry, ScenePrimitive primitive,
    std::string& reference, std::string& error)
{
    reference.clear(); error.clear();
    try
    {
        const auto filename=std::string("EnginePrimitive_")+Name(primitive)+"_v1.fbx";
        const auto relative=std::filesystem::path("Meshes")/filename;
        if(const auto* record=registry.FindByPath(relative))
        {
            if(record->type!=AssetType::Mesh) throw std::runtime_error("Primitive source path is registered as a different asset type.");
            const auto resolved=ProjectAssetResolver::Resolve(paths,registry,record->id);
            if(resolved.status!=AssetAvailability::Available) throw std::runtime_error("Registered primitive source is unavailable.");
            reference=record->id.ToReference();
            return true;
        }
        if(std::filesystem::exists(paths.GetRoot()/relative))
            throw std::runtime_error("Primitive source path already exists without registration; reconcile it before creating primitives.");
        const auto id=AssetID::Generate();
        if(!id) throw std::runtime_error("Cannot reserve primitive staging identity.");
        const auto directory=std::filesystem::temp_directory_path()/("Engine-primitive-"+id->ToString());
        if(!std::filesystem::create_directory(directory)) throw std::runtime_error("Cannot reserve primitive staging directory.");
        struct Cleanup
        {
            std::filesystem::path source, directory;
            ~Cleanup() { std::error_code ignored; std::filesystem::remove(source,ignored); std::filesystem::remove(directory,ignored); }
        } cleanup{directory/filename,directory};
        ExportSource(Generate(primitive),cleanup.source);
        // Validate through the exact production importer and cooked-mesh contract before registration.
        const auto imported=ModelImporter::LoadFlattened(cleanup.source,MeshUVOrientation::FlipV);
        const auto cooked=CookedMesh::Encode(imported,{});
        if(cooked.status!=CookedMesh::Status::Success) throw std::runtime_error(std::string(cooked.message));
        std::filesystem::path importedPath;
        if(!ProjectMeshImportService::Import(paths,registry,cleanup.source,importedPath,error)) return false;
        const auto* record=registry.FindByPath(importedPath.lexically_relative(paths.GetRoot()));
        if(!record) throw std::runtime_error("Imported primitive registration is absent.");
        reference=record->id.ToReference();
        return true;
    }
    catch(const std::exception& exception) { error=exception.what(); return false; }
}
}

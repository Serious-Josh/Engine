#pragma once

#include <vector>
#include <map>
#include <utility>
#include "assets/ImportedMesh.h"

struct Material;

class DescriptorHeap;
class ID3D12Device;
class ResourceUploader;
class Scene;
class Shader;
class Mesh;

class IDemoScene
{
    public:
    virtual ~IDemoScene() = default;

    virtual bool Initialize(
        ID3D12Device* device,
        ResourceUploader& uploader,
        DescriptorHeap& descriptorHeap,
        const Shader* vertexShader,
        const Shader* pixelShader) = 0;

    virtual void Shutdown() = 0;
    virtual void Update(float deltaTime) = 0;

    virtual Scene& GetScene() = 0;
    virtual const Scene& GetScene() const = 0;

    virtual std::vector<Material*> GetMaterials() = 0;

    virtual void ResetCamera() = 0;
    virtual void ResetDirectionalLight() = 0;

    virtual void SetAspectRatio(float aspectRatio) = 0;

    // Main-thread borrow for a synchronous query only. Never loads a source or
    // reads GPU buffers. File scenes override this with their immutable catalog.
    virtual const ImportedMesh* GetMeshGeometry(const Mesh& mesh) const noexcept
    {
        const auto found = m_editorGeometry.find(&mesh);
        return found == m_editorGeometry.end() ? nullptr : &found->second;
    }

protected:
    void RetainEditorGeometry(const Mesh& mesh, ImportedMesh geometry)
    {
        m_editorGeometry.insert_or_assign(&mesh, std::move(geometry));
    }
    void RetainEditorGeometry(const Mesh& mesh, std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    {
        ImportedMesh geometry;
        geometry.vertices = std::move(vertices);
        geometry.indices = std::move(indices);
        RetainEditorGeometry(mesh, std::move(geometry));
    }
    void ClearEditorGeometry() noexcept { m_editorGeometry.clear(); }

private:
    std::map<const Mesh*, ImportedMesh> m_editorGeometry;
};

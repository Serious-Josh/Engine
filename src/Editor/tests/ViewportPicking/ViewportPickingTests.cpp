#include "Editor/ViewportPicking.h"
#include "Editor/SceneSelection.h"
#include "DemoContent/IDemoScene.h"
#include "Scene/Scene.h"
#include "Renderer/Geometry/SphereGenerator.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace Picking = ViewportPicking;
static constexpr size_t None = (std::numeric_limits<size_t>::max)();
static void Check(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
static bool Near(double a, double b) { return std::abs(a - b) < 0.0001; }

static ImportedMesh Triangle(float z)
{
    ImportedMesh geometry;
    geometry.vertices = {{{-1, -1, z}}, {{1, -1, z}}, {{0, 1, z}}};
    geometry.indices = {0, 1, 2};
    geometry.metadata = DefaultMeshMetadata(3);
    return geometry;
}

struct Candidate
{
    std::string id;
    const ImportedMesh* geometry;
    MeshBounds bounds{{-10, -10, -10}, {10, 10, 20}, true};
    Transform transform;
};

static size_t Pick(const Picking::Ray& ray, const std::vector<Candidate>& candidates)
{
    return Picking::PickNearest(candidates.size(), ray.maxDistance, [&](size_t index, double& nearest)
    {
        const auto& item = candidates[index];
        return item.geometry && Picking::IntersectMesh(ray, item.transform.GetModelMatrix(), item.bounds,
            *item.geometry, !item.geometry->indices.empty(), item.geometry->metadata.submeshes, nearest);
    });
}

// Exercises the same scene-owned immutable CPU storage used by static demo meshes.
class FixtureScene final : public IDemoScene
{
public:
    Scene scene;
    Mesh mesh;
    bool Initialize(ID3D12Device*, ResourceUploader&, DescriptorHeap&, const Shader*, const Shader*) override { return false; }
    void Shutdown() override { ClearEditorGeometry(); }
    void Update(float) override {}
    Scene& GetScene() override { return scene; }
    const Scene& GetScene() const override { return scene; }
    std::vector<Material*> GetMaterials() override { return {}; }
    void ResetCamera() override {}
    void ResetDirectionalLight() override {}
    void SetAspectRatio(float) override {}
    void Publish(ImportedMesh geometry) { RetainEditorGeometry(mesh, std::move(geometry)); }
};

int main()
{
    try
    {
        const Picking::Ray ray{{0, 0, 0}, {0, 0, 1}, 100};
        auto outer = Triangle(10), inner = Triangle(3), front = Triangle(1);
        std::vector<Candidate> candidates{{"outer", &outer}, {"inner", &inner}};
        candidates[0].bounds = {{-10, -10, 1}, {10, 10, 20}, true};
        candidates[1].bounds = {{-1, -1, 3}, {1, 1, 3}, true};
        Check(Pick(ray, candidates) == 1, "Inner surface must beat enclosing AABB");
        candidates.push_back({"occluder", &front});
        Check(Pick(ray, candidates) == 2, "Genuine closer surface occlusion");
        std::swap(candidates[0], candidates[2]);
        Check(Pick(ray, candidates) == 0, "Nearest result independent of object order");
        candidates = {{"first", &inner}, {"equal", &inner}};
        Check(Pick(ray, candidates) == 0, "Exact depth ties keep first candidate");

        auto miss = Triangle(2);
        for(auto& vertex : miss.vertices) vertex.position[0] += 5;
        candidates = {{"bounds-only", &miss}, {"inner", &inner}};
        Check(Pick(ray, candidates) == 1, "AABB miss must not override a real hit");
        candidates.pop_back();
        Check(Pick(ray, candidates) == None, "AABB intersection is not a surface hit");
        candidates = {{"unavailable", nullptr}, {"inner", &inner}};
        Check(Pick(ray, candidates) == 1, "Unavailable streaming geometry cannot win by bounds");
        candidates.pop_back();
        Check(Pick(ray, candidates) == None, "Unavailable-only scene produces no pick");

        double distance = 100;
        const MeshBounds bounds{{-10, -10, -10}, {10, 10, 20}, true};
        auto hit = [&](const ImportedMesh& geometry, const Transform& transform)
        {
            distance = 100;
            return Picking::IntersectMesh(ray, transform.GetModelMatrix(), bounds, geometry, true,
                geometry.metadata.submeshes, distance);
        };
        Check(hit(inner, {}) && Near(distance, 3), "Ray originating inside bounds");
        Transform transform;
        transform.position.z = 4;
        Check(hit(inner, transform) && Near(distance, 7), "Translated geometry");
        transform = {};
        transform.rotation.y = 3.14159265359f;
        transform.position.z = 8;
        Check(hit(inner, transform) && Near(distance, 5), "Rotation and backface intersection");
        transform = {};
        transform.scale = {2, 0.5f, 3};
        Check(hit(inner, transform) && Near(distance, 9), "Nonuniform scale uses world-ray distance");
        transform.scale.x = -2;
        Check(hit(inner, transform) && Near(distance, 9), "Mirrored scale");
        transform.scale = {0, 1, 1};
        Check(!hit(inner, transform), "Collapsed triangle is degenerate");
        auto behind = Triangle(-2);
        Check(!hit(behind, {}), "Reject behind-camera triangles");
        auto tooFar = Triangle(101);
        Check(!hit(tooFar, {}), "Respect far clip bound");

        auto invalid = inner;
        invalid.vertices[2] = invalid.vertices[1];
        Check(!hit(invalid, {}), "Degenerate triangles");
        invalid = inner; invalid.indices[2] = 999;
        Check(!hit(invalid, {}), "Invalid indices");
        invalid = inner; invalid.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
        Check(!hit(invalid, {}), "Nonfinite vertices");
        Check(!hit(ImportedMesh{}, {}), "Empty geometry");
        invalid = inner;
        invalid.indices.insert(invalid.indices.end(), inner.indices.begin(), inner.indices.end());
        invalid.metadata = {{{0, 3, 0, "first"}, {3, 3, 0, "second"}}, {{"default", "Default", UINT32_MAX}}};
        Check(hit(invalid, {}), "Multiple submeshes");
        // The first range misses; the second range alone hits. Unrendered indices are excluded.
        auto multi = miss;
        multi.vertices.insert(multi.vertices.end(), inner.vertices.begin(), inner.vertices.end());
        multi.indices = {0, 1, 2, 3, 4, 5};
        multi.metadata = invalid.metadata;
        Check(hit(multi, {}), "Visit all rendered submeshes");
        multi.metadata.submeshes.resize(1);
        Check(!hit(multi, {}), "Do not pick triangles outside rendered ranges");
        auto nonindexed = inner;
        nonindexed.indices.clear();
        distance = 100;
        Check(Picking::IntersectMesh(ray, Matrix4::Identity(), bounds, nonindexed, false, {}, distance) && Near(distance, 3),
            "Nonindexed triangle lists");

        Camera camera;
        Picking::Ray fromCamera;
        Check(Picking::BuildPickingRay(camera, 320, 240, 640, 480, fromCamera), "Viewport unprojection");
        Check(fromCamera.direction.z > 0.999f && Near(fromCamera.direction.Length(), 1), "Center ray uses +Z and normalized direction");
        Check(!Picking::BuildPickingRay(camera, 0, 0, 0, 480, fromCamera), "Invalid viewport");
        camera.SetTarget(camera.GetPosition());
        Check(!Picking::BuildPickingRay(camera, 320, 240, 640, 480, fromCamera), "Singular camera is not an identity ray");

        // Store stable IDs, never the index returned by a previous pick.
        candidates = {{"outer-id", &outer}, {"inner-id", &inner}};
        std::vector<SceneSelection::Identity> ordered;
        SceneSelection::Select(ordered, SceneSelection::Identity{candidates[Pick(ray, candidates)].id}, false);
        std::reverse(candidates.begin(), candidates.end());
        Check(candidates[Pick(ray, candidates)].id == ordered.back().objectID, "Selection identity survives object reorder");
        SceneSelection::Select(ordered, SceneSelection::Identity{"outer-id"}, true);
        SceneSelection::Select(ordered, SceneSelection::Identity{"outer-id"}, true);
        Check(ordered.size() == 1 && ordered.back().objectID == "inner-id", "Shift-toggle integration preserves active fallback");

        FixtureScene fixture;
        Check(!fixture.GetMeshGeometry(fixture.mesh), "Geometry unavailable before publication");
        auto owned = Triangle(4);
        const auto* originalStorage = owned.vertices.data();
        fixture.Publish(std::move(owned));
        Check(fixture.GetMeshGeometry(fixture.mesh)->vertices.data() == originalStorage, "Retain source arrays without duplicate allocation");
        fixture.Shutdown();
        Check(!fixture.GetMeshGeometry(fixture.mesh), "Retired geometry is unavailable after scene shutdown");

        auto sphere = SphereGenerator::Generate(1, 32, 64);
        ImportedMesh representative;
        representative.vertices = std::move(sphere.vertices);
        representative.indices = std::move(sphere.indices);
        representative.metadata = DefaultMeshMetadata(static_cast<uint32_t>(representative.indices.size()));
        const MeshBounds sphereBounds{{-1, -1, -1}, {1, 1, 1}, true};
        const Picking::Ray sphereRay{{0.1f, 0.1f, -5}, {0, 0, 1}, 100};
        const auto started = std::chrono::steady_clock::now();
        for(int iteration = 0; iteration < 100; ++iteration)
        {
            distance = 100;
            Check(Picking::IntersectMesh(sphereRay, Matrix4::Identity(), sphereBounds, representative,
                true, representative.metadata.submeshes, distance), "Representative sphere pick");
        }
        const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        std::cout << "PASS: geometry/depth/transforms/ranges, unavailable and retired geometry, stable IDs, viewport rays\n";
        std::cout << representative.indices.size() / 3 << " triangles: " << milliseconds / 100 << " ms per Debug CPU pick (100 queries)\n";
        return 0;
    }
    catch(const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}

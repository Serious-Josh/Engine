#include "Editor/Editor.h"
#include "Editor/ViewportMarquee.h"
#include "DemoContent/IDemoScene.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneAssetResolver.h"
#include "Renderer/Material.h"
#include "imgui.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}
class CpuMarqueeScene final : public IDemoScene
{
public:
    Scene scene;
    Mesh mesh;
    ImportedMesh geometry;
    bool Initialize(ID3D12Device*, ResourceUploader&, DescriptorHeap&, const Shader*, const Shader*) override { return false; }
    void Shutdown() override {}
    void Update(float) override {}
    Scene& GetScene() override { return scene; }
    const Scene& GetScene() const override { return scene; }
    std::vector<Material*> GetMaterials() override { return {}; }
    void ResetCamera() override {}
    void ResetDirectionalLight() override {}
    void SetAspectRatio(float aspect) override { scene.GetCamera().SetAspectRatio(aspect); }
    const ImportedMesh* GetMeshGeometry(const Mesh& requested) const noexcept override
    {
        return &requested == &mesh ? &geometry : nullptr;
    }
};
class EmptyMeshResolver final : public SceneAssetResolver
{
public:
    Material material;
    Mesh* ResolveMesh(std::string_view) override { return nullptr; }
    Material* ResolveMaterial(std::string_view) override { return &material; }
};

void VerifyProjectionAndModifiers()
{
    using namespace ViewportMarquee;
    for(ImVec2 cursor : {ImVec2(20,20), ImVec2(-20,20), ImVec2(20,-20), ImVec2(-20,-20)})
    {
        const auto rectangle = Normalize({0,0}, cursor);
        Check(rectangle.minimum.x <= rectangle.maximum.x && rectangle.minimum.y <= rectangle.maximum.y, "Drag direction normalization failed");
    }
    Check(Overlaps({{0,0},{10,10}}, {{9,9},{20,20}}), "Partial overlap was excluded");
    Check(!Overlaps({{0,0},{10,10}}, {{11,11},{20,20}}), "Non-overlap included");
    const MeshBounds bounds{{-1,-1,-1},{1,1,1},true};
    Camera camera;
    camera.SetPosition({0,0,0});
    camera.SetTarget({0,0,1});
    camera.SetAspectRatio(1);
    camera.SetNearPlane(0.1f);
    camera.SetFarPlane(100);
    const Matrix4 viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    ViewportMarquee::Rectangle projected;
    Check(ProjectBounds(bounds, viewProjection * Matrix4::Translation({0,0,4}), {100,100}, projected), "Visible bounds excluded");
    Check(!ProjectBounds(bounds, viewProjection * Matrix4::Translation({0,0,-4}), {100,100}, projected), "Behind-camera bounds included");
    Check(ProjectBounds(bounds, viewProjection * Matrix4::Translation({0,0,0.5f}), {100,100}, projected), "Near-plane crossing excluded");
    Check(projected.minimum.x >= 0 && projected.maximum.x <= 100, "Near-plane bounds exceeded viewport");
    Check(ProjectBounds(bounds, viewProjection * Matrix4::Translation({2.5f,0,4}), {100,100}, projected), "Partially offscreen bounds excluded");
    Check(!ProjectBounds(bounds, viewProjection * Matrix4::Translation({30,0,4}), {100,100}, projected), "Fully offscreen bounds included");
    Check(ProjectBounds({{-1000,-1000,-1000},{1000,1000,1000},true}, viewProjection, {100,100}, projected) &&
        projected.minimum.x < 0.01f && projected.maximum.x > 99.99f, "Frustum-enclosing box was excluded");
    Check(ProjectBounds({{0,0,0},{0,0,0},true}, viewProjection * Matrix4::Translation({0,0,4}), {100,100}, projected), "Degenerate point bounds excluded");
    Matrix4 invalid = viewProjection;
    invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
    Check(!ProjectBounds(bounds, invalid, {100,100}, projected), "Nonfinite projection accepted");
    Check(!ProjectBounds(bounds, Matrix4{}, {100,100}, projected), "Singular projection accepted");
    const SceneSelection::Identity first{"first"}, second{"second"}, third{"third"};
    const std::vector<SceneSelection::Identity> original{first,second}, candidates{second,third};
    std::vector<SceneSelection::Identity> result;
    CombineSelection(original,candidates,SelectionMode::Replace,result);
    Check(result == candidates, "Plain replacement failed");
    CombineSelection(original,candidates,SelectionMode::Add,result);
    Check(result == std::vector<SceneSelection::Identity>{first,second,third}, "Shift addition ordering failed");
    CombineSelection(original,candidates,SelectionMode::Toggle,result);
    Check(result == std::vector<SceneSelection::Identity>{first,third}, "Ctrl snapshot toggle failed");
    CombineSelection(original,candidates,SelectionMode::Toggle,result);
    Check(result == std::vector<SceneSelection::Identity>{first,third}, "Ctrl inverted again on another preview frame");
    CombineSelection(original,{},SelectionMode::Replace,result);
    Check(result.empty(), "Empty marquee did not replace selection");
}
}

class MarqueeSelectionVerification
{
public:
    static void Run()
    {
        VerifyProjectionAndModifiers();
        VerifyClickModifiers();
        CpuMarqueeScene fixture;
        fixture.mesh.m_bounds = {{-0.5f,-0.5f,-0.5f},{0.5f,0.5f,0.5f},true};
        fixture.scene.GetCamera().SetPosition({0,0,0});
        fixture.scene.GetCamera().SetTarget({0,0,1});
        fixture.scene.GetCamera().SetAspectRatio(1);
        RenderObject first;
        first.mesh = &fixture.mesh;
        first.transform.position = {-1,0,5};
        fixture.scene.AddRenderObject(first);
        auto second = first;
        second.transform.position = {1,0,5};
        fixture.scene.AddRenderObject(second);
        Editor editor;
        editor.m_viewportImage = {{100,100},{200,200}};
        editor.SetViewportPublicationIdentity(1,2);
        editor.m_orderedSelection = {{{},1}};
        editor.RefreshSceneSelection(fixture);
        const auto committed = editor.m_orderedSelection;
        Start(editor,fixture);
        editor.m_marqueeGesture->cursor = {100,200};
        editor.RefreshViewportMarqueePreview(fixture);
        const auto* preview = editor.GetViewportMarqueePreview(fixture);
        Check(preview && *preview == std::vector<std::size_t>{0}, "Live preview membership/order is incorrect");
        Check(editor.m_orderedSelection == committed && editor.m_sceneHistory.empty(), "Preview changed committed selection or history");
        editor.m_marqueeGesture->cursor = {200,200};
        editor.RefreshViewportMarqueePreview(fixture);
        Check(*editor.GetViewportMarqueePreview(fixture) == std::vector<std::size_t>{0,1}, "Growing rectangle did not add preview member");
        editor.m_marqueeGesture->anchor = {110,0};
        editor.RefreshViewportMarqueePreview(fixture);
        Check(*editor.GetViewportMarqueePreview(fixture) == std::vector<std::size_t>{1}, "Leaving rectangle retained stale highlight");
        editor.CancelViewportMarquee();
        Check(!editor.GetViewportMarqueePreview(fixture) && editor.m_orderedSelection == committed, "Cancellation did not restore committed highlights");

        Start(editor,fixture);
        fixture.scene.AddRenderObject({});
        editor.RefreshViewportMarqueePreview(fixture);
        Check(!editor.m_marqueeGesture, "Structure mutation retained marquee identities");
        Start(editor,fixture);
        editor.SetViewportPublicationIdentity(1,3);
        Check(!editor.m_marqueeGesture, "Renderer binding publication retained preview");
        Start(editor,fixture);
        editor.m_viewportImage.size.x += 1;
        editor.RefreshViewportMarqueePreview(fixture);
        Check(!editor.m_marqueeGesture, "Viewport resize retained obsolete marquee coordinates");
        editor.m_viewportImage.size.x -= 1;
        Start(editor,fixture);
        CpuMarqueeScene replacement;
        editor.RefreshViewportMarqueePreview(replacement);
        Check(!editor.m_marqueeGesture, "Scene replacement retained marquee");

        Physics::Pose simulatedPose;
        simulatedPose.position = {1,0,5};
        std::optional<Physics::Pose> poses[] = {simulatedPose, std::nullopt, std::nullopt};
        fixture.scene.PreparePresentationCommit();
        fixture.scene.CommitPresentation(poses,true);
        Start(editor,fixture);
        editor.m_marqueeGesture->cursor = {100,200};
        editor.RefreshViewportMarqueePreview(fixture);
        Check(editor.GetViewportMarqueePreview(fixture)->empty(), "Marquee used authored pose instead of accepted physics pose");
        fixture.scene.CommitPresentation({},false);
        editor.RefreshViewportMarqueePreview(fixture);
        Check(*editor.GetViewportMarqueePreview(fixture) == std::vector<std::size_t>{0}, "Stop did not refresh marquee placement");
        editor.CancelViewportMarquee();
        VerifyEscapeAndRelease(editor,fixture,committed);
        VerifyStableIdentityAndDirtyState();
    }
private:
    static void VerifyClickModifiers()
    {
        auto* context = ImGui::CreateContext();
        struct Cleanup { ImGuiContext* context; ~Cleanup() { ImGui::DestroyContext(context); } } cleanup{context};
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize = {800,600};
        io.DeltaTime = 1.0f/60;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        CpuMarqueeScene fixture;
        fixture.geometry.vertices = {{{-1,-1,0}},{{1,-1,0}},{{0,1,0}}};
        fixture.geometry.indices = {0,1,2};
        fixture.geometry.metadata = DefaultMeshMetadata(3);
        fixture.mesh.m_vertexCount = 3;
        fixture.mesh.m_indexCount = 3;
        fixture.mesh.m_metadata = fixture.geometry.metadata;
        fixture.mesh.m_bounds = {{-1,-1,0},{1,1,0},true};
        fixture.scene.GetCamera().SetPosition({0,0,0});
        fixture.scene.GetCamera().SetTarget({0,0,1});
        fixture.scene.GetCamera().SetAspectRatio(1);
        RenderObject object;
        object.mesh = &fixture.mesh;
        object.transform.position = {20,0,5};
        fixture.scene.AddRenderObject(object);
        object.transform.position = {0,0,5};
        fixture.scene.AddRenderObject(object);
        Editor editor;
        editor.m_viewportImage = {{100,100},{200,200}};
        editor.m_viewportHovered = true;
        const auto click = [&](bool shift, bool ctrl)
        {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);
            ImGui::NewFrame();
            ImGui::EndFrame();
            io.AddKeyEvent(ImGuiMod_Shift,shift);
            io.AddKeyEvent(ImGuiMod_Ctrl,ctrl);
            io.AddMousePosEvent(200,200);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);
            ImGui::NewFrame();
            editor.HandleViewportSelection(fixture,200,200);
            ImGui::EndFrame();
            Check(!editor.m_marqueeGesture, "Object click incorrectly started marquee");
        };
        for(const auto modifiers : {std::array{false,false}, std::array{true,false},
            std::array{false,true}, std::array{true,true}})
        {
            editor.SelectSceneObject(fixture,0);
            click(modifiers[0],modifiers[1]);
            const bool modified = modifiers[0] || modifiers[1];
            Check(editor.m_selectedObjects == (modified ? std::vector<size_t>{0,1} : std::vector<size_t>{1}),
                "Click membership/order differs from expected modifier behavior");
            Check(editor.m_selectedObject == 1, "Added object was not primary");
            click(modifiers[0],modifiers[1]);
            Check(editor.m_selectedObjects == (modified ? std::vector<size_t>{0} : std::vector<size_t>{1}),
                "Repeated click did not remove selected member exactly once");
            Check(editor.m_selectedObject == (modified ? 0 : 1), "Removal primary fallback changed");
            Check(editor.m_sceneHistory.empty(), "Click selection changed history");
        }
    }
    static void Start(Editor& editor, const IDemoScene& scene)
    {
        ViewportMarquee::Gesture gesture;
        gesture.active = true;
        gesture.anchor = {0,0};
        gesture.cursor = {200,200};
        gesture.image = editor.m_viewportImage;
        gesture.sceneIdentity = &scene.GetScene();
        gesture.structureRevision = scene.GetScene().GetStructureRevision();
        gesture.rendererLifetime = editor.m_viewportRendererLifetime;
        gesture.bindingEpoch = editor.m_viewportBindingEpoch;
        const auto& objects = scene.GetScene().GetRenderObjects();
        for(std::size_t index = 0; index < objects.size(); ++index)
        {
            if(const auto* owner = dynamic_cast<const FileBackedDemoScene*>(&scene))
            {
                const auto metadata = owner->GetObjectMetadata(index);
                Check(metadata.has_value(), "Cannot snapshot stable ID");
                gesture.objectIdentities.push_back({metadata->objectID});
                gesture.persistentIndices.emplace(metadata->objectID,index);
            }
            else gesture.objectIdentities.push_back({{},index});
        }
        gesture.originalSelection = editor.m_orderedSelection;
        editor.m_marqueeGesture = std::move(gesture);
    }
    static void VerifyEscapeAndRelease(Editor& editor, CpuMarqueeScene& fixture,
        const std::vector<SceneSelection::Identity>& committed)
    {
        auto* context = ImGui::CreateContext();
        struct ContextCleanup
        {
            ImGuiContext* context;
            ~ContextCleanup() { ImGui::DestroyContext(context); }
        } contextCleanup{context};
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize = {800,600};
        io.DeltaTime = 1.0f/60;
        unsigned char* fontPixels;
        int fontWidth, fontHeight;
        io.Fonts->GetTexDataAsRGBA32(&fontPixels,&fontWidth,&fontHeight);
        io.AddMousePosEvent(200,200);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,true);
        io.AddKeyEvent(ImGuiKey_Escape,true);
        ImGui::NewFrame();
        Start(editor,fixture);
        Check(ImGui::IsKeyPressed(ImGuiKey_Escape,false), "Escape fixture event was not delivered");
        Check(editor.UpdateViewportMarquee(fixture), "Escape gesture was not consumed");
        Check(!editor.m_marqueeGesture && editor.m_orderedSelection == committed, "Escape committed preview selection");
        ImGui::EndFrame();
        io.AddKeyEvent(ImGuiKey_Escape,false);
        io.AddMousePosEvent(300,300);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,false);
        ImGui::NewFrame();
        Start(editor,fixture);
        Check(editor.UpdateViewportMarquee(fixture), "Release gesture was not consumed");
        Check(!editor.m_marqueeGesture && editor.m_selectedObjects == std::vector<std::size_t>{0,1}, "Release did not commit preview selection");
        ImGui::EndFrame();
    }
    static void VerifyStableIdentityAndDirtyState()
    {
        FileBackedDemoScene owner(std::filesystem::path{});
        SceneDocument document;
        document.id = "marquee-stable-identity";
        SceneObjectDocument object;
        object.id = "persistent-object";
        document.objects.push_back(object);
        EmptyMeshResolver resolver;
        std::string error;
        Check(owner.m_runtime->m_fileScene.LoadDocument(document,&error), "Cannot create file-backed fixture");
        PreparedScene prepared;
        Check(PrepareScene(document,resolver,prepared,&error), "Cannot prepare fixture");
        Check(CommitPreparedEditorScene(prepared,document,owner.m_runtime->m_scene,owner.m_runtime->m_objectMapping,&error), "Cannot commit fixture");
        owner.m_initialized = true;
        Editor editor;
        editor.m_viewportImage = {{0,0},{200,200}};
        editor.SetViewportPublicationIdentity(10,20);
        editor.m_orderedSelection = {{"persistent-object"}};
        editor.RefreshSceneSelection(owner);
        Start(editor,owner);
        editor.m_marqueeGesture->runtimeIdentity = owner.GetRuntimeIdentity();
        editor.m_marqueeGesture->mode = ViewportMarquee::SelectionMode::Add;
        editor.RefreshViewportMarqueePreview(owner);
        Check(editor.GetViewportMarqueePreview(owner) && *editor.GetViewportMarqueePreview(owner) == std::vector<std::size_t>{0},
            "Stable selection identity did not resolve");
        Check(!owner.m_runtime->GetFileScene().IsDirty() && SceneDocumentsEqual(document,owner.m_runtime->GetFileScene().GetDocument()) &&
            editor.m_sceneHistory.empty(), "Preview mutated authored scene/physics, dirty state or history");
        editor.m_marqueeGesture->originalSelection = {{"deleted-object"}};
        editor.RefreshViewportMarqueePreview(owner);
        Check(!editor.m_marqueeGesture, "Missing stable identity was committed");
    }
};

int VerifyViewportMarquee()
{
    try
    {
        MarqueeSelectionVerification::Run();
        std::cout << "PASS: marquee normalization/overlap/frustum clipping, deterministic modifiers, live preview/commit/cancel, stable identity, scene/renderer/resize invalidation, physics presentation and authored-data preservation. CPU/ImGui fixture only; no GPU/editor initialization.\n";
        return 0;
    }
    catch(const std::exception& exception)
    {
        std::cerr << "FAIL: viewport marquee: " << exception.what() << '\n';
        return 1;
    }
}

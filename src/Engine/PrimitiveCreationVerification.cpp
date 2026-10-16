#include "Editor/ScenePrimitiveCreation.h"
#include "Editor/SceneObjectDuplication.h"
#include "Editor/Editor.h"
#include "DemoContent/FileBackedDemoScene.h"
#include "Scene/SceneDocumentAdapter.h"
#include "Scene/SceneSerializer.h"
#include "Scene/SceneAssetResolver.h"
#include "assets/AssetRegistrySerializer.h"
#include "assets/ModelImporter.h"
#include "assets/CookedScene.h"
#include "Project/ProjectPaths.h"
#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Engine/BuildGame.h"
#include "Platform/Windows/WindowsWindow.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
void Check(bool condition, const std::string& error)
{
    if(!condition) throw std::runtime_error(error);
}
void VerifyGeometry(const ImportedMesh& geometry, ScenePrimitive primitive)
{
    Check(!geometry.vertices.empty() && !geometry.indices.empty() && geometry.indices.size()%3==0,"Empty/non-triangle geometry");
    for(const auto& vertex:geometry.vertices)
    {
        const Vector3 position{vertex.position[0],vertex.position[1],vertex.position[2]};
        const Vector3 normal{vertex.normal[0],vertex.normal[1],vertex.normal[2]};
        Check(std::abs(normal.Length()-1.0f)<0.001f,"Invalid normal");
        for(float value:vertex.position) Check(std::isfinite(value) && std::abs(value)<=0.50001f,"Invalid bounds");
        for(float value:vertex.uv) Check(std::isfinite(value) && value>=-0.00001f && value<=1.00001f,"Invalid UV");
        for(float value:vertex.tangent) Check(std::isfinite(value),"Invalid tangent");
        if(primitive==ScenePrimitive::Sphere)
            Check(std::abs(position.Length()-0.5f)<0.00001f && Vector3::Dot(position,normal)>0.499f,"Invalid sphere radius/normal");
    }
    for(size_t index=0; index<geometry.indices.size(); index+=3)
    {
        Vector3 positions[3],normal;
        for(unsigned int corner=0; corner<3; ++corner)
        {
            Check(geometry.indices[index+corner]<geometry.vertices.size(),"Invalid index");
            const auto& v=geometry.vertices[geometry.indices[index+corner]];
            positions[corner]={v.position[0],v.position[1],v.position[2]};
            normal=normal+Vector3{v.normal[0],v.normal[1],v.normal[2]};
        }
        const auto cross=Vector3::Cross(positions[1]-positions[0],positions[2]-positions[0]);
        Check(cross.Length()>0.0000001f && Vector3::Dot(cross,normal)>0,"Degenerate/inward triangle");
    }
}

}

class PrimitiveCreationVerification
{
public:
    static void Run()
    {
        namespace fs=std::filesystem;
        const auto token=AssetID::Generate();
        Check(token.has_value(),"Cannot allocate fixture identity");
        const auto root=fs::absolute(fs::path("build")/("primitive-creation-"+token->ToString()));
        const auto project=root/"Project";
        ProjectPaths paths(project);
        std::string error;
        Check(paths.EnsureDirectories(&error),error);
        ProjectAssetRegistry registry;
        SceneDocument document;
        document.id="primitive-creation-verification";
        document.name="Primitive Creation";
        std::vector<std::string> references;
        for(auto primitive:{ScenePrimitive::Cube,ScenePrimitive::Sphere})
        {
            const auto a=PrimitiveMeshAssets::Generate(primitive);
            const auto b=PrimitiveMeshAssets::Generate(primitive);
            VerifyGeometry(a,primitive);
            Check(a.indices==b.indices && a.vertices.size()==b.vertices.size() &&
                std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(Vertex))==0,"Nondeterministic primitive geometry");
            std::string reference;
            Check(PrimitiveMeshAssets::Ensure(paths,registry,primitive,reference,error),error);
            references.push_back(reference);
            const auto* record=registry.Find(*AssetID::ParseReference(reference));
            Check(record && record->type==AssetType::Mesh,"Primitive registration absent");
            const auto imported=ModelImporter::LoadFlattened(project/record->relativePath,MeshUVOrientation::FlipV);
            VerifyGeometry(imported,primitive);
            const auto cooked=CookedMesh::Encode(imported,{});
            Check(cooked.status==CookedMesh::Status::Success,std::string(cooked.message));
            const auto decoded=CookedMesh::Decode(cooked.payload,{});
            Check(decoded.status==CookedMesh::Status::Success && decoded.geometry,"Cooked primitive decode failed");
            VerifyGeometry(*decoded.geometry,primitive);
            const auto object=PreparePrimitiveObject(document,primitive,reference);
            Check(!object.physics && object.name==PrimitiveMeshAssets::Name(primitive) && object.materialReference=="builtin:default",
                "Primitive defaults invalid");
            SceneObjectDocument expected=object;
            expected.transform=Transform{};
            SceneDocument expectedDoc,actualDoc;
            expectedDoc.objects={expected}; actualDoc.objects={object};
            Check(SceneDocumentsEqual(expectedDoc,actualDoc),"Primitive transform is not identity");
            document.objects.push_back(object);
            const auto second=PreparePrimitiveObject(document,primitive,reference);
            Check(second.name==std::string(PrimitiveMeshAssets::Name(primitive))+" (2)" && second.id!=object.id && second.meshReference==reference,
                "Primitive name/identity sharing failed");
        }
        ProjectAssetRegistry reopenedRegistry;
        Check(AssetRegistrySerializer::LoadFromFile(paths.GetAssetRegistryPath(),reopenedRegistry,&error),error);
        for(size_t index=0; index<references.size(); ++index)
        {
            std::string reference;
            Check(PrimitiveMeshAssets::Ensure(paths,reopenedRegistry,index==0 ? ScenePrimitive::Cube : ScenePrimitive::Sphere,reference,error) &&
                reference==references[index],"Restart changed primitive AssetID");
        }
        Check(reopenedRegistry.GetAssets(AssetType::Mesh).size()==2,"Repeated primitive use registered extra meshes");
        Check(SaveSceneDocument(document,paths.GetScenesDirectory()/"Primitives.scene",&error),error);
        SceneDocument loaded;
        Check(LoadSceneDocument(paths.GetScenesDirectory()/"Primitives.scene",loaded,&error) && SceneDocumentsEqual(document,loaded),
            "Primitive scene save/reload failed");
        VerifyHistory(project,document,registry);
        VerifyArchive(root,project,document,registry);
    }
private:
    static void VerifyHistory(const std::filesystem::path& project,const SceneDocument& authored,const ProjectAssetRegistry& registry)
    {
        FileBackedDemoScene owner(project/"Scenes/Primitives.scene");
        owner.m_projectRoot=project;
        // CPU-only fixture of the production pending-mesh catalog path; no GPU resources exist.
        owner.m_assets.m_initialized=true;
        owner.m_assets.SetProjectAssets(ProjectPaths(project),registry);
        auto& resolver=owner.m_assets;
        SceneDocument baseline=authored;
        baseline.objects.clear();
        SceneObjectDocument untouched;
        untouched.id="untouched-object";
        untouched.name="Untouched";
        untouched.transform.position={2.0f,3.0f,4.0f};
        baseline.objects.push_back(untouched);
        std::string error;
        Check(owner.m_runtime->m_fileScene.LoadDocument(baseline,&error),error);
        PreparedScene prepared;
        Check(PrepareScene(baseline,resolver,prepared,&error),error);
        Check(CommitPreparedEditorScene(prepared,baseline,owner.m_runtime->m_scene,owner.m_runtime->m_objectMapping,&error),error);
        owner.m_initialized=true;
        Editor editor;
        Check(editor.UpdateSceneHistorySession(owner,ProjectPaths(project),"Primitive Creation"),"Cannot establish primitive history");
        const auto insert=[&](const SceneObjectDocument& object,size_t index)
        {
            const auto operation=editor.AllocateSceneHistoryOperationID();
            Check(editor.StageObjectDeletionHistory(object,index,{},{object.id},operation,editor.m_historyEpoch,false,error,true),error);
            auto candidate=owner.m_runtime->PrepareObjectRestoration(resolver,object,index,&error);
            Check(candidate!=nullptr,error);
            owner.m_runtime=std::move(candidate);
            Check(editor.RebindSceneHistoryAfterStructureChange(owner),"Insertion history rebind failed");
            editor.CompleteObjectDeletionHistory(operation,editor.m_historyEpoch,true,{});
        };
        for(const auto& object:authored.objects) insert(object,owner.m_runtime->GetFileScene().GetDocument().objects.size());
        Check(editor.m_historyCursor==2 && owner.m_runtime->GetFileScene().IsDirty(),"Creation history/dirty tracking failed");
        SceneObjectDocument duplicate;
        size_t duplicateIndex=0;
        Check(PrepareSceneObjectDuplicate(*owner.m_runtime,authored.objects.front().id,duplicate,duplicateIndex,error),error);
        Check(duplicate.id!=authored.objects.front().id && duplicate.meshReference==authored.objects.front().meshReference && !duplicate.physics,
            "Primitive duplication changed mesh/physics/identity");
        SceneObjectDocument sphereDuplicate;
        size_t sphereDuplicateIndex = 0;
        Check(PrepareSceneObjectDuplicate(*owner.m_runtime, authored.objects.back().id, sphereDuplicate, sphereDuplicateIndex, error), error);
        Check(sphereDuplicate.id != authored.objects.back().id && sphereDuplicate.meshReference == authored.objects.back().meshReference &&
            sphereDuplicate.materialReference == authored.objects.back().materialReference && !sphereDuplicate.physics,
            "Sphere duplication changed geometry, material or defaults");
        insert(duplicate,duplicateIndex);
        const auto replay=[&](bool redo)
        {
            editor.ReplaySceneHistory(owner,redo);
            auto request=editor.TakeSceneHistoryReplayRequest();
            Check(request.has_value(),"Primitive history did not queue replay");
            auto candidate=request->remove ? owner.m_runtime->PrepareObjectDeletion(request->object.id,&error) :
                owner.m_runtime->PrepareObjectRestoration(resolver,request->object,request->insertionIndex,&error);
            Check(candidate!=nullptr,error);
            owner.m_runtime=std::move(candidate);
            Check(editor.RebindSceneHistoryAfterStructureChange(owner),"Replay history rebind failed");
            editor.CompleteSceneHistoryReplay(request->identity,true,{});
        };
        replay(false); // duplicate
        replay(false); // sphere
        replay(false); // cube
        Check(SceneDocumentsEqual(baseline,owner.m_runtime->GetFileScene().GetDocument()),"Creation undo changed unrelated scene data");
        replay(true); replay(true); replay(true);
        const auto restored=owner.m_runtime->GetFileScene().GetDocument();
        Check(restored.objects.size()==4 && restored.objects[1].id==authored.objects[0].id && restored.objects[3].id==authored.objects[1].id,
            "Creation redo lost stable identity");
        const auto deletion=editor.AllocateSceneHistoryOperationID();
        Check(editor.StageObjectDeletionHistory(duplicate,duplicateIndex,{},{},deletion,editor.m_historyEpoch,false,error),error);
        owner.m_runtime=owner.m_runtime->PrepareObjectDeletion(duplicate.id,&error);
        Check(owner.m_runtime!=nullptr,error);
        Check(editor.RebindSceneHistoryAfterStructureChange(owner),"Deletion history rebind failed");
        editor.CompleteObjectDeletionHistory(deletion,editor.m_historyEpoch,true,{});
        replay(false); replay(true);
        auto expected=baseline;
        expected.objects.insert(expected.objects.end(),authored.objects.begin(),authored.objects.end());
        Check(SceneDocumentsEqual(expected,owner.m_runtime->GetFileScene().GetDocument()),"Primitive deletion/restoration changed authored data");
        Check(!owner.GetScene().HasPhysicsPresentation(),"Primitive authoring changed presentation");
    }
    static void VerifyArchive(const std::filesystem::path& root,const std::filesystem::path& project,
        const SceneDocument& document,const ProjectAssetRegistry& registry)
    {
        BuildGame::Service service;
        std::string error;
        const auto output=root/"Game";
        BuildGame::Request request{{"PrimitiveCreation",output,false},document,registry,project,
            WindowsWindow::GetExecutableDirectory()/"assets/Shaders",std::filesystem::path(ENGINE_RUNTIME_DEPLOY_DIR)};
        Check(service.Start(std::move(request),error),error);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::minutes(3);
        auto status=service.Poll();
        while(status.busy && std::chrono::steady_clock::now()<deadline)
        { std::this_thread::sleep_for(std::chrono::milliseconds(20)); status=service.Poll(); }
        Check(!status.busy && status.success,status.message);
        std::filesystem::rename(project,root/"RetainedProject");
        const auto archive=CookedScene::OpenArchive(output/"Game.toc",output/"Game.eap",BuildGame::SceneIdentity(document.id));
        Check(archive->ValidateReferences(&error),error);
        Check(SceneDocumentsEqual(document,archive->metadata.document),"Archive changed primitive authored data");
        for(const auto& object:document.objects)
        {
            const auto id=*AssetID::ParseReference(object.meshReference);
            const auto payload=archive->provider->ReadEntry(id,EngineAssetPak::Type::Mesh,CookedMesh::V1::Version,160*1024*1024);
            const auto decoded=CookedMesh::Decode(payload,{});
            Check(decoded.status==CookedMesh::Status::Success && decoded.geometry,"Archive-only primitive resolution failed");
            VerifyGeometry(*decoded.geometry,object.name=="Cube" ? ScenePrimitive::Cube : ScenePrimitive::Sphere);
        }
        std::cout<<"Archive-only primitive assets verified without loose project: "<<output.string()<<'\n';
    }
};

int VerifyPrimitiveCreation()
{
    try
    {
        PrimitiveCreationVerification::Run();
        std::cout<<"PASS: primitive geometry, FBX import/cook, stable asset/object identities, names, defaults, history, duplication, deletion, persistence and archive-only resolution. CPU fixture; no GPU/editor initialization.\n";
        return 0;
    }
    catch(const std::exception& exception) { std::cerr<<"FAIL: primitive creation: "<<exception.what()<<'\n'; return 1; }
}

#include "SceneObjectDuplication.h"
#include "Scene/SceneHierarchyEdits.h"
#include "Scene/FileSceneRuntime.h"
#include "assets/AssetID.h"
#include <algorithm>
#include <unordered_set>
std::string UniqueDuplicateObjectName(std::string_view name,const SceneDocument& scene)
{
    return SceneHierarchy::DuplicateName(name, scene);
}
bool PrepareSceneObjectDuplicate(const FileSceneRuntime& scene,std::string_view objectID,
    SceneObjectDocument& duplicate,std::size_t& insertionIndex,std::string& error)
{
    SceneObjectDocument snapshot; std::size_t index=0;
    if(!scene.CaptureObjectDeletionSnapshot(objectID,snapshot,index,&error)) return false;
    if(snapshot.camera)
    {
        snapshot.camera->main = false;
    }

    const auto& document=scene.GetFileScene().GetDocument();
    snapshot.name=UniqueDuplicateObjectName(snapshot.name,document);
    do
    {
        const auto id=AssetID::Generate();
        if(!id) { error="Cannot generate duplicate scene-object identity."; return false; }
        snapshot.id=id->ToString();
    } while(scene.GetObjectMapping().idToRuntimeIndex.contains(snapshot.id));
    duplicate=std::move(snapshot); insertionIndex=index+1;
    return true;
}

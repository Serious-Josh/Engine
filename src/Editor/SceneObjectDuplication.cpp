#include "SceneObjectDuplication.h"
#include "Scene/FileSceneRuntime.h"
#include "assets/AssetID.h"
#include <algorithm>
#include <unordered_set>
std::string UniqueDuplicateObjectName(std::string_view name,const SceneDocument& scene)
{
    std::string base(name);
    const auto open=base.rfind('(');
    if(open!=std::string::npos && base.back()==')' && open+2<base.size() &&
        std::all_of(base.begin()+open+1,base.end()-1,[](char c) { return c>='0' && c<='9'; }))
    {
        base.resize(open);
        if(!base.empty() && base.back()==' ') base.pop_back();
        if(base.empty()) base="Object";
    }
    std::unordered_set<std::string> names;
    for(const auto& object:scene.objects) names.insert(object.name);
    for(std::size_t suffix=2;;++suffix)
    {
        auto candidate=base+" ("+std::to_string(suffix)+")";
        if(!names.contains(candidate)) return candidate;
    }
}
bool PrepareSceneObjectDuplicate(const FileSceneRuntime& scene,std::string_view objectID,
    SceneObjectDocument& duplicate,std::size_t& insertionIndex,std::string& error)
{
    SceneObjectDocument snapshot; std::size_t index=0;
    if(!scene.CaptureObjectDeletionSnapshot(objectID,snapshot,index,&error)) return false;
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

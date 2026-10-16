#include "ScenePrimitiveCreation.h"
#include "SceneObjectDuplication.h"
#include "assets/AssetID.h"
#include <algorithm>
#include <stdexcept>

SceneObjectDocument PreparePrimitiveObject(const SceneDocument& document, ScenePrimitive primitive,
    const std::string& meshReference)
{
    const auto meshID=AssetID::ParseReference(meshReference);
    if(!meshID || meshID->ToReference()!=meshReference) throw std::invalid_argument("Primitive requires a canonical mesh AssetID.");
    SceneObjectDocument object;
    object.name=PrimitiveMeshAssets::Name(primitive);
    if(std::any_of(document.objects.begin(),document.objects.end(),[&](const auto& existing) { return existing.name==object.name; }))
        object.name=UniqueDuplicateObjectName(object.name,document);
    for(unsigned int attempt=0; attempt<8; ++attempt)
    {
        const auto id=AssetID::Generate();
        if(id && std::none_of(document.objects.begin(),document.objects.end(),[&](const auto& existing) { return existing.id==id->ToString(); }))
        { object.id=id->ToString(); break; }
    }
    if(object.id.empty()) throw std::runtime_error("Cannot allocate a fresh primitive object identity.");
    object.meshReference=meshReference;
    object.materialReference="builtin:default";
    // SceneObjectDocument defaults provide identity transform and absent physics.
    return object;
}

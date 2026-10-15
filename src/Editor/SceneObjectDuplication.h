#pragma once
#include "Scene/SceneDocument.h"
#include <string_view>
class FileSceneRuntime;
std::string UniqueDuplicateObjectName(std::string_view,const SceneDocument&);
bool PrepareSceneObjectDuplicate(const FileSceneRuntime&,std::string_view objectID,
    SceneObjectDocument& duplicate,std::size_t& insertionIndex,std::string& error);

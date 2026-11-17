#pragma once

#include "assets/AssetID.h"

#include <string_view>

// W1.3: the one mapping from a scene document's persistent id to its asset identity. A canonical GUID id is its own identity;
// any other (legacy) id maps to a deterministic name-based (version 5) GUID of "Engine.Scene:" + id. Ids are never normalized:
// case, braces and whitespace are part of the id. Throws std::runtime_error for an empty id.
//
// Build Game's packaged scene identity (BuildGame::SceneIdentity) and scene migration both use it, so a migrated scene keeps
// the asset identity its packaged builds already had.
AssetID SceneAssetIdentity(std::string_view persistentID);

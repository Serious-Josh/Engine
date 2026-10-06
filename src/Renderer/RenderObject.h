#pragma once

#include "Scene/Transform.h"

class Mesh;
struct Material;

struct RenderObject
{
    Mesh* mesh = nullptr;
    Material* material = nullptr;

    Transform transform;
    Transform previousTransform;
};
#pragma once

#include "Scene/Transform.h"

class Mesh;
class Material;

struct RenderObject
{
    Mesh* mesh = nullptr;
    Material* material = nullptr;

    Transform transform;
};
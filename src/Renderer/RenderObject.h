#pragma once

class Mesh;
class Material;

struct RenderObject
{
    Mesh* mesh = nullptr;
    Material* material = nullptr;
};
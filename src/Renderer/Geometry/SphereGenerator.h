#pragma once

#include "Renderer/Vertex.h"

#include <vector>

struct SphereGeometry
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
};

class SphereGenerator
{
    public:
        static SphereGeometry  Generate(float radius, unsigned int latitudeSegments, unsigned int longitudeSegments);
};
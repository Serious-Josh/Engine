#include "SphereGenerator.h"

#include <cmath>

SphereGeometry SphereGenerator::Generate(float radius, unsigned int latitudeSegments, unsigned int longitudeSegments)
{
    SphereGeometry geometry;

    const float pi = 3.14159265358979323846f;

    for(unsigned int latitude = 0; latitude <= latitudeSegments; ++latitude){
        const float v = static_cast<float>(latitude) / static_cast<float>(latitudeSegments);
        const float theta = v * pi;

        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);

        for(unsigned int longitude = 0; longitude <= longitudeSegments; ++longitude){
            const float u = static_cast<float>(longitude) / static_cast<float>(longitudeSegments);
            const float phi = u * 2.0f * pi;

            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);

            const float x = sinTheta * cosPhi;
            const float y = cosTheta;
            const float z = sinTheta * sinPhi;

            Vertex vertex{};

            vertex.position[0] = x * radius;
            vertex.position[1] = y * radius;
            vertex.position[2] = z * radius;

            vertex.normal[0] = x;
            vertex.normal[1] = y;
            vertex.normal[2] = z;

            vertex.uv[0] = u;
            vertex.uv[1] = v;

            geometry.vertices.push_back(vertex);
        }
    }

    for(unsigned int latitude = 0; latitude < latitudeSegments; ++latitude){
        for(unsigned int longitude = 0; longitude < longitudeSegments; ++longitude){
            const unsigned int first =
                latitude * (longitudeSegments + 1) + longitude;

            const unsigned int second =
                first + longitudeSegments + 1;

            geometry.indices.push_back(first);
            geometry.indices.push_back(first + 1);
            geometry.indices.push_back(second);

            geometry.indices.push_back(second);
            geometry.indices.push_back(first + 1);
            geometry.indices.push_back(second + 1);
        }
    }

    return geometry;
}
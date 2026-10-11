#include "Renderer/Geometry/DisplacementMeshGenerator.h"
#include "Renderer/TangentGenerator.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <string>
#include <stdexcept>
#include <random>

namespace
{
    constexpr int resolution = 256;
    struct Point { int x, z; };
    using Triangle = std::array<std::uint32_t, 3>;

    std::string failureContext;

    void Require(bool condition, const char* message)
    {
        if(!condition) throw std::runtime_error(std::string(message) + failureContext);
    }

    Point Position(std::uint32_t index)
    {
        return {static_cast<int>(index / 257), static_cast<int>(index % 257)};
    }

    int Cross(Point a, Point b, Point c)
    {
        return (b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x);
    }

    // Exact integer separating-axis test: touching edges/vertices are allowed,
    // but any positive-area intersection between triangle interiors is rejected.
    bool Overlap(const Triangle& a, const Triangle& b)
    {
        for(const auto& triangle : {a, b})
        {
            for(int edge=0; edge<3; ++edge)
            {
                Point p=Position(triangle[edge]), q=Position(triangle[(edge+1)%3]);
                int nx=q.z-p.z, nz=p.x-q.x;
                auto interval = [&](const Triangle& t)
                {
                    std::array<int,3> dots{};
                    for(int k=0;k<3;++k) { auto v=Position(t[k]); dots[k]=nx*v.x+nz*v.z; }
                    return std::pair{*std::min_element(dots.begin(),dots.end()),
                        *std::max_element(dots.begin(),dots.end())};
                };
                auto ia=interval(a), ib=interval(b);
                if(ia.second<=ib.first || ib.second<=ia.first) return false;
            }
        }
        return true;
    }

    void Validate(const DisplacementGeometry& geometry)
    {
        Require(geometry.vertices.size()==66049 && geometry.patches.size()==64, "vertex/patch counts");
        Require(!geometry.indices.empty() && geometry.indices.size()%3==0, "index count");
        std::vector<bool> used(geometry.vertices.size(),false);
        std::vector<Triangle> triangles;
        std::set<Triangle> unique;
        struct Edge { unsigned count=0; int direction=0; unsigned firstPatch=0, lastPatch=0; };
        std::map<std::pair<unsigned,unsigned>,Edge> edges;
        std::vector<std::vector<unsigned>> buckets(resolution*resolution);
        long long twiceArea=0;
        unsigned patchIndex=0;
        for(std::size_t i=0;i<geometry.indices.size();i+=3)
        {
            while(patchIndex+1<geometry.patches.size() && i>=geometry.patches[patchIndex+1].firstIndex) ++patchIndex;
            const auto& patch=geometry.patches[patchIndex];
            failureContext=" at triangle " + std::to_string(i/3) + ", patch (" +
                std::to_string(patch.patchX) + "," + std::to_string(patch.patchZ) + ")";
            Triangle t{geometry.indices[i],geometry.indices[i+1],geometry.indices[i+2]};
            for(auto index:t) { Require(index<geometry.vertices.size(), "index bounds"); used[index]=true; }
            auto a=Position(t[0]),b=Position(t[1]),c=Position(t[2]);
            int area=Cross(a,b,c);
            Require(area<0, "nondegenerate, upward XZ winding");
            twiceArea-=area;
            auto sorted=t;std::sort(sorted.begin(),sorted.end());
            Require(unique.insert(sorted).second, "duplicate triangle");
            for(int k=0;k<3;++k)
            {
                auto from=t[k],to=t[(k+1)%3];
                auto& edge=edges[std::minmax(from,to)];
                if(edge.count==0) edge.firstPatch=patchIndex;
                edge.lastPatch=patchIndex;
                ++edge.count;edge.direction+=from<to ? 1 : -1;
            }
            const int minX=(std::min)({a.x,b.x,c.x}),maxX=(std::max)({a.x,b.x,c.x});
            const int minZ=(std::min)({a.z,b.z,c.z}),maxZ=(std::max)({a.z,b.z,c.z});
            std::set<unsigned> candidates;
            for(int x=minX;x<maxX;++x) for(int z=minZ;z<maxZ;++z)
                for(auto previous:buckets[x*resolution+z]) candidates.insert(previous);
            for(auto previous:candidates) Require(!Overlap(t,triangles[previous]), "overlapping triangle interiors");
            auto triangleIndex=static_cast<unsigned>(triangles.size());
            for(int x=minX;x<maxX;++x) for(int z=minZ;z<maxZ;++z)
                buckets[x*resolution+z].push_back(triangleIndex);
            triangles.push_back(t);
        }
        failureContext=" (whole-mesh projected area)";
        Require(twiceArea==2LL*resolution*resolution, "complete projected area coverage");
        unsigned perimeterLength=0;
        for(const auto& [indices,edge]:edges)
        {
            auto a=Position(indices.first),b=Position(indices.second);
            failureContext=" at grid edge ("+std::to_string(a.x)+","+std::to_string(a.z)+")-("+
                std::to_string(b.x)+","+std::to_string(b.z)+"), incident patches ("+
                std::to_string(geometry.patches[edge.firstPatch].patchX)+","+
                std::to_string(geometry.patches[edge.firstPatch].patchZ)+") and ("+
                std::to_string(geometry.patches[edge.lastPatch].patchX)+","+
                std::to_string(geometry.patches[edge.lastPatch].patchZ)+")";
            bool perimeter=(a.x==0 && b.x==0)||(a.x==resolution && b.x==resolution)||
                (a.z==0 && b.z==0)||(a.z==resolution && b.z==resolution);
            Require(perimeter ? edge.count==1 : edge.count==2 && edge.direction==0,
                "edge incidence/orientation (hole, unpaired interior edge, or nonmanifold edge)");
            if(perimeter) perimeterLength+=std::abs(b.x-a.x)+std::abs(b.z-a.z);
            // Only referenced vertices count: unused full-grid samples on coarse
            // edges are harmless. A referenced vertex inside an edge is a T-junction.
            int steps=std::gcd(std::abs(b.x-a.x),std::abs(b.z-a.z));
            for(int k=1;k<steps;++k)
            {
                int x=a.x+(b.x-a.x)*k/steps,z=a.z+(b.z-a.z)*k/steps;
                Require(!used[x*257+z], "T-junction at referenced vertex inside an edge");
            }
        }
        failureContext=" (outer perimeter)";
        Require(perimeterLength==4u*resolution, "outer perimeter coverage");
        std::size_t next=0;
        for(const auto& patch:geometry.patches)
        {
            failureContext=" at patch ("+std::to_string(patch.patchX)+","+std::to_string(patch.patchZ)+")";
            Require(patch.firstIndex==next && patch.indexCount%3==0, "patch index ranges");
            next+=patch.indexCount;
        }
        Require(next==geometry.indices.size(), "complete patch ranges");
    }
}

int main()
{
    std::vector<std::uint8_t> pixels(19*11);
    for(unsigned i=0;i<pixels.size();++i) pixels[i]=static_cast<std::uint8_t>((i*37+i/7)%256);
    DisplacementHeightmap heightmap{pixels.data(),17,11,19};
    std::vector<Vertex> referenceVertices;
    using LODMap = std::array<std::uint32_t,64>;
    unsigned passed=0, failed=0;
    std::array<unsigned,5> transitionCoverage{};
    auto stitchedEdges = [](const LODMap& lods, unsigned x, unsigned z)
    {
        if(lods[x*8+z]!=0) return 0u;
        return unsigned(x>0 && lods[(x-1)*8+z]==1) +
            unsigned(x<7 && lods[(x+1)*8+z]==1) +
            unsigned(z>0 && lods[x*8+z-1]==1) +
            unsigned(z<7 && lods[x*8+z+1]==1);
    };
    auto runMap = [&](const std::string& name, const LODMap& lods)
    {
        for(unsigned x=0;x<8;++x) for(unsigned z=0;z<8;++z)
            if(lods[x*8+z]==0) ++transitionCoverage[stitchedEdges(lods,x,z)];
        const char* phase="generation";
        try
        {
            failureContext.clear();
            DisplacementMeshSettings settings{};settings.patchLODs=lods;
            auto geometry=DisplacementMeshGenerator::Generate(heightmap,settings);
            phase="before tangents";
            Validate(geometry);
            if(referenceVertices.empty()) referenceVertices=geometry.vertices;
            failureContext=" (cross-configuration vertex comparison)";
            Require(std::memcmp(referenceVertices.data(),geometry.vertices.data(),geometry.vertices.size()*sizeof(Vertex))==0,
                "positions, UVs, normals changed with patch topology");
            if(name=="L shape")
            {
                auto defaults=DisplacementMeshGenerator::Generate(heightmap,{});
                Require(defaults.indices==geometry.indices, "default LOD selection changed");
            }
            phase="after tangents";
            TangentGenerator::GenerateTangents(geometry.vertices,geometry.indices);
            Validate(geometry);
            ++passed;
            std::printf("PASS %-26s vertices=%zu indices=%zu triangles=%zu patches=%zu\n",name.c_str(),
                geometry.vertices.size(),geometry.indices.size(),geometry.indices.size()/3,geometry.patches.size());
        }
        catch(const std::exception& error)
        {
            ++failed;
            std::printf("FAIL %s [%s]: %s\n",name.c_str(),phase,error.what());
            std::puts("Reproducible LOD map: rows are patchX=0..7; columns are patchZ=0..7:");
            for(unsigned x=0;x<8;++x) { for(unsigned z=0;z<8;++z) std::printf("%u",lods[x*8+z]); std::puts(""); }
            std::puts("Fine patches with multiple coarser neighbors (x,z: stitched-edge count):");
            for(unsigned x=0;x<8;++x) for(unsigned z=0;z<8;++z)
            {
                unsigned count=stitchedEdges(lods,x,z);
                if(count>=2) std::printf("(%u,%u): %u\n",x,z,count);
            }
        }
        std::fflush(stdout);
    };
    auto run = [&](const char* name, std::initializer_list<std::pair<unsigned,unsigned>> coarse)
    {
        LODMap lods{};
        for(auto [x,z]:coarse) lods[x*8+z]=1;
        runMap(name,lods);
    };
    run("all LOD 0",{});
    run("one LOD 1",{{3,3}});
    run("two adjacent",{{3,3},{4,3}});
    run("L shape",{{3,3},{4,3},{3,4}});
    run("diagonal",{{3,3},{4,4}});
    run("opposite diagonal",{{4,3},{3,4}});
    run("outer boundary",{{0,0}});

    LODMap checkerboard{};
    for(unsigned x=0;x<8;++x) for(unsigned z=0;z<8;++z) checkerboard[x*8+z]=(x+z)%2;
    runMap("checkerboard",checkerboard);
    run("isolated LOD 1",{{1,1},{1,5},{3,3},{5,1},{5,5}});
    LODMap rectangle{};
    for(unsigned x=2;x<=5;++x) for(unsigned z=2;z<=4;++z) rectangle[x*8+z]=1;
    runMap("rectangular cluster",rectangle);
    LODMap diagonal{};
    for(unsigned i=0;i<8;++i) diagonal[i*8+i]=1;
    runMap("diagonal chain",diagonal);
    LODMap boundary{};
    for(unsigned i=0;i<8;++i) boundary[i]=boundary[7*8+i]=boundary[i*8]=boundary[i*8+7]=1;
    runMap("all four boundaries",boundary);
    LODMap allCoarse{};allCoarse.fill(1);
    runMap("all LOD 1",allCoarse);
    for(std::uint32_t i=0;i<20;++i)
    {
        const std::uint32_t seed=0xC0FFEEu+i;
        std::mt19937 random(seed);
        LODMap lods{};
        // Direct engine output avoids implementation-dependent distribution mappings.
        for(auto& lod:lods) lod=random()&1u;
        runMap("random seed "+std::to_string(seed),lods);
    }
    std::printf("Configurations: %u tested, %u passed, %u failed\n",passed+failed,passed,failed);
    for(unsigned edges=0;edges<=4;++edges)
        std::printf("Fine patches with %u coarser neighbors tested: %u\n",edges,transitionCoverage[edges]);
    return failed ? 1 : 0;
}

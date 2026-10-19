# Engine
Engine is an in development game engine. The goal is to make an actually usable engine with as advanced features as is fun to experiment with and implement. 

## WYSIWYG
A fundamental principle for Engine is 'what you see is what you get' (WYSIWYG). Any view/performance you see should be equal universally within the editor and within any published product. If it runs bad in engine it'll run bad in game and vise-versa.

## Features

- Deferrer Rendering
- Bindless Texturing
- PBR
    - Standard Metallic/Roughness
    - Complex n/k IOR complient materials
        - Dielectrics
        - Conductors
- RayTracing
    - RT soft shadows
    - RT reflections
    - RTGI
    - RTAO
- GPU-Driven Adaptive Displacement w/ DXR Support (WIP)
- Box3D Physics Integration
- AngelScript Integration (WIP)

Future goals listed here:

- Mesh Shading System
- Multi-threaded rendering
- ReSTIR Path Tracing
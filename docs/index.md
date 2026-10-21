# Engine

**Engine** is a 3D game engine and editor built with C++23, Direct3D 12, and DirectX Raytracing (DXR).

The project goal is to develop a modern rendering pipeline alongside a pratical, integrated editor for creating, managing, and packaging games.

Engine is currently being actively developed. Features, workflows, and file formats may change as development continues.

## Overview

Engine combines real-time rendering, scene editing, physics, asset management, and packaged application support within a single development environment.

Its architecture spearates the editor from the packaged runtime, allowing authored content to be packaged for distribution.

## Features

### Rendering

- Direct3D 12 rendering architecture
- Direct X Raytracing integration
    - RTGI
    - RT Reflections
    - RT Shadows
    - RTAO
- Full PBR support
- Deferred lighting
- GPU resource and descriptor management

### Scene Editor

- Interactive 3D viewport
- Scene hierarchy with parent-child relations
- Asset browsing
- Realtime material editing

### Physics

- 3D rigid-body physics using [Box3D](https://github.com/erincatto/box3d) integration
- Static, dynamic, and kinematic bodies
- Box, sphere, capsule collision shapes
- Collision and Trigger events
- Character controller support

### Asset and Packaging

- Project-relative asset resolution
- Asset registry and import workflows
- Mesh and texture processing
- Standalone packaged runtime

## Editor and Runtime Architecture

Engine consists of two primary applications:

| Application | Purpose |
|---|---|
| `Engine.exe` | Editor for authoring scenes, configuring objects, and managing assets |
| `EngineRuntime.exe` | Standalone runtime for executing packaged content |

The editor works with source assets and scene documents.

The packaged runtime uses cooked archives rather than relying on loose editor source assets.

## Building

Engine targets Windows and uses a C++23 toolchain with Direct3D 12 support.

A standard Debug build can be made using CMake:

`cmake --build build --config Debug --target Engine EngineRuntime -- /m /v:minimal`

Build outputs:

`build/Debug/Engine.exe`
`build/Game/Debug/EngineRuntime.exe`

Additional setup and dependency requirements should be documented in the build guide.

## Documentation

Documentation is organized around the engine's major systems and will be continuously updated as development continues.

| Section | Description |
|---|---|
| Getting Started | Setting up, building, and launching Engine |
| Editor Guide | Viewport controls, scene editing, and Editor workflows |
| Rendering | Rendering architecture, lighting, ray tracing, and post-processing |
| Scene System | Scene objects, hierarchy, transforms, and serialization |
| [Physics](Physics/README.md) | Rigid bodies, colliders, triggers, and character controllers |
| Asset Pipeline | Importing, processing, and cooking assets |
| Packaging | Building and distributing standalone applications |

## Development Roadmap

The folowing areas are planned or currently under development:

- **AngelScript integration** - Gameplay scripting and engine API exposure
- **Editor Workflow Improvmeents** - Continued improvements to usability, configuration, and content creation
- **Rendering and Runtime Improvements** - Further optimization, testing, and expansion of rendering and runtime systems

This roadmap is subject to change, and regularly does change.
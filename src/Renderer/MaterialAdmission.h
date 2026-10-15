#pragma once
#include "assets/ProjectMaterialPreparationService.h"
#include "Renderer/StructuredBuffer.h"
#include "Renderer/UploadBuffer.h"

struct Material;

enum class MaterialAdmissionStatus { Pending, Ready, Retry, Stale, Unsupported, Failed };
struct MaterialAdmissionResult
{
    MaterialAdmissionStatus status = MaterialAdmissionStatus::Pending;
    Material* material = nullptr; // Only returned after successful publication.
    std::string message;
};

// An unsubmitted replacement, or the old completed slot after a swap.
struct MaterialTableReplacement
{
    StructuredBuffer buffer;
    UploadBuffer upload;
    ~MaterialTableReplacement() { buffer.Shutdown(); upload.Shutdown(); }
};
struct MaterialAdmissionState;

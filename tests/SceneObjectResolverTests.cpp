
#include "Scene/SceneObjectResolver.h"
#include "Scene/SceneAssetResolver.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            std::exit(EXIT_FAILURE);
        }
    }

    // These pointers are identity tokens only.
    // The resolver must never dereference them.
    class MockResolver final : public SceneAssetResolver
    {
    public:
        Mesh* ResolveMesh(std::string_view reference) override
        {
            return reference == "Meshes/Sphere.mesh"
                ? reinterpret_cast<Mesh*>(&m_meshToken)
                : nullptr;
        }

        Material* ResolveMaterial(std::string_view reference) override
        {
            return reference == "Materials/Default.material"
                ? reinterpret_cast<Material*>(&m_materialToken)
                : nullptr;
        }

    private:
        unsigned char m_meshToken = 0;
        unsigned char m_materialToken = 0;
    };

    SceneDocument MakeDocument()
    {
        SceneDocument document;
        document.id = "scene-001";

        SceneObjectDocument object;
        object.id = "object-001";
        object.name = "Sphere";
        object.meshReference = "Meshes/Sphere.mesh";
        object.materialReference = "Materials/Default.material";
        object.transform.position = {1.0f, 2.0f, 3.0f};

        document.objects.push_back(object);
        return document;
    }

    void TestSuccessfulResolution()
    {
        MockResolver resolver;
        SceneDocument document = MakeDocument();
        std::vector<ResolvedSceneObject> output;
        std::string error;

        Require(ResolveSceneObjects(document, resolver, output, &error),
                "Valid references should resolve.");

        Require(output.size() == 1, "Expected one resolved object.");
        Require(output[0].id == "object-001", "Object ID was lost.");
        Require(output[0].name == "Sphere", "Object name was lost.");
        Require(output[0].renderObject.mesh != nullptr, "Mesh was not resolved.");
        Require(output[0].renderObject.material != nullptr, "Material was not resolved.");

        Require(output[0].renderObject.transform.position.x == 1.0f,
                "Object transform was not preserved.");
        Require(output[0].renderObject.previousTransform.position.x == 1.0f,
                "Previous transform was not initialized.");
    }

    void TestFailedResolutionPreservesOutput()
    {
        MockResolver resolver;
        SceneDocument document = MakeDocument();
        std::vector<ResolvedSceneObject> output;
        std::string error;

        Require(ResolveSceneObjects(document, resolver, output, &error),
                "Initial resolution failed.");

        document.objects[0].meshReference = "Meshes/Missing.mesh";

        Require(!ResolveSceneObjects(document, resolver, output, &error),
                "Missing mesh should fail.");
        Require(output.size() == 1 && output[0].id == "object-001",
                "Missing mesh changed existing output.");

        document.objects[0].meshReference = "Meshes/Sphere.mesh";
        document.objects[0].materialReference = "Materials/Missing.material";

        Require(!ResolveSceneObjects(document, resolver, output, &error),
                "Missing material should fail.");
        Require(output.size() == 1 && output[0].id == "object-001",
                "Missing material changed existing output.");
    }

    void TestInvalidReferences()
    {
        MockResolver resolver;
        SceneDocument document = MakeDocument();
        std::vector<ResolvedSceneObject> output;
        std::string error;

        document.objects[0].meshReference.clear();

        Require(!ResolveSceneObjects(document, resolver, output, &error),
                "Empty mesh reference should fail.");

        document.objects[0].meshReference = "Meshes/Sphere.mesh";
        document.objects[0].materialReference.clear();

        Require(!ResolveSceneObjects(document, resolver, output, &error),
                "Empty material reference should fail.");

        document.objects[0].materialReference = "Materials/Default.material";
        document.objects[0].id.clear();

        Require(!ResolveSceneObjects(document, resolver, output, &error),
                "Invalid document should fail.");
    }

    void TestEmptyScene()
    {
        MockResolver resolver;
        SceneDocument document;
        document.id = "empty-scene";

        std::vector<ResolvedSceneObject> output;
        std::string error;

        Require(ResolveSceneObjects(document, resolver, output, &error),
                "Valid empty scene should resolve.");
        Require(output.empty(), "Empty scene produced render objects.");
    }
}

int main()
{
    TestSuccessfulResolution();
    TestFailedResolutionPreservesOutput();
    TestInvalidReferences();
    TestEmptyScene();

    std::cout << "Scene object resolver tests passed.\n";
    return EXIT_SUCCESS;
}

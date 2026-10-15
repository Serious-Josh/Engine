#pragma once
#include "Scene/SceneDocument.h"
#include "assets/ProjectAssetRegistry.h"
#include <filesystem>
#include <memory>
#include <thread>
namespace BuildGame
{
struct Settings { std::string title="MyGame"; std::filesystem::path output; bool advanced=false; };
struct Request { Settings settings; SceneDocument document; ProjectAssetRegistry registry; std::filesystem::path project, shaderRoot, runtimeDirectory; };
struct Status { bool busy=false, success=false; float progress=0; std::string message="Ready."; };
bool ValidateTitle(std::string_view title, std::string& error);
AssetID SceneIdentity(std::string_view persistentID);
class Service
{
public:
    Service(); ~Service();
    bool Start(Request request,std::string& error);
    void Cancel();
    Status Poll() const;
private:
    struct State;
    std::shared_ptr<State> m_state;
    std::jthread m_worker;
};
}

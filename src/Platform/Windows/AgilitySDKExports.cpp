// DirectX 12 Agility SDK runtime selection. D3D12 reads these executable exports to load
// D3D12Core.dll from D3D12/ beside the executable (cmake/AgilitySDK.cmake deploys it).
#include <Windows.h>

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = ENGINE_AGILITY_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

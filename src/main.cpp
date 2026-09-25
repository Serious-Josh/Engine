#include "Engine/Engine.h"

int main(){
    Engine engine;

    engine.Initialize();
    engine.Run();
    engine.Shutdown();
    
    return 0;
}
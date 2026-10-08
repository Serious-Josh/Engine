#include "Engine/Engine.h"
#include "Core/Log.h"

#include <cstdlib>

int main(){
    Engine engine;

    if(!engine.Initialize()){
        Log::Error("Engine initialization failed; aborting without automatic resource teardown because GPU completion may be unconfirmed.");
        std::abort();
    }
    
    engine.Run();
    engine.Shutdown();
    
    return 0;
}
// ScriptApiExport: writes Engine's AngelScript API as as.predefined for the AngelScript Language Server.
// Usage: ScriptApiExport <path/to/as.predefined>
// Initializes ScriptHost exactly as gameplay does (no renderer, audio or project is involved) and reflects it.

#include "Scripting/ScriptApiExport.h"
#include "Scripting/ScriptHost.h"

#include <iostream>

namespace
{
void PrintDiagnostics(const Scripting::Result& result)
{
    for(const auto& diagnostic : result.diagnostics)
    {
        std::cerr << diagnostic.section << ": " << diagnostic.text << '\n';
    }
}
}

int main(int argumentCount, char** arguments)
{
    if(argumentCount != 2)
    {
        std::cerr << "Usage: ScriptApiExport <path/to/as.predefined>\n";
        return 2;
    }

    Scripting::ScriptHost host;
    const Scripting::Result initialized = host.Initialize();

    if(initialized.status != Scripting::Status::Ready)
    {
        std::cerr << "ScriptHost initialization failed.\n";
        PrintDiagnostics(initialized);
        return 1;
    }

    std::string declarations;
    const Scripting::Result described = host.DescribeApi(declarations);

    if(described.status != Scripting::Status::Finished)
    {
        std::cerr << "Cannot describe the script API.\n";
        PrintDiagnostics(described);
        return 1;
    }

    std::string error;
    const auto written = Scripting::WriteDeclarationFile(arguments[1], declarations, error);

    switch(written)
    {
    case Scripting::DeclarationFileStatus::Written:
        std::cout << "Wrote " << arguments[1] << '\n';
        return 0;
    case Scripting::DeclarationFileStatus::Unchanged:
        std::cout << "Up to date: " << arguments[1] << '\n';
        return 0;
    case Scripting::DeclarationFileStatus::Refused:
    case Scripting::DeclarationFileStatus::Failed:
        std::cerr << error << '\n';
        return 1;
    }

    return 1;
}

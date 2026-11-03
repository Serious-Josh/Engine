#pragma once

#include <string>

class asIScriptEngine;
class asITypeInfo;

namespace Scripting
{
// Writes the script-visible interface registered on `engine` as AngelScript declarations in the form the AngelScript
// Language Server reads from as.predefined. The text is derived only from the engine's reflection data, so it always
// matches what gameplay scripts compile against. `scriptBase` is the compiled EAScript class (a script class, which
// engine reflection does not list); its public members are emitted as an abstract class. Output order is stable:
// namespaces, then kind, then name; members keep their registration order.
std::string WriteScriptApiDeclarations(const asIScriptEngine& engine, const asITypeInfo* scriptBase);
}

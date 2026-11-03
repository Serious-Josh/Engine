#include "Scripting/ScriptApiDeclarations.h"

#include <angelscript.h>

#include <algorithm>
#include <map>
#include <string_view>
#include <vector>

namespace Scripting
{
namespace
{
// Emission order inside one namespace block. Types come before the functions and properties that use them, so a
// reader scanning the file meets each type before its first use.
enum class EntryKind
{
    Enum,
    Typedef,
    Funcdef,
    Class,
    Function,
    Property
};

struct Entry
{
    EntryKind kind;
    std::string name;
    std::string text;
};

// AngelScript returns declarations in a buffer reused by the next call; copy before asking again.
std::string Copy(const char* text)
{
    return text ? std::string(text) : std::string();
}

std::string Indented(std::string_view text, std::string_view indent)
{
    std::string indented;
    std::size_t lineStart = 0;

    while(lineStart < text.size())
    {
        const std::size_t lineEnd = text.find('\n', lineStart);
        const std::size_t length = lineEnd == std::string_view::npos ? text.size() - lineStart : lineEnd - lineStart;
        // Blank separator lines stay empty rather than carrying trailing indentation.
        if(length > 0)
        {
            indented.append(indent).append(text.substr(lineStart, length));
        }

        indented.push_back('\n');

        if(lineEnd == std::string_view::npos)
        {
            break;
        }

        lineStart = lineEnd + 1;
    }

    return indented;
}

std::string EnumText(const asITypeInfo& type)
{
    std::string text = "enum " + Copy(type.GetName()) + "\n{\n";

    for(asUINT index = 0; index < type.GetEnumValueCount(); ++index)
    {
        int value = 0;
        const std::string name = Copy(type.GetEnumValueByIndex(index, &value));
        text += "    " + name + " = " + std::to_string(value);
        text += index + 1 < type.GetEnumValueCount() ? ",\n" : "\n";
    }

    return text + "}\n";
}

std::string ClassHeader(const asITypeInfo& type, bool isAbstract)
{
    std::string header = isAbstract ? "abstract class " : "class ";
    header += Copy(type.GetName());

    if(type.GetSubTypeCount() > 0)
    {
        header += "<";

        for(asUINT index = 0; index < type.GetSubTypeCount(); ++index)
        {
            header += index > 0 ? ", " : "";
            header += Copy(type.GetSubType(index)->GetName());
        }

        header += ">";
    }

    return header + "\n{\n";
}

// Constructors (value types) and factories (reference types) are both written as constructors, which is how scripts
// spell either. Destructors, list constructors and garbage-collector behaviours are not callable by name.
void AppendConstructors(const asITypeInfo& type, std::string& text)
{
    for(asUINT index = 0; index < type.GetBehaviourCount(); ++index)
    {
        asEBehaviours behaviour = asBEHAVE_CONSTRUCT;
        const asIScriptFunction* function = type.GetBehaviourByIndex(index, &behaviour);

        if(function && behaviour == asBEHAVE_CONSTRUCT)
        {
            text += "    " + Copy(function->GetDeclaration(false, false, true)) + ";\n";
        }
    }

    for(asUINT index = 0; index < type.GetFactoryCount(); ++index)
    {
        const asIScriptFunction* factory = type.GetFactoryByIndex(index);

        if(factory)
        {
            // A factory is declared as `Type@ Type(...)`; scripts call it exactly like a constructor.
            std::string declaration = Copy(factory->GetDeclaration(false, false, true));
            const std::size_t nameStart = declaration.find(Copy(type.GetName()) + "(");

            if(nameStart != std::string::npos)
            {
                declaration.erase(0, nameStart);
            }

            text += "    " + declaration + ";\n";
        }
    }
}

// Registered types expose every member. For the EAScript script class only what a derived class may use is written:
// private and protected members are implementation, and compiler-generated methods (opAssign) have no source section.
std::string ClassText(const asITypeInfo& type, bool isScriptBase)
{
    std::string text = ClassHeader(type, isScriptBase);

    if(!isScriptBase)
    {
        AppendConstructors(type, text);
    }

    for(asUINT index = 0; index < type.GetPropertyCount(); ++index)
    {
        bool isPrivate = false;
        bool isProtected = false;
        type.GetProperty(index, nullptr, nullptr, &isPrivate, &isProtected);

        if(!isPrivate && !isProtected)
        {
            text += "    " + Copy(type.GetPropertyDeclaration(index, true)) + ";\n";
        }
    }

    for(asUINT index = 0; index < type.GetMethodCount(); ++index)
    {
        // The real implementations, not script-class virtual stubs (which have no declaring section).
        const asIScriptFunction* method = type.GetMethodByIndex(index, false);

        if(!method || method->IsPrivate() || method->IsProtected())
        {
            continue;
        }

        const char* section = nullptr;

        if(isScriptBase && (method->GetDeclaredAt(&section, nullptr, nullptr) < 0 || !section))
        {
            continue;
        }

        text += "    " + Copy(method->GetDeclaration(false, false, true)) + ";\n";
    }

    for(asUINT index = 0; index < type.GetChildFuncdefCount(); ++index)
    {
        const asIScriptFunction* signature = type.GetChildFuncdef(index)->GetFuncdefSignature();
        text += "    funcdef " + Copy(signature->GetDeclaration(false, false, true)) + ";\n";
    }

    return text + "}\n";
}

using EntriesByNamespace = std::map<std::string, std::vector<Entry>>;

void Add(EntriesByNamespace& entries, const char* nameSpace, EntryKind kind, std::string name, std::string text)
{
    entries[Copy(nameSpace)].push_back({kind, std::move(name), std::move(text)});
}

void CollectTypes(const asIScriptEngine& engine, EntriesByNamespace& entries)
{
    for(asUINT index = 0; index < engine.GetEnumCount(); ++index)
    {
        const asITypeInfo* type = engine.GetEnumByIndex(index);
        Add(entries, type->GetNamespace(), EntryKind::Enum, Copy(type->GetName()), EnumText(*type));
    }

    for(asUINT index = 0; index < engine.GetTypedefCount(); ++index)
    {
        const asITypeInfo* type = engine.GetTypedefByIndex(index);
        const std::string aliased = Copy(engine.GetTypeDeclaration(type->GetTypedefTypeId(), true));
        Add(entries, type->GetNamespace(), EntryKind::Typedef, Copy(type->GetName()),
            "typedef " + aliased + " " + Copy(type->GetName()) + ";\n");
    }

    for(asUINT index = 0; index < engine.GetFuncdefCount(); ++index)
    {
        const asITypeInfo* type = engine.GetFuncdefByIndex(index);

        // Funcdefs owned by a class are written inside that class.
        if(type->GetParentType())
        {
            continue;
        }

        const std::string signature = Copy(type->GetFuncdefSignature()->GetDeclaration(false, false, true));
        Add(entries, type->GetNamespace(), EntryKind::Funcdef, Copy(type->GetName()), "funcdef " + signature + ";\n");
    }

    for(asUINT index = 0; index < engine.GetObjectTypeCount(); ++index)
    {
        const asITypeInfo* type = engine.GetObjectTypeByIndex(index);
        Add(entries, type->GetNamespace(), EntryKind::Class, Copy(type->GetName()), ClassText(*type, false));
    }
}

void CollectGlobals(const asIScriptEngine& engine, EntriesByNamespace& entries)
{
    for(asUINT index = 0; index < engine.GetGlobalFunctionCount(); ++index)
    {
        const asIScriptFunction* function = engine.GetGlobalFunctionByIndex(index);
        Add(entries, function->GetNamespace(), EntryKind::Function, Copy(function->GetName()),
            Copy(function->GetDeclaration(false, false, true)) + ";\n");
    }

    for(asUINT index = 0; index < engine.GetGlobalPropertyCount(); ++index)
    {
        const char* name = nullptr;
        const char* nameSpace = nullptr;
        int typeId = 0;
        bool isConst = false;
        engine.GetGlobalPropertyByIndex(index, &name, &nameSpace, &typeId, &isConst);

        const std::string type = Copy(engine.GetTypeDeclaration(typeId, true));
        const std::string declaration = (isConst ? "const " : "") + type + " " + Copy(name) + ";\n";
        Add(entries, nameSpace, EntryKind::Property, Copy(name), declaration);
    }
}

std::string NamespaceText(const std::string& nameSpace, std::vector<Entry>& entries)
{
    // Stable sort keeps overloads in registration order, so the output changes only when the registrations do.
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right)
    {
        if(left.kind != right.kind)
        {
            return left.kind < right.kind;
        }

        return left.name < right.name;
    });

    std::string body;
    EntryKind previousKind = entries.front().kind;

    for(const Entry& entry : entries)
    {
        // Multi-line blocks (enums, classes) and each change of kind are separated by a blank line.
        const bool isBlock = entry.text.find('\n') + 1 < entry.text.size();

        if(!body.empty() && (isBlock || entry.kind != previousKind))
        {
            body += "\n";
        }

        body += entry.text;
        previousKind = entry.kind;
    }

    if(nameSpace.empty())
    {
        return body;
    }

    // Nested namespaces ("A::B") are written as nested blocks.
    std::string opening;
    std::string closing;
    std::string indent;
    std::size_t partStart = 0;

    while(partStart <= nameSpace.size())
    {
        const std::size_t partEnd = std::min(nameSpace.find("::", partStart), nameSpace.size());
        opening += indent + "namespace " + nameSpace.substr(partStart, partEnd - partStart) + "\n" + indent + "{\n";
        closing = indent + "}\n" + closing;
        indent += "    ";
        partStart = partEnd + 2;
    }

    return opening + Indented(body, indent) + closing;
}
}

std::string WriteScriptApiDeclarations(const asIScriptEngine& engine, const asITypeInfo* scriptBase)
{
    EntriesByNamespace entries;
    CollectTypes(engine, entries);
    CollectGlobals(engine, entries);

    if(scriptBase)
    {
        Add(entries, scriptBase->GetNamespace(), EntryKind::Class, Copy(scriptBase->GetName()),
            ClassText(*scriptBase, true));
    }

    std::string text =
        "// as.predefined: Engine's AngelScript API for the AngelScript Language Server.\n"
        "// Generated by ScriptApiExport from the bindings ScriptHost registers. Do not edit; regenerate instead.\n"
        "// AngelScript " ANGELSCRIPT_VERSION_STRING "\n";

    // std::map orders namespaces by name, so the global namespace ("") comes first.
    for(auto& [nameSpace, namespaceEntries] : entries)
    {
        text += "\n" + NamespaceText(nameSpace, namespaceEntries);
    }

    return text;
}
}

#include "Scripting/ScriptHost.h"
#include "Core/Log.h"
#include "Scripting/ScriptApiDeclarations.h"

#include <angelscript.h>
#include <scriptstdstring.h>
#include <scriptbuilder.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>
#include <map>

namespace Scripting
{
namespace
{
constexpr std::size_t TextLimit = 1024;
constexpr std::size_t DiagnosticLimit = 32;
constexpr std::size_t SourceLimit = 1024 * 1024;

// AS.5 engine section compiled into every behaviour module. Native classes cannot be script base
// classes, so EAScript is a script class: an abstract base holding the checked owner handle the
// host binds after construction, plus empty lifecycle callbacks a derived class may override.
constexpr const char* EAScriptSection = "Engine/EAScript.as";
constexpr const char* EAScriptSource =
    "abstract class EAScript\n"
    "{\n"
    "    private SceneObject eaOwner_;\n"
    "    SceneObject GetOwner() const { return eaOwner_; }\n"
    "    void OnStart() {}\n"
    "    void OnStop() {}\n"
    "    void OnUpdate(float deltaTime) {}\n"
    "}\n";
constexpr const char* OwnerProperty = "eaOwner_";

// Each successful initialization is a new world; handles from any other world never resolve.
std::atomic<std::uint64_t> g_worldCounter{0};

// Script-visible owner handle: plain data copied by value. The default handle is invalid.
struct ObjectHandle
{
    std::uint64_t world = 0;
    std::uint32_t slot = 0;
    std::uint32_t reserved = 0;
};

struct ScriptVector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ScriptVector2
{
    float x = 0.0f;
    float y = 0.0f;
};

const float MathPi = 3.14159265358979323846f;

std::string Bounded(std::string_view text)
{
    std::string result(text.substr(0, TextLimit));
    std::replace(result.begin(), result.end(), '\0', '?');
    return result;
}

void AddDiagnostic(Result& result, std::string_view text, std::string_view section = {},
    int line = 0, int column = 0, std::string_view severity = "Error")
{
    if(result.diagnostics.size() < DiagnosticLimit)
    {
        result.diagnostics.push_back({Bounded(section), line, column, Bounded(severity), Bounded(text)});
    }
}

// Attributes diagnostics to the asset and revision of the source closure that produced them.
void AttributeDiagnostics(Result& result, const SourceSnapshot& snapshot)
{
    for(auto& diagnostic : result.diagnostics)
    {
        diagnostic.assetReference = snapshot.rootAsset;
        diagnostic.sourceRevision = snapshot.revision;

        for(const auto& source : snapshot.sections)
        {
            if(source.name == diagnostic.section)
            {
                diagnostic.assetReference = source.assetReference;
                break;
            }
        }
    }
}

std::string QualifiedName(const asITypeInfo& type)
{
    const std::string space = type.GetNamespace() ? type.GetNamespace() : "";
    return space.empty() ? type.GetName() : space + "::" + type.GetName();
}

asIScriptFunction* FindDefaultFactory(const asITypeInfo& type)
{
    for(asUINT index = 0; index < type.GetFactoryCount(); ++index)
    {
        auto* factory = type.GetFactoryByIndex(index);

        if(factory && factory->GetParamCount() == 0)
        {
            return factory;
        }
    }

    return nullptr;
}

// Instantiable EAScript classes of a module: concrete, derived and default-constructible.
std::vector<std::string> FindBehaviourClasses(asIScriptModule& module)
{
    std::vector<std::string> classes;
    const auto* base = module.GetTypeInfoByName("EAScript");

    if(!base)
    {
        return classes;
    }

    for(asUINT index = 0; index < module.GetObjectTypeCount(); ++index)
    {
        const auto* type = module.GetObjectTypeByIndex(index);

        if(type && type != base && type->DerivesFrom(base) && !(type->GetFlags() & asOBJ_ABSTRACT) &&
            FindDefaultFactory(*type))
        {
            classes.push_back(QualifiedName(*type));
        }
    }

    std::sort(classes.begin(), classes.end());
    return classes;
}

struct EngineDeleter
{
    void operator()(asIScriptEngine* engine) const noexcept
    {
        engine->ShutDownAndRelease();
    }
};

struct ModuleDeleter
{
    void operator()(asIScriptModule* module) const noexcept
    {
        module->Discard();
    }
};

struct ContextDeleter
{
    void operator()(asIScriptContext* context) const noexcept
    {
        // Abort suspended calls before unpreparing; a context never survives one call.
        context->Abort();
        context->Unprepare();
        context->Release();
    }
};

struct ObjectDeleter
{
    void operator()(asIScriptObject* object) const noexcept
    {
        object->Release();
    }
};

struct Budget
{
    std::size_t remaining;
    std::chrono::steady_clock::time_point deadline;
    bool exceeded = false;
};

struct SourceCollection
{
    const SourceReader& reader;
    SourceLimits limits;
    Result& result;
    SourceSnapshot snapshot;
    std::map<std::string, SourceSection> sources;
    std::vector<std::string> active;
    std::size_t total = 0;

    int Load(const std::string& name, CScriptBuilder& builder)
    {
        CanonicalScriptSection(name);

        if(std::find(active.begin(), active.end(), name) != active.end())
        {
            throw std::runtime_error("Script include cycle: " + name);
        }

        for(const auto& [existing, source] : sources)
        {
            if(existing != name && ScriptNamesEqual(existing, name))
            {
                throw std::runtime_error("Ambiguous script section alias: " + name);
            }
        }

        if(sources.contains(name))
        {
            return 0;
        }

        if(active.size() >= limits.depth || sources.size() >= limits.sections)
        {
            throw std::runtime_error("Script include depth or section limit exceeded.");
        }

        SourceSection source = reader(name);

        if(source.name != name || source.assetReference.empty() || source.assetReference.size() > 256 ||
            source.bytes.size() > limits.sectionBytes ||
            source.bytes.size() > limits.totalBytes - total || source.bytes.find('\0') != std::string::npos)
        {
            throw std::runtime_error("Invalid script source identity, bytes or closure size.");
        }

        for(const auto& [existingName, existing] : sources)
        {
            if(existing.assetReference == source.assetReference)
            {
                throw std::runtime_error("Multiple script section names share one asset identity.");
            }
        }

        if(name == snapshot.rootSection)
        {
            snapshot.rootAsset = source.assetReference;
        }

        total += source.bytes.size();
        auto [entry, inserted] = sources.emplace(name, std::move(source));
        active.push_back(name);
        const int added = builder.AddSectionFromMemory(name.c_str(), entry->second.bytes.data(),
            static_cast<unsigned int>(entry->second.bytes.size()));
        active.pop_back();

        if(added < 0)
        {
            throw std::runtime_error("Script source preprocessing failed: " + name);
        }

        return added;
    }

    static int Include(const char* include, const char* from, CScriptBuilder* builder, void* parameter) noexcept
    {
        auto& collection = *static_cast<SourceCollection*>(parameter);

        try
        {
            return collection.Load(ResolveScriptInclude(from, include), *builder);
        }
        catch(const std::exception& error)
        {
            try
            {
                AddDiagnostic(collection.result, error.what(), from);
            }
            catch(...)
            {
            }

            return -1;
        }
        catch(...)
        {
            return -1;
        }
    }
};

void CheckBudget(asIScriptContext* context, Budget* budget) noexcept
{
    if(budget->remaining == 0 || std::chrono::steady_clock::now() >= budget->deadline)
    {
        budget->exceeded = true;
        context->Abort();
        return;
    }

    --budget->remaining;
}

bool ValidLimits(const ExecutionLimits& limits)
{
    return limits.statements != 0 && limits.timeout.count() > 0 && limits.timeout <= std::chrono::seconds(10);
}

void ConstructVector3(asIScriptGeneric* generic)
{
    *static_cast<ScriptVector3*>(generic->GetObject()) = {};
}

void ConstructVector3Components(asIScriptGeneric* generic)
{
    *static_cast<ScriptVector3*>(generic->GetObject()) =
        {generic->GetArgFloat(0), generic->GetArgFloat(1), generic->GetArgFloat(2)};
}

void ConstructObjectHandle(asIScriptGeneric* generic)
{
    *static_cast<ObjectHandle*>(generic->GetObject()) = {};
}

void RaiseScriptException(const char* text)
{
    if(auto* context = asGetActiveContext())
    {
        context->SetException(text);
    }
}

// AS.6A Vector3 arithmetic. Generic functions must keep the exact type void(asIScriptGeneric*)
// (no noexcept) so asFUNCTION selects the generic convention.
const ScriptVector3& ThisVector(asIScriptGeneric* generic)
{
    return *static_cast<const ScriptVector3*>(generic->GetObject());
}

const ScriptVector3& ArgumentVector(asIScriptGeneric* generic)
{
    return *static_cast<const ScriptVector3*>(generic->GetArgObject(0));
}

void ReturnVector(asIScriptGeneric* generic, ScriptVector3 value)
{
    generic->SetReturnObject(&value);
}

void VectorAdd(asIScriptGeneric* generic)
{
    const auto& left = ThisVector(generic);
    const auto& right = ArgumentVector(generic);
    ReturnVector(generic, {left.x + right.x, left.y + right.y, left.z + right.z});
}

void VectorSubtract(asIScriptGeneric* generic)
{
    const auto& left = ThisVector(generic);
    const auto& right = ArgumentVector(generic);
    ReturnVector(generic, {left.x - right.x, left.y - right.y, left.z - right.z});
}

void VectorScale(asIScriptGeneric* generic)
{
    const auto& value = ThisVector(generic);
    const float scale = generic->GetArgFloat(0);
    ReturnVector(generic, {value.x * scale, value.y * scale, value.z * scale});
}

void VectorDivide(asIScriptGeneric* generic)
{
    const auto& value = ThisVector(generic);
    const float divisor = generic->GetArgFloat(0);

    if(divisor == 0.0f)
    {
        RaiseScriptException("Vector3 division by zero.");
        return;
    }

    ReturnVector(generic, {value.x / divisor, value.y / divisor, value.z / divisor});
}

void VectorNegate(asIScriptGeneric* generic)
{
    const auto& value = ThisVector(generic);
    ReturnVector(generic, {-value.x, -value.y, -value.z});
}

void VectorAddAssign(asIScriptGeneric* generic)
{
    auto& value = *static_cast<ScriptVector3*>(generic->GetObject());
    const auto& other = ArgumentVector(generic);
    value = {value.x + other.x, value.y + other.y, value.z + other.z};
    generic->SetReturnAddress(&value);
}

void VectorSubtractAssign(asIScriptGeneric* generic)
{
    auto& value = *static_cast<ScriptVector3*>(generic->GetObject());
    const auto& other = ArgumentVector(generic);
    value = {value.x - other.x, value.y - other.y, value.z - other.z};
    generic->SetReturnAddress(&value);
}

void VectorScaleAssign(asIScriptGeneric* generic)
{
    auto& value = *static_cast<ScriptVector3*>(generic->GetObject());
    const float scale = generic->GetArgFloat(0);
    value = {value.x * scale, value.y * scale, value.z * scale};
    generic->SetReturnAddress(&value);
}

float VectorLengthOf(const ScriptVector3& value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

void VectorLength(asIScriptGeneric* generic)
{
    generic->SetReturnFloat(VectorLengthOf(ThisVector(generic)));
}

// AS.6B Vector2 (mouse deltas) and Math, kept to what mouse look needs.
void ConstructVector2(asIScriptGeneric* generic)
{
    *static_cast<ScriptVector2*>(generic->GetObject()) = {};
}

void ConstructVector2Components(asIScriptGeneric* generic)
{
    *static_cast<ScriptVector2*>(generic->GetObject()) = {generic->GetArgFloat(0), generic->GetArgFloat(1)};
}

const ScriptVector2& ThisVector2(asIScriptGeneric* generic)
{
    return *static_cast<const ScriptVector2*>(generic->GetObject());
}

const ScriptVector2& ArgumentVector2(asIScriptGeneric* generic)
{
    return *static_cast<const ScriptVector2*>(generic->GetArgObject(0));
}

void ReturnVector2(asIScriptGeneric* generic, ScriptVector2 value)
{
    generic->SetReturnObject(&value);
}

void Vector2Add(asIScriptGeneric* generic)
{
    ReturnVector2(generic, {ThisVector2(generic).x + ArgumentVector2(generic).x,
        ThisVector2(generic).y + ArgumentVector2(generic).y});
}

void Vector2Subtract(asIScriptGeneric* generic)
{
    ReturnVector2(generic, {ThisVector2(generic).x - ArgumentVector2(generic).x,
        ThisVector2(generic).y - ArgumentVector2(generic).y});
}

void Vector2Scale(asIScriptGeneric* generic)
{
    const float scale = generic->GetArgFloat(0);
    ReturnVector2(generic, {ThisVector2(generic).x * scale, ThisVector2(generic).y * scale});
}

void Vector2Length(asIScriptGeneric* generic)
{
    const auto& value = ThisVector2(generic);
    generic->SetReturnFloat(std::sqrt(value.x * value.x + value.y * value.y));
}

void MathSin(asIScriptGeneric* generic)
{
    generic->SetReturnFloat(std::sin(generic->GetArgFloat(0)));
}

void MathCos(asIScriptGeneric* generic)
{
    generic->SetReturnFloat(std::cos(generic->GetArgFloat(0)));
}

void MathClamp(asIScriptGeneric* generic)
{
    const float value = generic->GetArgFloat(0);
    const float minimum = generic->GetArgFloat(1);
    const float maximum = generic->GetArgFloat(2);

    if(minimum > maximum)
    {
        RaiseScriptException("Math::Clamp requires minimum <= maximum.");
        return;
    }

    generic->SetReturnFloat(value < minimum ? minimum : value > maximum ? maximum : value);
}

void MathToRadians(asIScriptGeneric* generic)
{
    generic->SetReturnFloat(generic->GetArgFloat(0) * (MathPi / 180.0f));
}

// The zero vector normalizes to zero, so "no input" stays "no movement" without a script branch.
void VectorNormalized(asIScriptGeneric* generic)
{
    const auto& value = ThisVector(generic);
    const float length = VectorLengthOf(value);
    ReturnVector(generic, length > 0.0f ? ScriptVector3{value.x / length, value.y / length, value.z / length} :
        ScriptVector3{});
}
}

struct ScriptHost::Impl
{
    struct Behaviour
    {
        std::unique_ptr<asIScriptModule, ModuleDeleter> module;
        std::shared_ptr<const SourceSnapshot> snapshot;
    };

    struct Instance
    {
        std::unique_ptr<asIScriptObject, ObjectDeleter> object;
        ModuleID module = 0;
    };

    const std::thread::id owner = std::this_thread::get_id();
    Result result;
    bool executing = false;
    std::string section;
    std::uint64_t world = 0;
    ObjectResolver* resolver = nullptr;
    InputSource* input = nullptr;
    AudioService* audio = nullptr;
    std::size_t logCount = 0;
    // Reverse member destruction releases instances, then modules, then the engine.
    std::unique_ptr<asIScriptEngine, EngineDeleter> engine;
    std::unique_ptr<asIScriptModule, ModuleDeleter> module;
    std::vector<Behaviour> behaviours;
    std::vector<Instance> instances;

    bool IsAvailable() const noexcept
    {
        return owner == std::this_thread::get_id() && !executing;
    }

    // Drops every script object before the modules defining their types, then the engine.
    void Release() noexcept
    {
        instances.clear();

        if(engine)
        {
            engine->GarbageCollect(asGC_FULL_CYCLE);
        }

        behaviours.clear();
        module.reset();
        engine.reset();
        world = 0;
    }

    bool Resolves(const ObjectHandle& handle) const noexcept
    {
        try
        {
            return resolver && world != 0 && handle.world == world && executing &&
                owner == std::this_thread::get_id() && resolver->IsValid(handle.slot);
        }
        catch(...)
        {
            return false;
        }
    }

    // Runs one prepared call under the budget; the context never survives it. Fills
    // result.status and diagnostics. created receives a factory's returned object.
    void Run(asIScriptFunction& function, asIScriptObject* object, const ExecutionLimits& limits,
        std::string_view diagnosticSection, asIScriptObject** created, const float* argument = nullptr)
    {
        Budget budget{limits.statements, std::chrono::steady_clock::now() + limits.timeout};
        std::unique_ptr<asIScriptContext, ContextDeleter> context(engine->CreateContext());

        if(!context || context->Prepare(&function) < 0 || (object && context->SetObject(object) < 0) ||
            (argument && context->SetArgFloat(0, *argument) < 0) ||
            context->SetLineCallback(asFUNCTION(CheckBudget), &budget, asCALL_CDECL) < 0)
        {
            AddDiagnostic(result, "Cannot prepare bounded execution context.");
            result.status = Status::Failed;
            return;
        }

        struct ExecutionScope
        {
            bool& active;
            ~ExecutionScope()
            {
                active = false;
            }
        } executionScope{executing};

        executing = true;
        logCount = 0;
        const int execution = context->Execute();

        if(execution == asEXECUTION_FINISHED)
        {
            result.status = Status::Finished;

            if(created)
            {
                auto* returned = static_cast<asIScriptObject*>(context->GetReturnObject());

                if(returned)
                {
                    returned->AddRef();
                }

                *created = returned;
            }
        }
        else if(budget.exceeded)
        {
            result.status = Status::BudgetExceeded;
            AddDiagnostic(result, "Script execution budget exceeded; context aborted.", diagnosticSection);
        }
        else if(execution == asEXECUTION_EXCEPTION)
        {
            result.status = Status::Exception;
            const char* exceptionSection = nullptr;
            int column = 0;
            const int line = context->GetExceptionLineNumber(&column, &exceptionSection);
            AddDiagnostic(result, context->GetExceptionString(), exceptionSection ? exceptionSection : "", line, column);

            for(asUINT index = 0; index < (std::min)(context->GetCallstackSize(), asUINT(8)); ++index)
            {
                auto* frame = context->GetFunction(index);
                const char* frameSection = nullptr;
                const int frameLine = context->GetLineNumber(index, nullptr, &frameSection);

                if(frame)
                {
                    AddDiagnostic(result, frame->GetDeclaration(), frameSection ? frameSection : diagnosticSection,
                        frameLine, 0, "Stack");
                }
            }
        }
        else
        {
            result.status = execution == asEXECUTION_SUSPENDED ? Status::Suspended : Status::Failed;
            AddDiagnostic(result, "Unexpected execution status " + std::to_string(execution) +
                "; context discarded without resumption.", diagnosticSection);
        }
    }

    static void Message(const asSMessageInfo* message, Impl* state) noexcept
    {
        try
        {
            const char* severity = message->type == asMSGTYPE_ERROR ? "Error" :
                message->type == asMSGTYPE_WARNING ? "Warning" : "Info";
            AddDiagnostic(state->result, message->message, message->section,
                message->row, message->col, severity);
        }
        catch(...)
        {
            // Allocation failures must not unwind through the compiler's C callback.
        }
    }

    static void Info(const std::string& message, Impl* state) noexcept
    {
        auto* context = asGetActiveContext();

        try
        {
            if(!context || !state->executing || state->owner != std::this_thread::get_id())
            {
                throw std::runtime_error("Logging requires the owning execution context.");
            }

            // Per-execution log cap prevents a bounded loop from flooding the host sink.
            if(state->logCount++ >= 64)
            {
                context->SetException("Script log message limit exceeded.");
                return;
            }

            const char* activeSection = nullptr;
            const int line = context->GetLineNumber(0, nullptr, &activeSection);
            Log::Info("[Script " + Bounded(activeSection ? activeSection : state->section) + ":" +
                std::to_string(line) + "] " + Bounded(message));
        }
        catch(const std::exception&)
        {
            if(context)
            {
                context->SetException("Recoverable native logging exception.");
            }
        }
        catch(...)
        {
            if(context)
            {
                context->SetException("Unknown native logging exception.");
            }
        }
    }

    // SceneObject methods; not declared noexcept, because asFUNCTION recognises the generic
    // convention by the exact type void(asIScriptGeneric*). Each catches everything itself.
    // Expected misuse (stale or default handles) answers false or an empty
    // name; reading the position of an invalid object raises a script exception instead.
    static void ObjectIsValid(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        generic->SetReturnByte(state.Resolves(*static_cast<const ObjectHandle*>(generic->GetObject())) ? 1 : 0);
    }

    static void ObjectGetName(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());

        try
        {
            std::string name = state.Resolves(handle) ? Bounded(state.resolver->GetName(handle.slot)) : std::string();
            generic->SetReturnObject(&name);
        }
        catch(...)
        {
            if(auto* context = asGetActiveContext())
            {
                context->SetException("SceneObject name is unavailable.");
            }
        }
    }

    bool ControllerUsable(const ObjectHandle& handle) const noexcept
    {
        try
        {
            return Resolves(handle) && resolver->HasCharacterController(handle.slot);
        }
        catch(...)
        {
            return false;
        }
    }

    // AS.6A Character Controller handle: same world and slot as its SceneObject, checked against
    // the current Play session on every use. Requests are recorded by the runtime, never applied here.
    static void ObjectGetCharacterController(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        ObjectHandle controller{};

        if(state.ControllerUsable(handle))
        {
            controller = handle;
        }

        generic->SetReturnObject(&controller);
    }

    static void ControllerIsValid(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        generic->SetReturnByte(state.ControllerUsable(*static_cast<const ObjectHandle*>(generic->GetObject())) ? 1 : 0);
    }

    static void ControllerIsGrounded(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        bool grounded = false;

        try
        {
            grounded = state.ControllerUsable(handle) && state.resolver->IsCharacterGrounded(handle.slot);
        }
        catch(...)
        {
            grounded = false;
        }

        generic->SetReturnByte(grounded ? 1 : 0);
    }

    static void ControllerMove(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        const auto& velocity = ArgumentVector(generic);

        try
        {
            if(!state.ControllerUsable(handle))
            {
                RaiseScriptException("CharacterController is not valid.");
                return;
            }

            const float values[3] = {velocity.x, velocity.y, velocity.z};
            std::string error;

            if(!state.resolver->MoveCharacter(handle.slot, values, error))
            {
                RaiseScriptException(Bounded(error).c_str());
            }
        }
        catch(...)
        {
            RaiseScriptException("CharacterController movement request failed.");
        }
    }

    static void ControllerJump(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());

        try
        {
            if(!state.ControllerUsable(handle))
            {
                RaiseScriptException("CharacterController is not valid.");
                return;
            }

            std::string error;

            if(!state.resolver->JumpCharacter(handle.slot, error))
            {
                RaiseScriptException(Bounded(error).c_str());
            }
        }
        catch(...)
        {
            RaiseScriptException("CharacterController jump request failed.");
        }
    }

    // AS.6A Input: false outside a gameplay dispatch, without focus, or for an unknown key.
    static void InputQuery(asIScriptGeneric* generic, bool pressed)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto value = static_cast<int>(generic->GetArgDWord(0));
        bool result = false;

        try
        {
            if(state.input && state.executing && state.owner == std::this_thread::get_id() &&
                value >= static_cast<int>(Key::W) && value <= static_cast<int>(Key::Space))
            {
                const auto key = static_cast<Key>(value);
                result = pressed ? state.input->WasKeyPressed(key) : state.input->IsKeyDown(key);
            }
        }
        catch(...)
        {
            result = false;
        }

        generic->SetReturnByte(result ? 1 : 0);
    }

    // Audio.1C gameplay audio. Like input, the service answers only during this host's own execution; without it
    // every call is a quiet no-op (an invalid handle, false). The service validates handles and values itself and
    // reports actionable problems; nothing here raises a script exception, so audio can never stop gameplay.
    bool AudioAvailable() const noexcept
    {
        return audio && executing && owner == std::this_thread::get_id();
    }

    static Impl& State(asIScriptGeneric* generic)
    {
        return *static_cast<Impl*>(generic->GetAuxiliary());
    }

    static const AudioEventHandle& ThisAudioEvent(asIScriptGeneric* generic)
    {
        return *static_cast<const AudioEventHandle*>(generic->GetObject());
    }

    static void ConstructAudioEvent(asIScriptGeneric* generic)
    {
        *static_cast<AudioEventHandle*>(generic->GetObject()) = {};
    }

    static void AudioPlay(asIScriptGeneric* generic)
    {
        Impl& state = State(generic);
        AudioEventHandle handle{};

        try
        {
            const auto& event = *static_cast<const std::string*>(generic->GetArgObject(0));

            if(state.AudioAvailable() && !state.audio->PlayEvent(event, handle))
            {
                handle = {};
            }
        }
        catch(...)
        {
            handle = {};
        }

        generic->SetReturnObject(&handle);
    }

    // Runs one service query or command; any failure (or an exception escaping a service) answers false.
    template<typename Call>
    static bool AudioCall(asIScriptGeneric* generic, Call call)
    {
        Impl& state = State(generic);

        try
        {
            return state.AudioAvailable() && call(*state.audio);
        }
        catch(...)
        {
            return false;
        }
    }

    static void AudioEventIsValid(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const bool valid = AudioCall(generic, [&](AudioService& audio) { return audio.IsEventValid(handle); });
        generic->SetReturnByte(valid ? 1 : 0);
    }

    static void AudioEventIsPlaying(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const bool playing = AudioCall(generic, [&](AudioService& audio) { return audio.IsEventPlaying(handle); });
        generic->SetReturnByte(playing ? 1 : 0);
    }

    static void AudioEventStop(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const bool allowFadeOut = generic->GetArgByte(0) != 0;
        AudioCall(generic, [&](AudioService& audio) { return audio.StopEvent(handle, allowFadeOut); });
    }

    static void AudioEventSetParameter(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const auto& name = *static_cast<const std::string*>(generic->GetArgObject(0));
        const float value = generic->GetArgFloat(1);
        const bool set = AudioCall(generic, [&](AudioService& audio)
        {
            return audio.SetEventParameter(handle, name, value);
        });
        generic->SetReturnByte(set ? 1 : 0);
    }

    static void AudioEventSetVolume(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const float volume = generic->GetArgFloat(0);
        const bool set = AudioCall(generic, [&](AudioService& audio) { return audio.SetEventVolume(handle, volume); });
        generic->SetReturnByte(set ? 1 : 0);
    }

    static void AudioEventRelease(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        AudioCall(generic, [&](AudioService& audio) { return audio.ReleaseEvent(handle); });
    }

    // Audio.2C positional events and attachment.
    static void AudioPlayAt(asIScriptGeneric* generic)
    {
        Impl& state = State(generic);
        AudioEventHandle handle{};

        try
        {
            const auto& event = *static_cast<const std::string*>(generic->GetArgObject(0));
            const auto& value = *static_cast<const ScriptVector3*>(generic->GetArgObject(1));
            const float position[3] = {value.x, value.y, value.z};

            if(state.AudioAvailable() && !state.audio->PlayEventAt(event, position, handle))
            {
                handle = {};
            }
        }
        catch(...)
        {
            handle = {};
        }

        generic->SetReturnObject(&handle);
    }

    static void AudioEventSetPosition(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const auto& value = ArgumentVector(generic);
        const float position[3] = {value.x, value.y, value.z};
        const bool set = AudioCall(generic, [&](AudioService& audio) { return audio.SetEventPosition(handle, position); });
        generic->SetReturnByte(set ? 1 : 0);
    }

    static void AudioEventAttachTo(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const auto& object = *static_cast<const ObjectHandle*>(generic->GetArgObject(0));
        const std::string key = State(generic).ObjectKey(object);
        const bool attached = AudioCall(generic, [&](AudioService& audio) { return audio.AttachEvent(handle, key); });
        generic->SetReturnByte(attached ? 1 : 0);
    }

    static void AudioEventDetach(asIScriptGeneric* generic)
    {
        const AudioEventHandle handle = ThisAudioEvent(generic);
        const bool detached = AudioCall(generic, [&](AudioService& audio) { return audio.DetachEvent(handle); });
        generic->SetReturnByte(detached ? 1 : 0);
    }

    // Audio.2C AudioSource: the same world and slot as its SceneObject, resolved to the object's persistent ID on
    // every call. The session owns the runtime state; a stale or default handle reaches it with an empty key, so
    // the session warns once and nothing changes.
    std::string ObjectKey(const ObjectHandle& handle) const
    {
        try
        {
            return Resolves(handle) ? resolver->GetObjectKey(handle.slot) : std::string();
        }
        catch(...)
        {
            return {};
        }
    }

    static std::string SourceKey(asIScriptGeneric* generic)
    {
        return State(generic).ObjectKey(*static_cast<const ObjectHandle*>(generic->GetObject()));
    }

    static void ObjectGetAudioSource(asIScriptGeneric* generic)
    {
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        const std::string key = State(generic).ObjectKey(handle);
        const bool present = !key.empty() &&
            AudioCall(generic, [&](AudioService& audio) { return audio.HasAudioSource(key); });
        ObjectHandle source = present ? handle : ObjectHandle{};
        generic->SetReturnObject(&source);
    }

    static void AudioSourceIsValid(asIScriptGeneric* generic)
    {
        const std::string key = SourceKey(generic);
        const bool valid = !key.empty() &&
            AudioCall(generic, [&](AudioService& audio) { return audio.HasAudioSource(key); });
        generic->SetReturnByte(valid ? 1 : 0);
    }

    static void AudioSourcePlay(asIScriptGeneric* generic)
    {
        const std::string key = SourceKey(generic);
        AudioCall(generic, [&](AudioService& audio) { return audio.PlayAudioSource(key); });
    }

    static void AudioSourceStop(asIScriptGeneric* generic)
    {
        const std::string key = SourceKey(generic);
        const bool allowFadeOut = generic->GetArgByte(0) != 0;
        AudioCall(generic, [&](AudioService& audio) { return audio.StopAudioSource(key, allowFadeOut); });
    }

    static void AudioSourceIsPlaying(asIScriptGeneric* generic)
    {
        const std::string key = SourceKey(generic);
        const bool playing = AudioCall(generic, [&](AudioService& audio) { return audio.IsAudioSourcePlaying(key); });
        generic->SetReturnByte(playing ? 1 : 0);
    }

    static void AudioSourceSetEnabled(asIScriptGeneric* generic)
    {
        const std::string key = SourceKey(generic);
        const bool enabled = generic->GetArgByte(0) != 0;
        AudioCall(generic, [&](AudioService& audio) { return audio.SetAudioSourceEnabled(key, enabled); });
    }

    static void AudioSourceIsEnabled(asIScriptGeneric* generic)
    {
        const std::string key = SourceKey(generic);
        const bool enabled = AudioCall(generic, [&](AudioService& audio) { return audio.IsAudioSourceEnabled(key); });
        generic->SetReturnByte(enabled ? 1 : 0);
    }

    static void AudioSetGlobalParameter(asIScriptGeneric* generic)
    {
        const auto& name = *static_cast<const std::string*>(generic->GetArgObject(0));
        const float value = generic->GetArgFloat(1);
        const bool set = AudioCall(generic, [&](AudioService& audio) { return audio.SetGlobalParameter(name, value); });
        generic->SetReturnByte(set ? 1 : 0);
    }

    static void AudioSetBusVolume(asIScriptGeneric* generic)
    {
        const auto& bus = *static_cast<const std::string*>(generic->GetArgObject(0));
        const float volume = generic->GetArgFloat(1);
        const bool set = AudioCall(generic, [&](AudioService& audio) { return audio.SetBusVolume(bus, volume); });
        generic->SetReturnByte(set ? 1 : 0);
    }

    static void AudioSetVCAVolume(asIScriptGeneric* generic)
    {
        const auto& vca = *static_cast<const std::string*>(generic->GetArgObject(0));
        const float volume = generic->GetArgFloat(1);
        const bool set = AudioCall(generic, [&](AudioService& audio) { return audio.SetVCAVolume(vca, volume); });
        generic->SetReturnByte(set ? 1 : 0);
    }

    static void InputIsKeyDown(asIScriptGeneric* generic)
    {
        InputQuery(generic, false);
    }

    bool InputAvailable() const noexcept
    {
        return input && executing && owner == std::this_thread::get_id();
    }

    static void InputGetMouseDelta(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        float delta[2] = {};

        try
        {
            if(state.InputAvailable())
            {
                state.input->GetMouseDelta(delta);
            }
        }
        catch(...)
        {
            delta[0] = 0.0f;
            delta[1] = 0.0f;
        }

        ScriptVector2 value{delta[0], delta[1]};
        generic->SetReturnObject(&value);
    }

    static void InputSetCursorLocked(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());

        try
        {
            if(state.InputAvailable())
            {
                state.input->SetCursorLocked(generic->GetArgByte(0) != 0);
            }
        }
        catch(...)
        {
            RaiseScriptException("Cursor lock request failed.");
        }
    }

    // AS.6B controller yaw: reading an invalid handle gives 0; changing it raises an exception.
    static void ControllerGetYaw(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        float yaw = 0.0f;

        try
        {
            if(!state.ControllerUsable(handle) || !state.resolver->GetCharacterYaw(handle.slot, yaw))
            {
                yaw = 0.0f;
            }
        }
        catch(...)
        {
            yaw = 0.0f;
        }

        generic->SetReturnFloat(yaw);
    }

    static void ControllerSetYaw(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        const float yaw = generic->GetArgFloat(0);

        try
        {
            if(!state.ControllerUsable(handle))
            {
                RaiseScriptException("CharacterController is not valid.");
                return;
            }

            std::string error;

            if(!state.resolver->SetCharacterYaw(handle.slot, yaw, error))
            {
                RaiseScriptException(Bounded(error).c_str());
            }
        }
        catch(...)
        {
            RaiseScriptException("CharacterController yaw request failed.");
        }
    }

    // AS.6B hierarchy and runtime transform methods on SceneObject.
    static void ObjectFindChild(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        const auto& name = *static_cast<const std::string*>(generic->GetArgObject(0));
        ObjectHandle child{};

        try
        {
            std::uint32_t slot = 0;

            if(state.Resolves(handle) && state.resolver->FindChild(handle.slot, name, slot))
            {
                child = {state.world, slot, 0};
            }
        }
        catch(...)
        {
            child = {};
        }

        generic->SetReturnObject(&child);
    }

    static void ReturnResolvedVector(asIScriptGeneric* generic, bool resolved, const float (&value)[3], const char* failure)
    {
        if(!resolved)
        {
            RaiseScriptException(failure);
            return;
        }

        ScriptVector3 result{value[0], value[1], value[2]};
        generic->SetReturnObject(&result);
    }

    static void ObjectGetLocalRotation(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        float rotation[3] = {};
        bool resolved = false;

        try
        {
            resolved = state.Resolves(handle) && state.resolver->GetLocalRotation(handle.slot, rotation);
        }
        catch(...)
        {
            resolved = false;
        }

        ReturnResolvedVector(generic, resolved, rotation, "SceneObject is not valid.");
    }

    static void ObjectGetForward(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        float forward[3] = {};
        bool resolved = false;

        try
        {
            resolved = state.Resolves(handle) && state.resolver->GetForward(handle.slot, forward);
        }
        catch(...)
        {
            resolved = false;
        }

        ReturnResolvedVector(generic, resolved, forward, "SceneObject is not valid.");
    }

    static void ObjectSetLocalRotation(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        const auto& value = ArgumentVector(generic);

        try
        {
            if(!state.Resolves(handle))
            {
                RaiseScriptException("SceneObject is not valid.");
                return;
            }

            const float rotation[3] = {value.x, value.y, value.z};
            std::string error;

            if(!state.resolver->SetLocalRotation(handle.slot, rotation, error))
            {
                RaiseScriptException(Bounded(error).c_str());
            }
        }
        catch(...)
        {
            RaiseScriptException("SceneObject rotation request failed.");
        }
    }

    static void InputWasKeyPressed(asIScriptGeneric* generic)
    {
        InputQuery(generic, true);
    }

    static void ObjectGetPosition(asIScriptGeneric* generic)
    {
        const auto& state = *static_cast<const Impl*>(generic->GetAuxiliary());
        const auto& handle = *static_cast<const ObjectHandle*>(generic->GetObject());
        float position[3] = {};

        try
        {
            if(!state.Resolves(handle) || !state.resolver->GetPosition(handle.slot, position))
            {
                if(auto* context = asGetActiveContext())
                {
                    context->SetException("SceneObject is not valid.");
                }

                return;
            }

            ScriptVector3 value{position[0], position[1], position[2]};
            generic->SetReturnObject(&value);
        }
        catch(...)
        {
            if(auto* context = asGetActiveContext())
            {
                context->SetException("SceneObject position is unavailable.");
            }
        }
    }
};

ScriptHost::ScriptHost() : m_impl(std::make_unique<Impl>())
{
}

ScriptHost::~ScriptHost()
{
    m_impl->Release();
}

Result ScriptHost::Initialize()
{
    return InitializeWithLogDeclaration("void Info(const string &in)");
}

Result ScriptHost::InitializeWithLogDeclaration(const char* declaration)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    state.Release();
    state.result = {Status::Failed};
    std::unique_ptr<asIScriptEngine, EngineDeleter> candidate(asCreateScriptEngine(ANGELSCRIPT_VERSION));

    if(!candidate)
    {
        AddDiagnostic(state.result, "Cannot create AngelScript 2.38.0 engine.");
        return state.result;
    }

    const auto require = [](int code)
    {
        if(code < 0)
        {
            throw std::runtime_error("AngelScript initialization failed with status " + std::to_string(code));
        }
    };

    try
    {
        require(candidate->SetMessageCallback(asFUNCTION(Impl::Message), &state, asCALL_CDECL));
        // No global initializer or module teardown script execution is admitted.
        require(candidate->SetEngineProperty(asEP_DISALLOW_GLOBAL_VARS, true));
        require(candidate->SetEngineProperty(asEP_INIT_GLOBAL_VARS_AFTER_BUILD, false));
        require(candidate->SetEngineProperty(asEP_MAX_STACK_SIZE, 256 * 1024));
        require(candidate->SetEngineProperty(asEP_MAX_CALL_STACK_SIZE, 128));
        RegisterStdString(candidate.get());

        if(!candidate->GetTypeInfoByDecl("string"))
        {
            throw std::runtime_error("AngelScript string registration failed.");
        }

        require(candidate->SetDefaultNamespace("Log"));
        require(candidate->RegisterGlobalFunction(declaration, asFUNCTION(Impl::Info), asCALL_CDECL_OBJLAST, &state));
        require(candidate->SetDefaultNamespace(""));

        // AS.5 value types. Generic calling convention avoids native ABI assumptions for
        // by-value returns; both types are plain data copied bitwise and constructed zeroed.
        const asDWORD vectorFlags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_C | asOBJ_APP_CLASS_ALLFLOATS;
        require(candidate->RegisterObjectType("Vector3", sizeof(ScriptVector3), vectorFlags));
        require(candidate->RegisterObjectBehaviour("Vector3", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(ConstructVector3), asCALL_GENERIC));
        require(candidate->RegisterObjectBehaviour("Vector3", asBEHAVE_CONSTRUCT, "void f(float, float, float)",
            asFUNCTION(ConstructVector3Components), asCALL_GENERIC));
        require(candidate->RegisterObjectProperty("Vector3", "float x", offsetof(ScriptVector3, x)));
        require(candidate->RegisterObjectProperty("Vector3", "float y", offsetof(ScriptVector3, y)));
        require(candidate->RegisterObjectProperty("Vector3", "float z", offsetof(ScriptVector3, z)));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 opAdd(const Vector3 &in) const",
            asFUNCTION(VectorAdd), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 opSub(const Vector3 &in) const",
            asFUNCTION(VectorSubtract), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 opMul(float) const",
            asFUNCTION(VectorScale), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 opMul_r(float) const",
            asFUNCTION(VectorScale), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 opDiv(float) const",
            asFUNCTION(VectorDivide), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 opNeg() const",
            asFUNCTION(VectorNegate), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 &opAddAssign(const Vector3 &in)",
            asFUNCTION(VectorAddAssign), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 &opSubAssign(const Vector3 &in)",
            asFUNCTION(VectorSubtractAssign), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 &opMulAssign(float)",
            asFUNCTION(VectorScaleAssign), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "float Length() const",
            asFUNCTION(VectorLength), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector3", "Vector3 Normalized() const",
            asFUNCTION(VectorNormalized), asCALL_GENERIC));

        // AS.6B Vector2 for mouse deltas.
        const asDWORD vector2Flags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_C | asOBJ_APP_CLASS_ALLFLOATS;
        require(candidate->RegisterObjectType("Vector2", sizeof(ScriptVector2), vector2Flags));
        require(candidate->RegisterObjectBehaviour("Vector2", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(ConstructVector2), asCALL_GENERIC));
        require(candidate->RegisterObjectBehaviour("Vector2", asBEHAVE_CONSTRUCT, "void f(float, float)",
            asFUNCTION(ConstructVector2Components), asCALL_GENERIC));
        require(candidate->RegisterObjectProperty("Vector2", "float x", offsetof(ScriptVector2, x)));
        require(candidate->RegisterObjectProperty("Vector2", "float y", offsetof(ScriptVector2, y)));
        require(candidate->RegisterObjectMethod("Vector2", "Vector2 opAdd(const Vector2 &in) const",
            asFUNCTION(Vector2Add), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector2", "Vector2 opSub(const Vector2 &in) const",
            asFUNCTION(Vector2Subtract), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector2", "Vector2 opMul(float) const",
            asFUNCTION(Vector2Scale), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector2", "Vector2 opMul_r(float) const",
            asFUNCTION(Vector2Scale), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("Vector2", "float Length() const",
            asFUNCTION(Vector2Length), asCALL_GENERIC));

        require(candidate->SetDefaultNamespace("Math"));
        require(candidate->RegisterGlobalFunction("float Sin(float)", asFUNCTION(MathSin), asCALL_GENERIC));
        require(candidate->RegisterGlobalFunction("float Cos(float)", asFUNCTION(MathCos), asCALL_GENERIC));
        require(candidate->RegisterGlobalFunction("float Clamp(float, float, float)", asFUNCTION(MathClamp), asCALL_GENERIC));
        require(candidate->RegisterGlobalFunction("float ToRadians(float)", asFUNCTION(MathToRadians), asCALL_GENERIC));
        require(candidate->RegisterGlobalProperty("const float Pi", const_cast<float*>(&MathPi)));
        require(candidate->SetDefaultNamespace(""));

        // AS.6A keys and gameplay input queries.
        require(candidate->RegisterEnum("Key"));
        require(candidate->RegisterEnumValue("Key", "W", static_cast<int>(Key::W)));
        require(candidate->RegisterEnumValue("Key", "A", static_cast<int>(Key::A)));
        require(candidate->RegisterEnumValue("Key", "S", static_cast<int>(Key::S)));
        require(candidate->RegisterEnumValue("Key", "D", static_cast<int>(Key::D)));
        require(candidate->RegisterEnumValue("Key", "Space", static_cast<int>(Key::Space)));
        require(candidate->SetDefaultNamespace("Input"));
        require(candidate->RegisterGlobalFunction("bool IsKeyDown(Key)", asFUNCTION(Impl::InputIsKeyDown),
            asCALL_GENERIC, &state));
        require(candidate->RegisterGlobalFunction("bool WasKeyPressed(Key)", asFUNCTION(Impl::InputWasKeyPressed),
            asCALL_GENERIC, &state));
        require(candidate->RegisterGlobalFunction("Vector2 GetMouseDelta()", asFUNCTION(Impl::InputGetMouseDelta),
            asCALL_GENERIC, &state));
        require(candidate->RegisterGlobalFunction("void SetCursorLocked(bool)", asFUNCTION(Impl::InputSetCursorLocked),
            asCALL_GENERIC, &state));
        require(candidate->SetDefaultNamespace(""));

        const asDWORD objectFlags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_C | asOBJ_APP_CLASS_ALLINTS;
        require(candidate->RegisterObjectType("SceneObject", sizeof(ObjectHandle), objectFlags));
        require(candidate->RegisterObjectBehaviour("SceneObject", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(ConstructObjectHandle), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("SceneObject", "bool IsValid() const",
            asFUNCTION(Impl::ObjectIsValid), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("SceneObject", "string GetName() const",
            asFUNCTION(Impl::ObjectGetName), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("SceneObject", "Vector3 GetPosition() const",
            asFUNCTION(Impl::ObjectGetPosition), asCALL_GENERIC, &state));

        require(candidate->RegisterObjectType("CharacterController", sizeof(ObjectHandle), objectFlags));
        require(candidate->RegisterObjectBehaviour("CharacterController", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(ConstructObjectHandle), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("CharacterController", "bool IsValid() const",
            asFUNCTION(Impl::ControllerIsValid), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("CharacterController", "void Move(const Vector3 &in)",
            asFUNCTION(Impl::ControllerMove), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("CharacterController", "void Jump()",
            asFUNCTION(Impl::ControllerJump), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("CharacterController", "bool IsGrounded() const",
            asFUNCTION(Impl::ControllerIsGrounded), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("SceneObject", "CharacterController GetCharacterController() const",
            asFUNCTION(Impl::ObjectGetCharacterController), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("CharacterController", "float GetYaw() const",
            asFUNCTION(Impl::ControllerGetYaw), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("CharacterController", "void SetYaw(float)",
            asFUNCTION(Impl::ControllerSetYaw), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("SceneObject", "SceneObject FindChild(const string &in) const",
            asFUNCTION(Impl::ObjectFindChild), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("SceneObject", "Vector3 GetLocalRotation() const",
            asFUNCTION(Impl::ObjectGetLocalRotation), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("SceneObject", "void SetLocalRotation(const Vector3 &in)",
            asFUNCTION(Impl::ObjectSetLocalRotation), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("SceneObject", "Vector3 GetForward() const",
            asFUNCTION(Impl::ObjectGetForward), asCALL_GENERIC, &state));

        // Audio.1C gameplay audio: AudioEvent value handles and the Audio namespace.
        const asDWORD audioFlags = asOBJ_VALUE | asOBJ_POD | asOBJ_APP_CLASS_C | asOBJ_APP_CLASS_ALLINTS;
        require(candidate->RegisterObjectType("AudioEvent", sizeof(AudioEventHandle), audioFlags));
        require(candidate->RegisterObjectBehaviour("AudioEvent", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(Impl::ConstructAudioEvent), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("AudioEvent", "bool IsValid() const",
            asFUNCTION(Impl::AudioEventIsValid), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "bool IsPlaying() const",
            asFUNCTION(Impl::AudioEventIsPlaying), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "void Stop(bool allowFadeOut = true)",
            asFUNCTION(Impl::AudioEventStop), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "bool SetParameter(const string &in, float)",
            asFUNCTION(Impl::AudioEventSetParameter), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "bool SetVolume(float)",
            asFUNCTION(Impl::AudioEventSetVolume), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "void Release()",
            asFUNCTION(Impl::AudioEventRelease), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "bool SetPosition(const Vector3 &in)",
            asFUNCTION(Impl::AudioEventSetPosition), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "bool AttachTo(const SceneObject &in)",
            asFUNCTION(Impl::AudioEventAttachTo), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioEvent", "bool Detach()",
            asFUNCTION(Impl::AudioEventDetach), asCALL_GENERIC, &state));

        // Audio.2C runtime control of a SceneObject's Audio Source component.
        require(candidate->RegisterObjectType("AudioSource", sizeof(ObjectHandle), objectFlags));
        require(candidate->RegisterObjectBehaviour("AudioSource", asBEHAVE_CONSTRUCT, "void f()",
            asFUNCTION(ConstructObjectHandle), asCALL_GENERIC));
        require(candidate->RegisterObjectMethod("SceneObject", "AudioSource GetAudioSource() const",
            asFUNCTION(Impl::ObjectGetAudioSource), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioSource", "bool IsValid() const",
            asFUNCTION(Impl::AudioSourceIsValid), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioSource", "void Play()",
            asFUNCTION(Impl::AudioSourcePlay), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioSource", "void Stop(bool allowFadeOut = true)",
            asFUNCTION(Impl::AudioSourceStop), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioSource", "bool IsPlaying() const",
            asFUNCTION(Impl::AudioSourceIsPlaying), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioSource", "void SetEnabled(bool)",
            asFUNCTION(Impl::AudioSourceSetEnabled), asCALL_GENERIC, &state));
        require(candidate->RegisterObjectMethod("AudioSource", "bool IsEnabled() const",
            asFUNCTION(Impl::AudioSourceIsEnabled), asCALL_GENERIC, &state));

        require(candidate->SetDefaultNamespace("Audio"));
        require(candidate->RegisterGlobalFunction("AudioEvent Play(const string &in)",
            asFUNCTION(Impl::AudioPlay), asCALL_GENERIC, &state));
        require(candidate->RegisterGlobalFunction("AudioEvent PlayAt(const string &in, const Vector3 &in)",
            asFUNCTION(Impl::AudioPlayAt), asCALL_GENERIC, &state));
        require(candidate->RegisterGlobalFunction("bool SetGlobalParameter(const string &in, float)",
            asFUNCTION(Impl::AudioSetGlobalParameter), asCALL_GENERIC, &state));
        require(candidate->RegisterGlobalFunction("bool SetBusVolume(const string &in, float)",
            asFUNCTION(Impl::AudioSetBusVolume), asCALL_GENERIC, &state));
        require(candidate->RegisterGlobalFunction("bool SetVCAVolume(const string &in, float)",
            asFUNCTION(Impl::AudioSetVCAVolume), asCALL_GENERIC, &state));
        require(candidate->SetDefaultNamespace(""));

        // Build an empty module to validate the complete configuration, including the
        // upstream string add-on whose registration entry point returns void.
        std::unique_ptr<asIScriptModule, ModuleDeleter> probe(candidate->GetModule("configuration", asGM_ALWAYS_CREATE));

        if(!probe)
        {
            throw std::runtime_error("Cannot allocate configuration module.");
        }

        require(probe->Build());
        probe.reset();
        state.engine = std::move(candidate);
        state.world = ++g_worldCounter;
        state.result.status = Status::Ready;
    }
    catch(const std::exception& error)
    {
        AddDiagnostic(state.result, error.what());
    }

    return state.result;
}

Result ScriptHost::Shutdown()
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    state.Release();
    state.section.clear();
    state.result = {Status::NotInitialized};
    return state.result;
}

Result ScriptHost::Compile(std::string_view section, std::string_view source)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    state.result = {Status::Failed};

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    // Failed compilation never leaves a stale previous module executable.
    state.module.reset();

    if(section.empty() || section.size() > 256 || section.find('\0') != std::string_view::npos ||
        source.empty() || source.size() > SourceLimit || source.find('\0') != std::string_view::npos)
    {
        AddDiagnostic(state.result, "Invalid section name or source size/encoding.");
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    state.section = section;
    std::unique_ptr<asIScriptModule, ModuleDeleter> candidate(state.engine->GetModule("isolated", asGM_ALWAYS_CREATE));

    if(!candidate || candidate->AddScriptSection(state.section.c_str(), source.data(), source.size()) < 0 ||
        candidate->Build() < 0)
    {
        AddDiagnostic(state.result, "In-memory compilation failed.", section);
        return state.result;
    }

    // The isolated AS.2 path runs global functions only; classes run through behaviour modules.
    if(candidate->GetObjectTypeCount() != 0)
    {
        AddDiagnostic(state.result, "Script classes run only as EAScript behaviours, not in the isolated host.", section);
        return state.result;
    }

    state.module = std::move(candidate);
    state.result.status = Status::Compiled;
    return state.result;
}

Result ScriptHost::ValidateSources(std::string_view rootSection, const SourceReader& reader, SourceLimits limits)
{
    return BuildClosure(rootSection, reader, limits, nullptr);
}

Result ScriptHost::BuildSources(std::string_view rootSection, const SourceReader& reader, ModuleID& module,
    SourceLimits limits)
{
    return BuildClosure(rootSection, reader, limits, &module);
}

Result ScriptHost::BuildClosure(std::string_view rootSection, const SourceReader& reader, SourceLimits limits,
    ModuleID* keep)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    state.module.reset();
    state.result = {Status::Failed};
    SourceCollection collection{reader, limits, state.result};
    std::unique_ptr<asIScriptModule, ModuleDeleter> built;
    struct ValidationScope
    {
        bool& busy;
        ~ValidationScope()
        {
            busy = false;
        }
    } validationScope{state.executing};
    state.executing = true;
    collection.snapshot.rootSection = Bounded(rootSection);

    try
    {
        if(!reader || limits.sections == 0 || limits.sections > 64 || limits.depth == 0 || limits.depth > 16 ||
            limits.sectionBytes == 0 || limits.sectionBytes > SourceLimit || limits.totalBytes == 0 ||
            limits.totalBytes > 8 * SourceLimit)
        {
            throw std::runtime_error("Invalid source closure limits.");
        }

        CanonicalScriptSection(rootSection);
        CScriptBuilder builder;
        builder.SetIncludeCallback(SourceCollection::Include, &collection);
        const std::string moduleName = keep ? "behaviour-" + std::to_string(state.behaviours.size()) : "validation-only";

        if(builder.StartNewModule(state.engine.get(), moduleName.c_str()) < 0)
        {
            throw std::runtime_error("Cannot prepare source validation module.");
        }

        built.reset(builder.GetModule());

        // The engine section is part of every module but never of the project source closure.
        if(builder.AddSectionFromMemory(EAScriptSection, EAScriptSource) < 0)
        {
            throw std::runtime_error("Cannot add the EAScript engine section.");
        }

        collection.Load(std::string(rootSection), builder);
        collection.snapshot.complete = true;

        if(builder.BuildModule() >= 0)
        {
            state.result.classes = FindBehaviourClasses(*built);
            state.result.status = Status::Compiled;
        }
    }
    catch(const std::exception& error)
    {
        AddDiagnostic(state.result, error.what(), rootSection);
    }

    for(auto& [name, source] : collection.sources)
    {
        collection.snapshot.sections.push_back(std::move(source));
    }

    collection.snapshot.revision = SourceClosureRevision(collection.snapshot);
    AttributeDiagnostics(state.result, collection.snapshot);
    state.result.snapshot = std::make_shared<const SourceSnapshot>(std::move(collection.snapshot));

    if(keep && state.result.status == Status::Compiled)
    {
        *keep = state.behaviours.size();
        state.behaviours.push_back({std::move(built), state.result.snapshot});
    }

    return state.result;
}

Result ScriptHost::ValidateSnapshot(const SourceSnapshot& snapshot, SourceLimits limits)
{
    return BuildSnapshotClosure(snapshot, limits, nullptr);
}

Result ScriptHost::BuildSnapshot(const SourceSnapshot& snapshot, ModuleID& module, SourceLimits limits)
{
    return BuildSnapshotClosure(snapshot, limits, &module);
}

Result ScriptHost::BuildSnapshotClosure(const SourceSnapshot& snapshot, SourceLimits limits, ModuleID* keep)
{
    const std::size_t behavioursBefore = m_impl->behaviours.size();

    // Reapply include/path/closure policy to memory-only data. No filesystem fallback.
    auto result = BuildClosure(snapshot.rootSection, [&](std::string_view name)
    {
        const SourceSection* match = nullptr;

        for(const auto& source : snapshot.sections)
        {
            if(source.name == name)
            {
                if(match)
                {
                    throw std::runtime_error("Duplicate source section in memory snapshot.");
                }

                match = &source;
            }
        }

        if(!match)
        {
            throw std::runtime_error("Missing source section in memory snapshot.");
        }

        return *match;
    }, limits, keep);

    if(result.snapshot && snapshot.complete && result.snapshot->revision != snapshot.revision)
    {
        result.status = Status::Failed;
        result.classes.clear();
        const auto previousCount = result.diagnostics.size();
        AddDiagnostic(result, "Snapshot digest/identity or unused-section mismatch.", snapshot.rootSection);

        if(result.diagnostics.size() != previousCount)
        {
            auto& diagnostic = result.diagnostics.back();
            diagnostic.assetReference = snapshot.rootAsset;
            diagnostic.sourceRevision = result.snapshot->revision;
        }

        // A mismatched snapshot never leaves an executable module behind.
        if(m_impl->behaviours.size() > behavioursBefore)
        {
            m_impl->behaviours.pop_back();
        }
    }

    return result;
}

Result ScriptHost::Execute(std::string_view declaration, ExecutionLimits limits)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    state.result = {Status::Failed};

    if(!state.module || declaration.empty() || declaration.size() > 256 ||
        declaration.find('\0') != std::string_view::npos || !ValidLimits(limits))
    {
        AddDiagnostic(state.result, "Execution requires a compiled module and bounded limits.");
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    const std::string functionDeclaration(declaration);
    auto* function = state.module->GetFunctionByDecl(functionDeclaration.c_str());

    if(!function)
    {
        AddDiagnostic(state.result, "Missing function: " + functionDeclaration, state.section);
        state.result.status = Status::MissingFunction;
        return state.result;
    }

    if(function->GetParamCount() != 0 || function->GetReturnTypeId() != asTYPEID_VOID)
    {
        AddDiagnostic(state.result, "AS.2 execution requires void with no parameters.", state.section);
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    state.Run(*function, nullptr, limits, state.section, nullptr);
    return state.result;
}

Result ScriptHost::CreateInstance(ModuleID module, std::string_view className, std::uint32_t ownerSlot,
    InstanceID& instance, ExecutionLimits limits)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    state.result = {Status::Failed};

    if(module >= state.behaviours.size() || !ValidLimits(limits))
    {
        AddDiagnostic(state.result, "Instance creation requires a built module and bounded limits.");
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    const auto& behaviour = state.behaviours[module];
    const auto& snapshot = *behaviour.snapshot;
    const auto classes = FindBehaviourClasses(*behaviour.module);

    if(std::find(classes.begin(), classes.end(), className) == classes.end())
    {
        AddDiagnostic(state.result, "Class '" + std::string(className) + "' is not an instantiable EAScript class.",
            snapshot.rootSection);
        state.result.status = Status::MissingFunction;
        AttributeDiagnostics(state.result, snapshot);
        return state.result;
    }

    asITypeInfo* type = nullptr;

    for(asUINT index = 0; index < behaviour.module->GetObjectTypeCount() && !type; ++index)
    {
        auto* candidate = behaviour.module->GetObjectTypeByIndex(index);

        if(candidate && QualifiedName(*candidate) == className)
        {
            type = candidate;
        }
    }

    auto* factory = type ? FindDefaultFactory(*type) : nullptr;

    if(!factory)
    {
        AddDiagnostic(state.result, "Class '" + std::string(className) + "' has no default constructor.", snapshot.rootSection);
        AttributeDiagnostics(state.result, snapshot);
        return state.result;
    }

    asIScriptObject* created = nullptr;
    state.Run(*factory, nullptr, limits, snapshot.rootSection, &created);
    std::unique_ptr<asIScriptObject, ObjectDeleter> object(created);

    if(state.result.status != Status::Finished || !object)
    {
        if(state.result.status == Status::Finished)
        {
            state.result.status = Status::Failed;
            AddDiagnostic(state.result, "Constructor returned no object.", snapshot.rootSection);
        }

        AttributeDiagnostics(state.result, snapshot);
        return state.result;
    }

    // Bind the owner after construction: the handle is valid from OnStart onwards.
    bool bound = false;

    for(asUINT index = 0; index < object->GetPropertyCount() && !bound; ++index)
    {
        if(std::strcmp(object->GetPropertyName(index), OwnerProperty) == 0)
        {
            *static_cast<ObjectHandle*>(object->GetAddressOfProperty(index)) = {state.world, ownerSlot, 0};
            bound = true;
        }
    }

    if(!bound)
    {
        state.result.status = Status::Failed;
        AddDiagnostic(state.result, "EAScript owner property is missing.", snapshot.rootSection);
        return state.result;
    }

    instance = state.instances.size();
    state.instances.push_back({std::move(object), module});
    return state.result;
}

Result ScriptHost::Invoke(InstanceID instance, Lifecycle callback, ExecutionLimits limits)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    state.result = {Status::Failed};

    if(instance >= state.instances.size() || !ValidLimits(limits))
    {
        AddDiagnostic(state.result, "Lifecycle call requires a live instance and bounded limits.");
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    auto& target = state.instances[instance];
    const auto& snapshot = *state.behaviours[target.module].snapshot;
    const char* declaration = callback == Lifecycle::Start ? "void OnStart()" : "void OnStop()";
    // Virtual lookup: the most derived override, or EAScript's empty default.
    auto* method = target.object->GetObjectType()->GetMethodByDecl(declaration);

    if(!method)
    {
        AddDiagnostic(state.result, std::string("Missing lifecycle method: ") + declaration, snapshot.rootSection);
        state.result.status = Status::MissingFunction;
        AttributeDiagnostics(state.result, snapshot);
        return state.result;
    }

    state.Run(*method, target.object.get(), limits, snapshot.rootSection, nullptr);
    AttributeDiagnostics(state.result, snapshot);
    return state.result;
}

Result ScriptHost::Update(InstanceID instance, float deltaTime, ExecutionLimits limits)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    state.result = {Status::Failed};

    if(instance >= state.instances.size() || !ValidLimits(limits) || !std::isfinite(deltaTime) || deltaTime < 0.0f)
    {
        AddDiagnostic(state.result, "Update requires a live instance, bounded limits and a finite delta time.");
        state.result.status = Status::InvalidRequest;
        return state.result;
    }

    auto& target = state.instances[instance];
    const auto& snapshot = *state.behaviours[target.module].snapshot;
    // Virtual lookup: the most derived override, or EAScript's empty default.
    auto* method = target.object->GetObjectType()->GetMethodByDecl("void OnUpdate(float)");

    if(!method)
    {
        AddDiagnostic(state.result, "Missing lifecycle method: void OnUpdate(float)", snapshot.rootSection);
        state.result.status = Status::MissingFunction;
        AttributeDiagnostics(state.result, snapshot);
        return state.result;
    }

    state.Run(*method, target.object.get(), limits, snapshot.rootSection, nullptr, &deltaTime);
    AttributeDiagnostics(state.result, snapshot);
    return state.result;
}

Result ScriptHost::DescribeApi(std::string& declarations)
{
    auto& state = *m_impl;

    if(!state.IsAvailable())
    {
        return {Status::InvalidRequest};
    }

    state.result = {Status::Failed};

    if(!state.engine)
    {
        return {Status::NotInitialized};
    }

    // EAScript is script source, so engine reflection does not list it. Compile the same embedded section every
    // behaviour module receives, in a module of its own, and reflect the resulting class.
    std::unique_ptr<asIScriptModule, ModuleDeleter> baseModule(
        state.engine->GetModule("api-declarations", asGM_ALWAYS_CREATE));

    if(!baseModule || baseModule->AddScriptSection(EAScriptSection, EAScriptSource) < 0 || baseModule->Build() < 0)
    {
        AddDiagnostic(state.result, "Cannot compile the EAScript base class for API declarations.", EAScriptSection);
        return state.result;
    }

    const asITypeInfo* scriptBase = baseModule->GetTypeInfoByName("EAScript");

    if(!scriptBase)
    {
        AddDiagnostic(state.result, "The EAScript base class is missing from its section.", EAScriptSection);
        return state.result;
    }

    try
    {
        declarations = WriteScriptApiDeclarations(*state.engine, scriptBase);
    }
    catch(const std::exception& error)
    {
        AddDiagnostic(state.result, error.what());
        return state.result;
    }

    state.result.status = Status::Finished;
    return state.result;
}

void ScriptHost::SetObjectResolver(ObjectResolver* resolver) noexcept
{
    m_impl->resolver = resolver;
}

void ScriptHost::SetInputSource(InputSource* input) noexcept
{
    m_impl->input = input;
}

void ScriptHost::SetAudioService(AudioService* audio) noexcept
{
    m_impl->audio = audio;
}
}

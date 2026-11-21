#include "ShaderVariants.h"

#include <atomic>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr ShaderVariant kShaderVariants[] = {
#include "ShaderVariantTable.inl"
    };

    // A chain longer than the table must repeat a variant.
    constexpr std::size_t kMaximumChainLength = std::size(kShaderVariants);

    std::atomic<bool> g_enabled{true};

    const std::unordered_map<std::string_view, const ShaderVariant*>& Index()
    {
        static const std::unordered_map<std::string_view, const ShaderVariant*> index = []
        {
            std::unordered_map<std::string_view, const ShaderVariant*> variants;

            for(const ShaderVariant& variant : kShaderVariants)
            {
                variants.emplace(variant.path, &variant);
            }

            return variants;
        }();

        return index;
    }

    // Space-separated words of a table column.
    std::vector<std::string_view> Words(std::string_view text)
    {
        std::vector<std::string_view> words;

        while(!text.empty())
        {
            const std::size_t start = text.find_first_not_of(' ');

            if(start == std::string_view::npos)
            {
                break;
            }

            text.remove_prefix(start);
            const std::size_t end = text.find(' ');
            words.push_back(text.substr(0, end));
            text.remove_prefix(end == std::string_view::npos ? text.size() : end);
        }

        return words;
    }

    // An include of a shader-root-relative file, spelled relative to the variant's directory as its wrapper's was.
    std::string IncludeLine(std::string_view variantPath, std::string_view includedPath)
    {
        const std::filesystem::path directory = std::filesystem::path(variantPath).parent_path();
        const std::filesystem::path included(includedPath);
        const std::filesystem::path spelled = directory.empty() ? included : included.lexically_relative(directory);
        return "#include \"" + spelled.generic_string() + "\"\n";
    }

    std::string DefineLine(std::string_view definition)
    {
        const std::size_t equals = definition.find('=');

        if(equals == std::string_view::npos)
        {
            return "#define " + std::string(definition) + "\n";
        }

        return "#define " + std::string(definition.substr(0, equals)) + " " + std::string(definition.substr(equals + 1)) +
            "\n";
    }
}

namespace ShaderVariants
{
    std::span<const ShaderVariant> All()
    {
        return kShaderVariants;
    }

    const ShaderVariant* Find(const std::filesystem::path& relativePath)
    {
        if(!Enabled())
        {
            return nullptr;
        }

        const std::string key = relativePath.lexically_normal().generic_string();
        const auto& index = Index();
        const auto found = index.find(key);
        return found == index.end() ? nullptr : found->second;
    }

    const ShaderVariant* FindUnder(const std::filesystem::path& shaderRoot, const std::filesystem::path& sourcePath)
    {
        if(shaderRoot.empty() || sourcePath.empty() || !Enabled())
        {
            return nullptr;
        }

        std::error_code error;
        const std::filesystem::path root = std::filesystem::absolute(shaderRoot, error).lexically_normal();
        const std::filesystem::path source = std::filesystem::absolute(sourcePath, error).lexically_normal();

        if(error)
        {
            return nullptr;
        }

        const std::filesystem::path relative = source.lexically_relative(root);

        if(relative.empty() || *relative.begin() == "..")
        {
            return nullptr;
        }

        return Find(relative);
    }

    std::optional<std::string> Source(const ShaderVariant& variant)
    {
        std::string definitions;
        std::vector<std::string_view> afterLists;
        const ShaderVariant* current = &variant;
        std::string_view sourceFile;

        // Walk the base chain outermost first: each level's definitions precede the next level's, as nested wrappers did.
        for(std::size_t depth = 0; current; ++depth)
        {
            if(depth >= kMaximumChainLength)
            {
                return std::nullopt;
            }

            for(const std::string_view definition : Words(current->defines))
            {
                definitions += DefineLine(definition);
            }

            afterLists.push_back(current->after);
            const ShaderVariant* base = Index().contains(current->base) ? Index().at(current->base) : nullptr;

            if(!base)
            {
                sourceFile = current->base;
            }

            current = base;
        }

        if(sourceFile.empty())
        {
            return std::nullopt;
        }

        std::string text = definitions + IncludeLine(variant.path, sourceFile);

        // A level's `after` files followed its base's whole expansion, so the innermost level's come first.
        for(auto level = afterLists.rbegin(); level != afterLists.rend(); ++level)
        {
            for(const std::string_view file : Words(*level))
            {
                text += IncludeLine(variant.path, file);
            }
        }

        return text;
    }

    std::vector<std::string> Validate(const std::filesystem::path& shaderRoot)
    {
        std::vector<std::string> problems;
        std::unordered_set<std::string_view> names;
        std::error_code error;

        const auto resolves = [&](std::string_view path)
        {
            return Index().contains(path) || std::filesystem::is_regular_file(shaderRoot / path, error);
        };

        for(const ShaderVariant& variant : kShaderVariants)
        {
            const std::string name(variant.path);

            if(!names.insert(variant.path).second)
            {
                problems.push_back(name + ": listed twice");
            }

            if(std::filesystem::exists(shaderRoot / variant.path, error))
            {
                problems.push_back(name + ": a file exists at the variant's path and would be ambiguous");
            }

            if(!resolves(variant.base))
            {
                problems.push_back(name + ": base " + std::string(variant.base) + " is neither a variant nor a file");
            }

            for(const std::string_view file : Words(variant.after))
            {
                if(!std::filesystem::is_regular_file(shaderRoot / file, error))
                {
                    problems.push_back(name + ": after file " + std::string(file) + " does not exist");
                }
            }

            if(!Source(variant))
            {
                problems.push_back(name + ": its base chain repeats a variant");
            }
        }

        return problems;
    }

    void SetEnabledForTesting(bool enabled)
    {
        g_enabled = enabled;
    }

    bool Enabled()
    {
        return g_enabled;
    }
}

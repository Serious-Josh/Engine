// RTXPT.1A.1 offline generator for the bundled GGX single-scatter complement tables.
//
//   MaterialTableGenerator generate <output.emt> [--size N] [--lattice L] [--mapping linear|squared] [--threads T]
//   MaterialTableGenerator verify <file.emt> [expected-sha256]
//
// "generate" writes the table file and "<output>.sha256", and prints the hash. "verify" re-reads and validates
// a file and, when given, compares its SHA-256. The output is deterministic for a given toolchain.

#include "Tools/MaterialTableGenerator/MaterialTableGeneration.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    bool ComputeFileSha256(const std::filesystem::path& path, std::string& hexadecimal)
    {
        std::vector<std::byte> bytes;
        return MaterialTables::ReadMaterialTableBytes(path, bytes) && MaterialTables::ComputeSha256(bytes, hexadecimal);
    }

    int Usage()
    {
        std::cerr << "Usage:\n"
                     "  MaterialTableGenerator generate <output.emt> [--size N] [--lattice L] "
                     "[--mapping linear|squared] [--threads T]\n"
                     "  MaterialTableGenerator verify <file.emt> [expected-sha256]\n";
        return 2;
    }

    int Generate(int argumentCount, char** arguments)
    {
        const std::filesystem::path output = arguments[2];
        MaterialTables::GenerationSettings settings;

        for(int index = 3; index + 1 < argumentCount; index += 2)
        {
            const std::string option = arguments[index];
            const std::string value = arguments[index + 1];

            if(option == "--size")
            {
                settings.tableSize = static_cast<std::uint32_t>(std::stoul(value));
            }
            else if(option == "--lattice")
            {
                settings.latticeSize = static_cast<std::uint32_t>(std::stoul(value));
            }
            else if(option == "--mapping")
            {
                if(value != "linear" && value != "squared")
                {
                    return Usage();
                }

                settings.cosineMapping = value == "linear" ? MaterialTables::CosineMapping::Linear :
                    MaterialTables::CosineMapping::Squared;
            }
            else if(option == "--threads")
            {
                settings.threadCount = static_cast<unsigned int>(std::stoul(value));
            }
            else
            {
                return Usage();
            }
        }

        if(settings.tableSize < 2 || settings.tableSize > MaterialTables::MaximumTableSize || settings.latticeSize == 0)
        {
            return Usage();
        }

        const auto start = std::chrono::steady_clock::now();
        const MaterialTables::MaterialTableSet tables = MaterialTables::GenerateMaterialTables(settings);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::string error;

        if(!MaterialTables::WriteMaterialTableFile(output, tables, &error))
        {
            std::cerr << "FAIL: " << error << '\n';
            return 1;
        }

        std::string hash;

        if(!ComputeFileSha256(output, hash))
        {
            std::cerr << "FAIL: cannot hash " << output.string() << '\n';
            return 1;
        }

        std::ofstream(output.string() + ".sha256") << hash << "  " << output.filename().string() << '\n';
        std::cout << "Generated " << output.string() << " (" << settings.tableSize << "x" << settings.tableSize
                  << ", lattice " << settings.latticeSize << "^2, "
                  << (settings.cosineMapping == MaterialTables::CosineMapping::Squared ? "squared" : "linear")
                  << " cosine mapping) in " << seconds << " s\nSHA-256 " << hash << '\n';
        return 0;
    }

    int Verify(int argumentCount, char** arguments)
    {
        const std::filesystem::path path = arguments[2];
        MaterialTables::MaterialTableSet tables;
        std::string error;

        if(!MaterialTables::ReadMaterialTableFile(path, tables, &error))
        {
            std::cerr << "FAIL: " << error << '\n';
            return 1;
        }

        std::string hash;

        if(!ComputeFileSha256(path, hash))
        {
            std::cerr << "FAIL: cannot hash " << path.string() << '\n';
            return 1;
        }

        std::cout << "Valid " << path.string() << "\nSHA-256 " << hash << '\n';

        if(argumentCount > 3 && hash != arguments[3])
        {
            std::cerr << "FAIL: SHA-256 differs from the expected " << arguments[3] << '\n';
            return 1;
        }

        return 0;
    }
}

int main(int argumentCount, char** arguments)
{
    if(argumentCount < 3)
    {
        return Usage();
    }

    const std::string command = arguments[1];

    if(command == "generate")
    {
        return Generate(argumentCount, arguments);
    }

    if(command == "verify")
    {
        return Verify(argumentCount, arguments);
    }

    return Usage();
}

#include "symbols.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    std::unordered_map<uint32_t, std::string> g_names;
    std::unordered_set<uint32_t> g_imports;
    std::vector<uint32_t> g_starts;

    // One CSV field starting at `pos` (quoted fields may contain commas and doubled quotes).
    std::string ReadField(const std::string& line, size_t& pos)
    {
        std::string field;
        if (pos < line.size() && line[pos] == '"')
        {
            for (pos++; pos < line.size(); pos++)
            {
                if (line[pos] == '"')
                {
                    if (pos + 1 < line.size() && line[pos + 1] == '"')
                        field += line[++pos];
                    else
                    {
                        pos++;
                        break;
                    }
                }
                else
                    field += line[pos];
            }
        }
        else
        {
            size_t end = line.find(',', pos);
            field = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? line.size() : end;
        }
        if (pos < line.size() && line[pos] == ',')
            pos++;
        return field;
    }

    uint32_t ContainingStart(uint32_t address)
    {
        auto it = std::upper_bound(g_starts.begin(), g_starts.end(), address);
        return it == g_starts.begin() ? 0 : *(it - 1);
    }
}

size_t LoadGuestSymbols(const std::filesystem::path& csvPath)
{
    g_starts.clear();
    for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
        g_starts.push_back(uint32_t(PPCFuncMappings[i].guest));
    std::sort(g_starts.begin(), g_starts.end());

    std::ifstream file(csvPath);
    if (!file)
        return 0;

    // address,name,source,note
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t pos = 0;
        std::string address = ReadField(line, pos);
        std::string name = ReadField(line, pos);
        std::string source = ReadField(line, pos);
        if (address.empty() || name.empty())
            continue;

        uint32_t guest = uint32_t(strtoul(address.c_str(), nullptr, 16));
        g_names[guest] = std::move(name);
        if (source == "import")
            g_imports.insert(guest);
    }
    return g_names.size();
}

const char* GuestSymbolName(uint32_t address)
{
    auto it = g_names.find(address);
    return it == g_names.end() ? nullptr : it->second.c_str();
}

const char* DescribeGuestCode(uint32_t address, char* buffer, size_t size)
{
    uint32_t start = ContainingStart(address);
    if (start == 0 || address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE)
    {
        snprintf(buffer, size, "0x%08X", address);
        return buffer;
    }

    char offset[16] = "";
    if (address != start)
        snprintf(offset, sizeof(offset), "+0x%X", address - start);

    const char* name = GuestSymbolName(start);
    if (name && g_imports.count(start))
        snprintf(buffer, size, "%s%s (import)", name, offset);
    else if (name)
        snprintf(buffer, size, "%s%s (sub_%08X%s)", name, offset, start, offset);
    else
        snprintf(buffer, size, "sub_%08X%s", start, offset);
    return buffer;
}

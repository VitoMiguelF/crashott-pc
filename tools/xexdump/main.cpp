// Dumps the decrypted/decompressed XEX image as a flat binary plus a section map.
#include <file.h>
#include <image.h>
#include <cstdio>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: xexdump <in.xex> <out.bin>\n"); return 1; }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    printf("base=0x%zX size=0x%X entry=0x%zX\n", image.base, image.size, image.entry_point);
    for (auto& s : image.sections)
        printf("section %-10s base=0x%zX size=0x%X flags=%u\n", s.name.c_str(), s.base, s.size, s.flags);
    if (argc > 3)
    {
        FILE* sf = fopen(argv[3], "w");
        for (auto& sym : image.symbols)
            fprintf(sf, "0x%08zX 0x%zX %d %s\n", sym.address, sym.size, (int)sym.type, sym.name.c_str());
        fclose(sf);
    }

    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.data.get()), image.size);
    out.close();
    printf("size=0x%X ok=%d\n", image.size, (int)!out.fail());
    return out.fail() ? 2 : 0;
}

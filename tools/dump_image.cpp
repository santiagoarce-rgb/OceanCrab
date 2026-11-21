// dump_image: load + relocate a PSP ELF/PRX exactly like the runtime does and
// write the resulting guest image to a file, plus its section table, so
// offline analysis scripts (tools/symbolize/) see final addresses.
//
// usage: dump_image <EBOOT.ELF> <out.bin> <out_sections.csv>
// out.bin covers [load_base, end of the highest loaded section).
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: dump_image <EBOOT.ELF> <out.bin> <out_sections.csv>\n";
        return 2;
    }
    const auto elf = psprecomp::Elf32Image::from_file(argv[1]);
    psprecomp::GuestMemory memory;
    (void)elf.load_and_relocate(memory);
    const std::uint32_t base = psprecomp::kDefaultPspUserLoadBase;

    std::uint32_t end = base;
    std::ofstream sections(argv[3]);
    sections << "name,address,size,type,flags\n";
    for (const auto &section : elf.sections()) {
        if (section.address == 0u && section.size == 0u) continue;
        const std::uint32_t address = elf.section_runtime_address(section);
        sections << section.name << ",0x" << std::hex << address << ",0x" << section.size
                 << ",0x" << section.type << ",0x" << section.flags << std::dec << "\n";
        if ((section.flags & 0x2u) != 0u && section.type != 8u)  // SHF_ALLOC, not NOBITS
            end = std::max(end, address + section.size);
    }

    std::vector<std::uint8_t> image(end - base);
    for (std::uint32_t i = 0u; i < image.size(); ++i) image[i] = memory.load8(base + i);
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
    std::cout << "image 0x" << std::hex << base << "-0x" << end << std::dec << " (" << image.size()
              << " bytes)\n";
    return 0;
}

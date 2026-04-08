#define _CRT_SECURE_NO_WARNINGS


#include <Windows.h>
#include <process.h>
#include <TlHelp32.h>
#include <inttypes.h>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <memory>
#include <string_view>
#include <cstdint>
#include <string>
#include <cmath>
#include <thread>
#include <cassert>
#include <xstring>
#include <dwmapi.h>
#include <vector>
#include <map>
#include <array>
#include <fstream>
#include <direct.h>
#include <set>
#include <stack>
#include <unordered_set>
#include <wininet.h>
#include <random>
#include <Psapi.h>
#include <urlmon.h>      // URLDownloadToFile

// DMA Memory class (LeechCore/VMMDLL based)
#include "include/DMALibrary/Memory/Memory.h"

#pragma comment(lib, "wininet.lib")

// Safe memory reader
bool ReadSafe(uint64_t address, void* buffer, size_t size) {
    uint8_t* out = reinterpret_cast<uint8_t*>(buffer);
    for (size_t offset = 0; offset < size; offset += 0x1000) {
        size_t chunk = (size - offset < 0x1000) ? (size - offset) : 0x1000;
        if (!mem.Read(address + offset, out + offset, chunk)) {
            // Fill unreadable pages with nulls to maintain alignment
            memset(out + offset, 0, chunk);
        }
    }
    return true;
}

// Entropy calculation
double CalculateEntropy(const uint8_t* data, size_t size) {
    if (!size) return 0.0;
    int freq[256]{};

    for (size_t i = 0; i < size; i++)
        freq[data[i]]++;

    double entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (freq[i]) {
            double p = static_cast<double>(freq[i]) / size;
            entropy -= p * log2(p);
        }
    }
    return entropy;
}

// Relocations
bool FixRelocations(std::vector<uint8_t>& image, IMAGE_NT_HEADERS64& nt, uint64_t runtimeBase) {
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.VirtualAddress || !dir.Size)
        return true;

    uint64_t delta = runtimeBase - nt.OptionalHeader.ImageBase;
    uint8_t* cur = image.data() + dir.VirtualAddress;
    uint8_t* end = cur + dir.Size;

    while (cur < end) {
        auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(cur);
        cur += sizeof(*block);

        size_t count = (block->SizeOfBlock - sizeof(*block)) / sizeof(WORD);
        WORD* entries = reinterpret_cast<WORD*>(cur);

        for (size_t i = 0; i < count; i++) {
            WORD type = entries[i] >> 12;
            WORD off = entries[i] & 0xFFF;

            if (type == IMAGE_REL_BASED_DIR64) {
                uint64_t* patch = reinterpret_cast<uint64_t*>(image.data() + block->VirtualAddress + off);
                *patch += delta;
            }
        }
        cur += count * sizeof(WORD);
    }

    nt.OptionalHeader.ImageBase = runtimeBase;
    return true;
}

// Fix Import Address Table (IAT) by copying original memory addresses
bool FixIAT(uint64_t base, std::vector<uint8_t>& image, IMAGE_NT_HEADERS64* nt) {
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return true;

    // Bug #3 fix: validate import directory is within image bounds
    if (static_cast<uint64_t>(dir.VirtualAddress) + dir.Size > image.size()) return false;

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image.data() + dir.VirtualAddress);
    auto* importsEnd = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        image.data() + dir.VirtualAddress + dir.Size);

    // Bug #3 fix: bound the descriptor walk and require both Name and FirstThunk to be non-zero
    for (; imports + 1 <= importsEnd && imports->Name && imports->FirstThunk; imports++) {
        uint64_t firstThunkRVA = imports->FirstThunk;

        // Bug #3 fix: firstThunkRVA must be inside the image
        if (firstThunkRVA >= image.size()) continue;

        for (int i = 0; ; i++) {
            uint64_t offset = static_cast<uint64_t>(i) * sizeof(uint64_t);

            // Bug #3 fix: bounds-check every write before touching the buffer
            if (firstThunkRVA + offset + sizeof(uint64_t) > image.size()) break;

            uint64_t funcAddr = 0;
            // Read the actual resolved address from the live process memory
            if (!mem.Read(base + firstThunkRVA + offset, &funcAddr, sizeof(funcAddr)))
                break;

            if (funcAddr == 0) break;

            // Patch the dump's memory with the live resolved address
            *reinterpret_cast<uint64_t*>(image.data() + firstThunkRVA + offset) = funcAddr;
        }
    }
    return true;
}

// Dump module
bool DumpModule(uint64_t base, const std::string& outName) {
    IMAGE_DOS_HEADER dos{};
    if (!mem.Read((uintptr_t)base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    IMAGE_NT_HEADERS64 nt{};
    if (!mem.Read((uintptr_t)(base + dos.e_lfanew), &nt, sizeof(nt)))
        return false;

    // 1. Prepare Buffer (SizeOfImage is the size in memory)
    size_t imageSize = nt.OptionalHeader.SizeOfImage;
    std::vector<uint8_t> image(imageSize, 0);

    // 2. Read Headers
    ReadSafe(base, image.data(), nt.OptionalHeader.SizeOfHeaders);

    // 3. Read Sections
    uint64_t sectionHeaderAddr = base + dos.e_lfanew + sizeof(IMAGE_NT_HEADERS64);
    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
    mem.Read((uintptr_t)sectionHeaderAddr, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER));

    for (auto& s : sections) {
        uint64_t sectionVA = base + s.VirtualAddress;
        uint32_t sectionSize = s.Misc.VirtualSize;

        // Fix #1: Safe section name print (IMAGE_SIZEOF_SHORT_NAME=8, may not be null-terminated)
        char secName[IMAGE_SIZEOF_SHORT_NAME + 1]{};
        memcpy(secName, s.Name, IMAGE_SIZEOF_SHORT_NAME);
        std::cout << "[SEC] " << secName << " | VA: 0x" << std::hex << s.VirtualAddress << " | Size: 0x" << sectionSize;

        ReadSafe(sectionVA, image.data() + s.VirtualAddress, sectionSize);

        // Check for encryption/compression
        double entropy = CalculateEntropy(image.data() + s.VirtualAddress, sectionSize);
        std::cout << " | Entropy: " << std::dec << entropy << (entropy > 7.4 ? " [ENCRYPTED]" : "") << std::endl;

        // CRITICAL: Set PointerToRawData = VirtualAddress and SizeOfRawData = VirtualSize
        // This makes the file "Disk Layout" identical to "Memory Layout"
        auto* headerInDump = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + (sectionHeaderAddr - base) + (&s - &sections[0]) * sizeof(IMAGE_SECTION_HEADER));
        headerInDump->PointerToRawData = s.VirtualAddress;
        headerInDump->SizeOfRawData = s.Misc.VirtualSize;
    }

    // 4. Fix IAT (Optional but recommended for analysis)
    FixIAT(base, image, &nt);

    // 5. Bug #1 fix: tell analysis tools the dump is based at runtime address.
    //    The in-memory absolute pointers already reflect `base`, so instead of
    //    un-relocating we just update ImageBase. No need to touch .reloc.
    nt.OptionalHeader.ImageBase = base;

    //    Because PointerToRawData == VirtualAddress (set in the section loop),
    //    file alignment must equal section alignment, otherwise the layout is
    //    invalid per PE spec and strict loaders/parsers will reject it.
    nt.OptionalHeader.FileAlignment = nt.OptionalHeader.SectionAlignment;

    //    Bound imports cache addresses computed at link time and are stale
    //    after dumping — clear the directory so loaders don't trust it.
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress = 0;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].Size = 0;

    // 6. Bug #2 fix: write the modified NT headers back into the image buffer.
    //    All edits above were made on a stack copy; without this memcpy they
    //    would never reach the output file.
    if (dos.e_lfanew + sizeof(nt) <= image.size())
        memcpy(image.data() + dos.e_lfanew, &nt, sizeof(nt));

    // 7. Write to File
    std::ofstream f(outName, std::ios::binary);
    if (f.is_open()) {
        f.write(reinterpret_cast<char*>(image.data()), image.size());
        f.close();
        return true;
    }
    return false;
}

void SetColor(WORD color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

// ==================== Main ====================
int main() {
    SetConsoleTitleA("PE Memory Dumper (DMA)");

    const WORD COLOR_SUCCESS = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    const WORD COLOR_ERROR = FOREGROUND_RED | FOREGROUND_INTENSITY;
    const WORD COLOR_INFO = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY; // cyan

    // Initialize DMA and connect to process
    SetColor(COLOR_INFO);
    std::cout << "[*] Initializing DMA connection...\n";
    if (!mem.Init("r5apex_dx12.exe", true, false)) {
        SetColor(COLOR_ERROR);
        std::cerr << "[-] Failed to initialize DMA or find process. Is the DMA connected and game running?\n";
        system("pause"); return 1;
    }
    SetColor(COLOR_SUCCESS);
    std::cout << "[+] DMA connected! Process found!\n";
    std::cout << "[+] PID: " << std::dec << Memory::current_process.PID << "\n";

    // Module base (already obtained during Init)
    uint64_t moduleBase = Memory::current_process.base_address;
    uint64_t moduleSize = Memory::current_process.base_size;

    if (!moduleBase) {
        SetColor(COLOR_ERROR);
        std::cerr << "[-] Failed to find module base!\n";
        system("pause"); return 1;
    }

    SetColor(COLOR_SUCCESS);
    std::cout << "[+] Module base: 0x" << std::hex << moduleBase
        << "  Size: 0x" << moduleSize << std::dec << "\n";

    // Dump module
    SetColor(COLOR_INFO);
    std::cout << "[*] Dumping module...\n";
    if (!DumpModule(moduleBase, "game_dumped.exe")) {
        SetColor(COLOR_ERROR); std::cerr << "[-] Dump failed!\n"; system("pause"); return 1;
    }
    SetColor(COLOR_SUCCESS);
    std::cout << "[+] Dump completed successfully!\n";

    SetColor(COLOR_SUCCESS);
    std::cout << "[*] Press Enter to exit...\n";
    std::cin.get();

    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    return 0;
}

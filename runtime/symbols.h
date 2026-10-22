#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

// Names for guest functions, read at startup from <data>/symbols/functions.csv (paths.h; written by
// tools/rtti_scan.py from the RTTI in the image: class::vfNN, destructors, imports). The file is
// derived from the game, so it stays in private/ (dist: data/) and the runtime works without it (sub_XXXXXXXX
// only). CRASH_SYMBOLS=<path> loads another file, CRASH_SYMBOLS=0 disables the lookup.
//
// Load once before guest threads start; lookups are read-only afterwards and safe from the crash
// handler and the watchdog.
size_t LoadGuestSymbols(const std::filesystem::path& csvPath);

// Name of the function that starts exactly at `address`, or nullptr.
const char* GuestSymbolName(uint32_t address);

// Readable description of a guest code address, for reports:
//   "CLifeDisplay::vf03 (sub_821E2C78)"           function start with a name
//   "CLifeDisplay::vf03+0x1C (sub_821E2C78+0x1C)" inside it
//   "sub_82123450+0x8"                             no name
//   "XamShowSigninUI (import)"                     import thunk
// Addresses outside the recompiled functions are printed as plain hex.
const char* DescribeGuestCode(uint32_t address, char* buffer, size_t size);

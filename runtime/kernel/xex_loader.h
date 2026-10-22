#pragma once

#include <cstdint>
#include <filesystem>

struct XexModule
{
    uint32_t entryPoint{};
    uint32_t defaultStackSize{};
    uint32_t titleId{};  // from the execution info header (0x00040006)
};

// Title ID of the loaded XEX (0 before LoadXex). Used for per-title folders (XamContent*).
uint32_t CurrentTitleId();

// Maps the decrypted image at PPC_IMAGE_BASE (decrypted from default.xex at startup by
// xex_image/, or read from an image.bin written by tools/xexdump when `imagePath` is not empty)
// and reads the plaintext XEX header for the entry point and the import table.
// Function imports: XenonRecomp already turned each import thunk into a call to __imp__<Name>;
// the loader points the matching IAT slot at the thunk so reads of the IAT see a callable
// address. Variable imports (kernel data such as XboxKrnlVersion) get runtime-owned guest
// memory and their IAT slot points at it.
bool LoadXex(const std::filesystem::path& xexPath, const std::filesystem::path& imagePath, XexModule& module);

// Fills the function table read by PPC_LOOKUP_FUNC (indirect calls) from PPCFuncMappings.
// Unmapped code addresses point at a handler that reports the bad target.
void InitFunctionTable();

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vfs.h"

// Per-entry mod overrides for Radical .rcf archives (see tools/rcf.py for the format).
//
// A mod replaces one file inside an archive by placing it at
//   mods\<mod>\rcf\<archive name without .rcf>\<entry path>
// e.g. mods\textos\rcf\default\package\7efdcd91.p3d for "package\7efdcd91.p3d" in default.rcf.
// The first mod in load order wins. The archive is never rewritten: the game sees a virtual file
// equal to the original except that the directory entries of replaced files point at their new
// contents (0x800-aligned, as in the archive). The layout is compact: a new entry goes over its
// own original slot when it fits, else into the space freed by other replaced entries (best fit,
// biggest first), else after the original end. Only entries that already exist can be replaced,
// since the directory is keyed by a name hash.
//
// Size limits: the game's reader seeks with a signed 32-bit offset (core::XenonDrive::Read calls
// SetFilePointer with no high dword), so on its own it can't read past 2 GB (it then reports a
// dirty disc). fs/xenon_drive_hooks.cpp makes that seek unsigned, which leaves the 4 GB of the
// format's u32 offsets; entries that don't fit there are dropped with a warning. The final virtual
// size is logged, with a warning when it passes 2 GB.
//
// Built-in patches (RegisterBuiltinPatch) are the runtime's own same-size edits of an entry,
// independent of the mods folder (e.g. settings/frontend_patches.cpp). They are applied to the
// entry as the game would see it (the original, or the mod's file when a mod replaces it) and
// served as byte overrides on top of the virtual archive (or, for an entry a mod replaces, baked
// into an in-memory copy of the mod's file); a patcher that doesn't find what it expects in a
// mod's file leaves it unchanged. Rewrites (RegisterEntryRewrite) are the same but may change the
// entry's size. The first read of each replaced or rewritten entry is logged.

namespace rcf_overlay
{
    class Archive;

    // A patched view of `host` (the archive the game folder or a mod provides for `path`), or
    // nullptr when it isn't an .rcf on the game mount or no mod replaces any of its entries.
    // Views are cached per archive.
    std::shared_ptr<const Archive> Open(const vfs::VirtualPath& path, const std::filesystem::path& host);

    // `patcher` receives the entry's original bytes and edits them in place (same size); it returns
    // false to leave the entry unchanged (e.g. unexpected contents). `archive` is the file name
    // without .rcf ("default"), `entry` the entry path ("fighttrees\\frontend.bfig"), both
    // case-insensitive. Register before the game opens the archive.
    using EntryPatcher = std::function<bool(std::vector<uint8_t>& data)>;
    void RegisterBuiltinPatch(const std::string& archive, const std::string& entry, const std::string& description,
                              EntryPatcher patcher);

    // Like a built-in patch, but `rewriter` may change the entry's size (e.g. the language packs of
    // settings/language_pack.h rebuild text bibles); a resized entry is served from memory, laid
    // out like a mod's file. An entry's patches and rewrites run in registration
    // order, each on the result of the previous ones.
    void RegisterEntryRewrite(const std::string& archive, const std::string& entry, const std::string& description,
                              EntryPatcher rewriter);

    uint64_t Size(const Archive& archive);

    // Reads from the virtual archive; `baseFile` is the open handle of the original. Returns false
    // on a host read error. Bytes past the end are not transferred.
    bool Read(const Archive& archive, void* baseFile, uint64_t offset, uint8_t* data, uint32_t length,
              uint32_t& transferred);
}

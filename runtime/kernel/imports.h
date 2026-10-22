#pragma once

// Every xboxkrnl/xam import the recompiled code calls (`__imp__<Name>`, listed in
// import_list.inc) gets a weak stub that logs the call and returns 0 in r3. A real
// implementation is added by defining `PPC_FUNC(__imp__<Name>)` in another file of the runtime:
// the strong definition replaces the weak stub at link time, like the generated sub_* aliases.

// Name of the most recent import called (for crash reports).
const char* GetLastImportCalled();

// Records `name` (a string literal) as the most recent import; called by real implementations.
void SetLastImportCalled(const char* name);

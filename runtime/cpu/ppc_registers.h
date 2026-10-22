#pragma once

// Guest registers that config/Crash.toml may turn into C++ locals of each recompiled function
// (skip_lr, ctr_as_local): then PPCContext has no field for them, and the runtime's logs and crash
// reports see 0.
#include <ppc_config.h>
#include <ppc_context.h>

#include <cstdint>

inline uint32_t GuestLR([[maybe_unused]] const PPCContext& ctx)
{
#ifdef PPC_CONFIG_SKIP_LR
    return 0;
#else
    return uint32_t(ctx.lr);
#endif
}

inline uint32_t GuestCTR([[maybe_unused]] const PPCContext& ctx)
{
#ifdef PPC_CONFIG_CTR_AS_LOCAL
    return 0;
#else
    return ctx.ctr.u32;
#endif
}

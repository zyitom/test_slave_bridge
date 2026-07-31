/*
 * Cross-core channel glue for the EtherCAT core1 probe image: the only
 * translation unit that touches SHARE_RAM.
 *
 * Step 0 scope (../../CORE_SWAP_MIGRATION.md section 4): claim the channel,
 * validate its version, and wire the diagnostic ring. The process-data rings and
 * the ARQ endpoint stay untouched -- they move in step 1, together with the
 * doorbell reversal in step 2.
 */

#include "ecat_xcore.h"

#include <cstddef>
#include <cstdint>

#include "xcore_channel.hpp"
#include "xcore_diag.hpp"

// Included last: it defines printf as a macro, which would otherwise collide
// with the standard headers above.
#include "ecat_diag.h"

namespace {

using librmcs::firmware::ecat::kXcoreChannelVersion;
using librmcs::firmware::ecat::XcoreChannel;
using librmcs::firmware::ecat::XcoreDiagRing;

XcoreChannel* g_channel = nullptr;
// Bound separately from g_channel so ecat_diag_write() costs one load and one
// null test on the logging path, and so a version mismatch can refuse to bind
// the ring (the field offsets are exactly what a version change invalidates).
XcoreDiagRing* g_diag = nullptr;
std::uint32_t g_version = 0;

} // namespace

extern "C" bool ecat_xcore_init(void) {
    XcoreChannel& channel = librmcs::firmware::ecat::xcore_channel_wait();
    g_version = channel.version;
    if (g_version != kXcoreChannelVersion)
        return false;

    g_channel = &channel;
    g_diag = &channel.diag;
    return true;
}

extern "C" std::uint32_t ecat_xcore_channel_version(void) { return g_version; }

XcoreChannel* ecat_xcore_channel(void) { return g_channel; }

extern "C" void ecat_diag_write(const char* text, std::size_t size) {
    if (g_diag == nullptr || text == nullptr || size == 0)
        return;
    // Short return means the ring was full; dropping the remainder is the
    // documented contract (xcore_diag.hpp) -- core1 must never stall on a log.
    (void)librmcs::firmware::ecat::xcore_diag_write(*g_diag, text, size);
}

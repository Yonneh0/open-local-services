#ifndef OLS_HARDWAREINFO_H
#define OLS_HARDWAREINFO_H
// What the silicon itself states about what is bolted into the machine. This
// reads no driver and believes no marketing sheet: on Windows the machine's own
// firmware table (the SMBIOS blob the BIOS keeps for anyone who asks), on Linux
// the same table handed over under /sys/firmware/dmi. A machine whose firmware
// says nothing gets nothing - no number here is ever inferred from part names
// or guessed from the platform.
#include <string>
namespace ols
{
  namespace platform
  {
    // The memory sticks as the firmware describes them, gathered into one
    // statement about the bus they run on together. Bandwidth is arithmetic the
    // firmware itself makes possible: the speed the sticks are configured at
    // times the width of the data path they all share.
    struct MemorySticks
    {
      bool ok = false;
      std::string kind;            // what the firmware names the technology, when it names it ("DDR5")
      std::string maker;           // of the newest-numbered populated stick, if stated
      std::string part;            // its part number, if stated
      long long ratedMts = 0;      // the fastest rated speed any populated stick states (MT/s)
      long long configuredMts = 0; // the fastest configured speed any populated stick states (MT/s)
      int modules = 0;             // populated sticks the firmware describes
      long long dataBits = 0;      // the data path summed over those sticks (64 per stick of a usual pair)
      long long bytes = 0;         // bytes they hold between them

      // GB/s at decimal billions: what the bus could carry at its configured
      // speed, which is the number the rating beside the memory graph quotes.
      double bandwidthGbps() const
      {
        return (configuredMts > 0 && dataBits > 0)
                   ? static_cast<double>(configuredMts) * (static_cast<double>(dataBits) / 8.0) / 1000.0
                   : 0.0;
      }
    };

    // The sticks as this machine's firmware states them. Computed once: the
    // firmware table does not change under a standing server.
    const MemorySticks &memorySticks();

    // The picture card's own running speed, in megahertz, as the card itself
    // states it through the vendor management library - where one answers. A
    // machine whose card states nothing (an AMD graphics core, which keeps its
    // clocks out of any library this server will bind to, or a card of no maker
    // the server speaks to) reads not-ok, and the page says so rather than
    // guessing a figure from the product name.
    struct GpuClocks
    {
      bool ok = false;
      long long coreMhz = 0;    // graphics/SM clock this instant
      long long coreMaxMhz = 0; // the ceiling the card states for it
      long long memMhz = 0;     // memory clock this instant
      long long memMaxMhz = 0;  // the ceiling the card states for it
    };

    // Queried fresh on each ask: a clock is a moment, not a constant. Where no
    // vendor library answers - the usual case for an AMD core - this is cheap
    // and returns not-ok at once.
    GpuClocks gpuClocks();
  } // namespace platform
} // namespace ols
#endif // OLS_HARDWAREINFO_H

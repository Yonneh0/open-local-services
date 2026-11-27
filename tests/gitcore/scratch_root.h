// tests/gitcore/scratch_root.h — the ground every scratch of this harness stands on.
//
// The scratch folder is asked of the machine's temp first, where it has always
// stood. A machine can refuse directory creation there to this binary while a
// shell creates the same folder freely - a behavioural policy keyed on the
// program, of the kind a suite earns by once trying to remove the root
// namespace (the longpath case, in an earlier dress, did exactly that with an
// empty scratch) - and such a policy has been seen to refuse the names the
// cases use while answering a differently-named probe from the very same
// process. So this ground is decided twice over: once, up front, by a probe
// folder of the same naming shape the cases use; and again per case, where a
// refused scratch retries itself once under the folder the harness runs from.
// That second ground is disposable built-state, on the law every build root
// keeps: generated, cleaned by the cases that stand in it, gitignored.
// Whichever ground takes the scratch is reported, in a NOTE line the run
// never hides, naming both the refusal it worked around and its words.

#ifndef OLS_TESTS_SCRATCH_ROOT_H
#define OLS_TESTS_SCRATCH_ROOT_H

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace ols_test {

// The stamp the scratch names carry: unique within a run, so two runs and two
// cases of the same use never read each other's folders.
inline std::string scratchStamp()
{
    return std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// The second ground: a scratch folder beside the folder the harness runs from,
// made only if asked. Empty means even that refused, and says so through the
// cases that stand on it.
inline const std::filesystem::path &scratchAlt()
{
    static std::filesystem::path alt;
    static bool asked = false;
    if (!asked)
    {
        asked = true;
        std::error_code made{};
        const std::filesystem::path p =
            std::filesystem::current_path() / "ols-test-scratch";
        // Scratch, by law: whatever an earlier run left here is removed
        // before this one stands in it, the same clean start the temp ground
        // has always given a fresh-named folder.
        std::error_code gone{};
        std::filesystem::remove_all(p, gone);
        std::filesystem::create_directories(p, made);
        if (!made)
            alt = p;
        else
            std::cout << "NOTE the run folder " << p.string()
                      << " refuses a scratch ground too: " << made.message() << "\n";
    }
    return alt;
}

// The ground chosen for this run, decided once: temp when it answers a probe
// folder of the cases' own naming shape (folder, a file inside it, its own
// removal), the run folder when it does not.
inline const std::filesystem::path &scratchRoot()
{
    static std::filesystem::path chosen;
    static bool asked = false;
    if (asked)
        return chosen;
    asked = true;

    std::error_code ec{};
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
    if (!ec && !temp.empty())
    {
        const std::filesystem::path probe = temp / ("ols-b1-ground-" + scratchStamp());
        std::error_code one{};
        std::filesystem::create_directories(probe, one);
        if (!one)
        {
            std::ofstream note{probe / "probe.txt", std::ios::binary};
            note << "x";
            note.close();
            if (note.good())
            {
                std::filesystem::remove_all(probe, one);
                chosen = temp;
                return chosen;
            }
        }
        std::cout << "NOTE the temp folder " << temp.string()
                  << " refuses this harness's scratch (" << one.message()
                  << "); the scratch will stand beside the run folder\n";
    }
    else
    {
        std::cout << "NOTE the machine names no temp folder (" << ec.message()
                  << "); the scratch will stand beside the run folder\n";
    }
    chosen = scratchAlt();
    if (!chosen.empty())
        std::cout << "NOTE scratch ground: " << chosen.string() << "\n";
    return chosen;
}

} // namespace ols_test

#endif // OLS_TESTS_SCRATCH_ROOT_H

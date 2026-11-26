// open-local-services status view: composes the services' reports into one
// monitor document. The engines own their jobs; this only reads them.
#ifndef OLS_MONITORSERVICE_H
#define OLS_MONITORSERVICE_H

#include <functional>
#include <string>

namespace ols
{
    namespace monitor
    {

        // Composes what the engines report into one monitor document: every
        // image job, in one list newest first, tagged with the engine the work
        // belongs to, plus the counts the home page shows.
        // The engines own their jobs; this only reads them.
        // The timings argument is the way to the picture engine's cost ledger,
        // not the ledger itself: the queue estimate asks for it only while jobs
        // actually stand, so an idle machine never reads the file.
        std::string snapshot(const std::string &systemJson, const std::string &imageJobsJson,
                             const std::function<std::string()> &timings);

    } // namespace monitor
} // namespace ols

#endif // OLS_MONITORSERVICE_H

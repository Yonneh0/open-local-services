#pragma once
// Setting this machine's server to start by itself, from where it stands now.
//
// Nothing is installed by any of this. The server that is asked to start on its own
// is this one, out of this folder, with this room and these settings: what changes
// is what the machine does at boot. The words a page shows are the words the
// machine's own manager answered with, kept whole rather than tidied.
//
// Linux already keeps a unit for this server, so a machine with one has it read,
// enabled and started where it stands; a machine without one gets a unit written
// beside the folders it names, pointing at the server that is running now.
//
// Windows cannot carry a console program in the service control manager: a program
// that never answers the service control handler is written off as failed and
// stopped at boot. So there, starting on boot is a system task that starts when the
// machine starts - which is how these boxes start their own servers today - and the
// task carries the folder to start from. Where a real service of this name does
// exist on Windows, it is read, set, started, stopped and deleted with sc, because
// that is what it is.
//
// Every action states what it asked and what came back. A server that is not
// elevated, or not root, is refused by its own machine and says so with that
// machine's words rather than pretending the change was made.
#ifndef OLS_DEPLOYMENT_H
#define OLS_DEPLOYMENT_H

#include <string>
#include <vector>

namespace ols
{
  namespace deploy
  {
    // What this machine's manager says about this server right now.
    struct State
    {
      std::string name;    // the service or task this server answers as
      std::string manager; // systemd, the service control manager, or the task scheduler
      bool installed = false;
      bool running = false;
      // automatic: starts when the machine starts. manual: started by hand.
      // disabled: the machine was told to leave it alone. not installed: nothing
      // on this machine knows about this server yet.
      std::string startup;
      std::string target; // the program the manager would start
      std::string root;   // the folder it would start from
      std::string detail; // where the unit or task lives, or what could not be read
    };

    State state();

    // The state above, and the words of the last action this server was asked to
    // take, so a page that repaints keeps saying what happened.
    std::string stateJson();

    // What to do: startup_on (set up and start on boot), startup_off (leave it
    // alone at boot, without stopping anything that is running), remove (take it
    // off the machine's list), shutdown (stop it, and let this process go with it).
    // `words` is what was asked and what the manager said; false means the change
    // did not happen, and the words say why.
    bool act(const std::string &action, std::string &words);

    // The words this server was started with, kept for the soft hand below.
    void rememberStart(const std::vector<std::string> &args);

    // The full path of the program this process runs as - what a hand replaces,
    // and what its clock is read against.
    std::string selfProgram();

    // The soft hand: a person may ask this server to start again with the program
    // that stands on disk now - the binary a build just replaced. The child is
    // started as this process's own child, which carries the level this process
    // already stands at, so the machine is not asked again for permission while
    // the answer to the ask is still being written.
    bool softReloadRequested();
    void handOverNow();
  } // namespace deploy
} // namespace ols

#endif // OLS_DEPLOYMENT_H

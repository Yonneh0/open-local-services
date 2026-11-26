// Project service module: the ledger of the project studio. The projects that
// stand under <dataDir>/projects, the index that drives the dropdown, the facts
// each project states, and the change mark every project carries all live here.
// The git underneath them does not: a project's repo/ folder stays empty at
// this phase, and GitCore - objects, refs, diffs, and the smart-http doors of
// git - arrives in the later phases and joins these same folders with nothing
// here moved.
#ifndef OLS_PROJECT_PROJECTSERVICE_H
#define OLS_PROJECT_PROJECTSERVICE_H

#include <string>

namespace ols
{
  namespace project
  {

    class ProjectService
    {
    public:
      // Projects live under <dataDir>/projects; the folder and the index come
      // with the first write that needs them, never with the start.
      ProjectService(const std::string &dataDir);

      // The whole service is one machine: the registry, the facts, and the last
      // words stand in module-wide state, so a second object would be a second
      // handle to the one ledger already stood.
      ProjectService(const ProjectService &) = delete;
      ProjectService &operator=(const ProjectService &) = delete;
      ProjectService(ProjectService &&) = delete;
      ProjectService &operator=(ProjectService &&) = delete;

      // The ledger reads itself at the start; a door calls this to be sure the
      // reading happened before it answers. A missing index is an empty
      // registry, said honestly. An index the machine cannot read is said so
      // too, and the projects that stand under the studio are taken back from
      // their folders - the index is only a mirror of the facts, so the pages
      // are never painted over an empty list while real projects stand.
      bool initIfNeeded();

      // The dropdown document: every project with its rev, plus the counts of
      // open issues and open pulls when those sidecars are readable; when the
      // machine cannot read them the fields are absent rather than guessed.
      std::string listJson() const;

      // The proposal a name gets before it stands as a folder name: lowercase,
      // spaces to dashes, anything a slug may not wear stripped out, and a
      // leading or trailing dot or dash dropped - a folder would be made under
      // the shortened name, so the studio writes down what it stands. The
      // dialog previews with this; the create door refuses when what remains
      // is not a usable, fresh slug.
      std::string slugFor(const std::string &name) const;

      // Make a project stand: the name is offered, the slug is its proposal,
      // and the options name the default branch, a readme the studio would
      // write, and a license. The repo/ folder stays empty this phase - the
      // first commit and its files are GitCore's doors. A refused call states
      // its reason in lastError. The slug the project stands at comes back in
      // madeSlug: it is the module's naming, and the door answers from it -
      // not from a diff of the index, which another connection's create could
      // have moved under it. madeSlug is empty when nothing was made.
      bool create(const std::string &name, const std::string &optsJson, std::string &madeSlug);

      // Register a folder that already stands: a project carried in from
      // another machine or another place on this one. Nothing is copied; the
      // facts travel with the folder. The slug it was registered under comes
      // back in madeSlug, for the same reason create states it.
      bool adoptFolder(const std::string &path, std::string &madeSlug);

      // Stand a project up from a git link: the address is asked for its
      // refs, the studio's folder is made at the name's proposal with a bare
      // repo cloned from it, and the row says so with a source of
      // {kind:"git", url}. The road speaks plain http: an https address is
      // refused in words that name TLS, the layer this tree does not carry.
      // A clone that does not land leaves no project and no half-made
      // folder. This service holds no api key of its own - the process
      // keeps it in the doors - so the door that holds one hands it here;
      // empty travels no key. The slug the project stands at comes back in
      // madeSlug, for the same reason adoptFolder states it.
      bool adoptUrl(const std::string &url, const std::string &nameHint, std::string &madeSlug,
                    const std::string &apiKey = std::string());

      // The display name of a project, changed. The slug and the folder keep
      // their names; a slug is an address and does not move.
      bool rename(const std::string &slug, const std::string &newName);

      // Let a project go: the name has to be said again to let it, the tree
      // is taken away, and the slug may never be used again. A tree another
      // process holds locked comes away not at all - the project stays where
      // it stands, with its index row, and the call is refused. A tree that
      // would come away only partly, or an index that would not take the
      // word, still leaves the project out of the ledger: the call answers
      // true and states what remains, and what stands on disk, in
      // residueWords - including the folder a carried project was taken on
      // from, which was never the studio's to take away.
      bool remove(const std::string &slug, const std::string &confirmName,
                  std::string &residueWords);

      // One project's facts, as the machine holds them; empty when the
      // machine holds no such project.
      std::string metaJson(const std::string &slug) const;

      // The facts a page is allowed to edit - name, description, purpose,
      // license, planned features - and nothing else. The rev moves with the
      // edit.
      bool editMeta(const std::string &slug, const std::string &patchJson);

      // The change mark of one project, moved on. Every mutation calls it.
      bool bumpRev(const std::string &slug);

      // The directory of one project's bare repo, or the empty string when no
      // such project stands in the index. The code doors read through it; they
      // never build a path from a name the index does not carry.
      std::string repoDir(const std::string &slug) const;

      // {slug: rev, ...} for every project: the whole change detector the
      // pages compare against.
      std::string projectRevsJson() const;

      // The toolchain a file's extension wears, read from the machine and
      // cached once per process: when a tool answers for the extension, words
      // carries its name ("msvc-cl", "python", "node", "jdk"); when none does,
      // the refusal words name the extension and what the machine could not
      // find. Nothing here is assumed - the facts were probed.
      bool toolchainFor(const std::string &extension, std::string &words) const;

      // Make a build stand: the file is one of the project's, named by slug
      // and by its path inside the studio's tree for it, and nothing is copied
      // - the toolchain reads it where it stands and every artifact lands in
      // a short job root under the studio's TEMP, beside the records the
      // status door reads. The job's name comes out in jobIdOut; a second
      // build asked while one stands for the same project is refused with
      // words, and job roots older than a day are pruned on the next ask.
      bool startBuild(const std::string &slug, const std::string &relPath,
                      std::string &jobIdOut, std::string &words);

      // One build job as far as it has got: the state it wears, what it has
      // printed (a tail while it stands, the whole capture once it ended,
      // both capped), the exit code and duration when it ended, and the
      // compiler's own lines read as {file,line,col,message} rows with their
      // paths put back to project-relative where they name the built file.
      bool buildStatus(const std::string &jobId, std::string &answerJson,
                       std::string &words) const;

      // Ask a standing build to go: the process the job recorded is ended
      // tree-wide and the job answers as cancelled; a job that already ended
      // is refused with words.
      bool stopBuild(const std::string &jobId, std::string &words);

      // Why the last call was refused, in this service's own words; empty
      // when nothing was refused.
      std::string lastError() const;
    };

  } // namespace project
} // namespace ols

#endif // OLS_PROJECT_PROJECTSERVICE_H

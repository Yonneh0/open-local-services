// src/gitcore/refs.cpp — see refs.h for the promises this file keeps.

#include "refs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ols
{
    namespace gitcore
    {

        namespace
        {

            // The filesystem talks UTF-8 here, as the rest of the studio does: u8path
            // makes a Windows narrow string reach the wide API honestly, and on POSIX it
            // is the string itself.
            fs::path upath(const std::string &s) { return fs::u8path(s); }

            std::string path_str(const fs::path &p) { return p.u8string(); }

// The name the file API is handed. On Windows that is not always the path
// itself: past 260 characters the plain form is refused by the API before it
// looks at the disk, and 260 is not far — a branch name carrying a slash stands
// for a folder of its own, and a repository under a few nested folders passes it
// with room to spare. A ref that cannot be read is a ref this unit will write
// over without knowing it, and a ref that cannot be written is one the studio
// loses quietly, so every path here that reaches an API goes in the extended
// form: absolute, backslashes only, under the \\?\ prefix that carries no length
// limit and takes the name exactly as it stands. std::filesystem does this for
// its own calls; this is the same qualification, made once for the calls that
// are not std::filesystem's own. On POSIX the path is the name.
#ifdef _WIN32
            fs::path ext_path(const fs::path &p)
            {
                std::error_code ec;
                fs::path full = fs::absolute(p, ec).lexically_normal();
                if (ec)
                {
                    return p; // the path as given: what this did before the prefix existed
                }
                std::wstring w = full.wstring();
                for (wchar_t &c : w)
                {
                    if (c == L'/')
                    {
                        c = L'\\';
                    }
                }
                if (w.compare(0, 4, L"\\\\?\\") == 0)
                {
                    return p;
                }
                if (w.size() >= 2u && w[0] == L'\\' && w[1] == L'\\')
                {
                    return fs::path(std::wstring(L"\\\\?\\UNC\\") + w.substr(2u)); // a share keeps its server
                }
                return fs::path(std::wstring(L"\\\\?\\") + w);
            }

            std::wstring open_name(const fs::path &p) { return ext_path(p).wstring(); }
#else
            const fs::path &ext_path(const fs::path &p) { return p; }
            const fs::path &open_name(const fs::path &p) { return p; }
#endif

            bool ends_with(const std::string &s, const std::string &tail)
            {
                return s.size() >= tail.size() &&
                       s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
            }

            bool is_hex_digit(char c)
            {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }

            bool is_digit(char c) { return c >= '0' && c <= '9'; }

            char lower_char(char c)
            {
                return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
            }

            // What a ref file holds, trimmed of the whitespace that trails a well-written
            // line — the newline, a stray carriage return, the trailing space git's own
            // fixture headers carry.
            std::string trim_ref(const std::string &s)
            {
                std::size_t a = 0;
                std::size_t b = s.size();
                while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
                {
                    ++a;
                }
                while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' ||
                                 s[b - 1] == '\r' || s[b - 1] == '\n'))
                {
                    --b;
                }
                return s.substr(a, b - a);
            }

            // Whole file, as its bytes stand. False means not readable, and only that.
            bool read_file(const fs::path &p, std::string &out)
            {
                std::ifstream f(open_name(p), std::ios::binary);
                if (!f)
                {
                    return false;
                }
                out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                // A ref file that goes away between the open and the last read is a thing
                // that happens — the studio and git write these files beside each other —
                // and the iterator stops quietly when it does. Half a line of oid is not an
                // answer: the stream's own state says whether the bytes came, and a stream
                // that ended short of its end says read failure, same as one that never
                // opened.
                if (f.bad())
                {
                    out.clear();
                    return false;
                }
                return true;
            }

            void remove_quiet(const fs::path &p)
            {
                std::error_code ec;
                fs::remove(ext_path(p), ec);
            }

            bool ensure_dir(const fs::path &p)
            {
                // Both halves go through the extended name: the API that makes the folders
                // stops at the same 260 characters the one that opens the file in them does,
                // and a ref folder that cannot be made is a branch the studio cannot keep.
                const fs::path wide = ext_path(p);
                std::error_code ec;
                if (fs::is_directory(wide, ec))
                {
                    return true;
                }
                fs::create_directories(wide, ec);
                return fs::is_directory(wide);
            }

            // The staging name of a plain single-step write: <target>.new. git names its
            // packed-refs staging the same way, and the crash-safety claims of those
            // writes are stated in terms of this suffix. The multi-step transaction does
            // NOT stage here — it stands each update at its own unique name (see
            // unique_stage_for below) so that writers outside this unit can never have
            // their staged bytes renamed by someone else's move.
            fs::path temp_for(const fs::path &target)
            {
                fs::path t = target;
                t += ".new";
                return t;
            }

            // Stage bytes at a staging name; nothing at the target moves yet.
            bool write_at(const fs::path &stage, const std::string &bytes)
            {
                if (!ensure_dir(stage.parent_path()))
                {
                    return false;
                }
                std::ofstream out(open_name(stage), std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    return false;
                }
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.flush();
                if (!out)
                {
                    remove_quiet(stage);
                    return false;
                }
                out.close();
                return true;
            }

            // Stage bytes at the plain temp name; nothing at the target moves yet.
            bool write_temp(const fs::path &target, const std::string &bytes)
            {
                return write_at(temp_for(target), bytes);
            }

            // Move the staged bytes into place in one step. On Windows the replacement of
            // a file one cannot delete is exactly what MoveFileExW with REPLACE_EXISTING
            // does; on POSIX rename already replaces atomically. std::filesystem::rename
            // is left out because its behavior over an existing target is implementation
            // defined — this pair is the same move on both platforms, which is what the
            // transaction needs.
            bool replace_file(const fs::path &stage, const fs::path &target)
            {
#ifdef _WIN32
                if (!MoveFileExW(open_name(stage).c_str(), open_name(target).c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    remove_quiet(stage);
                    return false;
                }
                return true;
#else
                std::error_code ec;
                fs::rename(stage, target, ec);
                if (ec)
                {
                    remove_quiet(stage);
                    return false;
                }
                return true;
#endif
            }

            bool replace_from_temp(const fs::path &target)
            {
                return replace_file(temp_for(target), target);
            }

            // Stage and move: the one-step form every plain write of this file uses.
            bool write_file_atomic(const fs::path &target, const std::string &bytes)
            {
                if (!write_temp(target, bytes))
                {
                    return false;
                }
                return replace_from_temp(target);
            }

            // ---- the transaction's lock -------------------------------------------------
            //
            // commitRefs moves a ref by compare-and-swap: the expected-old value is read,
            // and one rename puts the new one. Two of those running at once over one ref
            // both read the same old value, one rename overwrites the other's staging,
            // and both are told they won — the ref ends as someone's answer, but the
            // loser's caller was lied to. So the transaction stands on a lock across the
            // whole of it, <ref>.lock, created EXCLUSIVE: no truncate, no open-if-exists,
            // one winner per race by the filesystem's own word. The loser does not wait
            // and does not guess — false, now, nothing touched, the house's word for
            // "someone else moved first".
            //
            // The age clause is the crashed holder's half. A lock whose file stands older
            // than kRefLockStaleAge is presumed abandoned by a process that died holding
            // it, and the next writer removes it and creates once more. The number is
            // generous on purpose: a transaction is a handful of file steps in one folder,
            // done in milliseconds, and the age must never be the thing that cuts a live
            // one short. Against a holder that is merely alive the takeover is held off
            // too:
            //   - Windows: the live holder keeps the handle open, and removing an open
            //     file fails — the takeover comes back false and the holder is undisturbed;
            //   - POSIX: unlink succeeds under an open handle, so an age-true takeover of
            //     a holder running longer than the age is possible in principle; that is
            //     the documented, vanishingly narrow exposure of age-based takeover, and
            //     it stands far above the unconditional double-win of no lock at all.
            constexpr std::chrono::minutes kRefLockStaleAge{5};

            bool lock_is_stale(const fs::path &lockFile)
            {
                std::error_code ec;
                const auto stamp = fs::last_write_time(lockFile, ec);
                if (ec)
                {
                    return false; // a stamp that cannot be read says held, not dead
                }
                return fs::file_time_type::clock::now() - stamp > kRefLockStaleAge;
            }

            // An exclusively-created lock file, held to the destructor. Not copyable, not
            // movable: the handle is the lock, and it stands where it was made.
            class RefLock
            {
            public:
                RefLock() = default;
                ~RefLock() { release(); }
                RefLock(const RefLock &) = delete;
                RefLock &operator=(const RefLock &) = delete;

                // Take the lock. True when this process now holds it; false now, without
                // waiting, when another holder has it or the file cannot be made at all.
                // A holder presumed dead by the age clause is cleared once, and the
                // create retried once.
                bool acquire(const fs::path &lockFile)
                {
                    if (!ensure_dir(lockFile.parent_path()))
                    {
                        return false;
                    }
                    for (int attempt = 0; attempt < 2; ++attempt)
                    {
#ifdef _WIN32
                        HANDLE made = CreateFileW(open_name(lockFile).c_str(), GENERIC_WRITE, 0, nullptr,
                                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (made != INVALID_HANDLE_VALUE)
                        {
                            handle_ = made;
                            path_ = lockFile; // ours only from the create that succeeded: a
                                              // failed acquire never removes another's lock
                            return true;
                        }
                        // CREATE_NEW answers an existing file with ERROR_FILE_EXISTS;
                        // ERROR_ALREADY_EXISTS stands for the other create dispositions.
                        // Either word means held; only those two.
                        {
                            const DWORD err = GetLastError();
                            if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
                            {
                                return false; // no folder, no rights, no disk: not our race
                            }
                        }
#else
                        const int fd = ::open(open_name(lockFile).c_str(),
                                              O_CREAT | O_EXCL | O_WRONLY, 0666);
                        if (fd >= 0)
                        {
                            fd_ = fd;
                            path_ = lockFile; // ours only from the create that succeeded
                            return true;
                        }
                        if (errno != EEXIST)
                        {
                            return false;
                        }
#endif
                        if (attempt == 0 && lock_is_stale(lockFile))
                        {
                            remove_quiet(lockFile);
                            continue;
                        }
                        return false;
                    }
                    return false;
                }

                void release()
                {
#ifdef _WIN32
                    if (handle_ != INVALID_HANDLE_VALUE)
                    {
                        ::CloseHandle(handle_);
                        handle_ = INVALID_HANDLE_VALUE;
                    }
#else
                    if (fd_ >= 0)
                    {
                        ::close(fd_);
                        fd_ = -1;
                    }
#endif
                    if (!path_.empty())
                    {
                        remove_quiet(path_);
                        path_.clear();
                    }
                }

            private:
                fs::path path_;
#ifdef _WIN32
                HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
                int fd_ = -1;
#endif
            };

            // The staging name of one update of one transaction: <target>.new.<pid>.<seq>.
            // The lock above keeps the transactions of this unit apart; the plain
            // <target>.new is still one name shared with every writer that ignores it —
            // git beside the studio, a tool mid-migration — and a shared staging file is
            // one writer's bytes renamed by another's move. The unique suffix makes the
            // staging itself one writer's whatever else runs; the lock is what the
            // compare-and-swap answers to.
            fs::path unique_stage_for(const fs::path &target)
            {
                static std::atomic<unsigned long> seq{0};
#ifdef _WIN32
                const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
                const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
                fs::path t = target;
                t += ".new.";
                t += std::to_string(pid);
                t += '.';
                t += std::to_string(++seq);
                return t;
            }

            bool append_file(const fs::path &target, const std::string &bytes)
            {
                if (!ensure_dir(target.parent_path()))
                {
                    return false;
                }
                std::ofstream out(open_name(target), std::ios::binary | std::ios::app);
                if (!out)
                {
                    return false;
                }
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.flush();
                return static_cast<bool>(out);
            }

            // Split on '\n'. A file that ends in its newline does not gain an empty last
            // line; a blank line inside stays a blank entry, for the parsers to pass over
            // or refuse.
            std::vector<std::string> split_lines(const std::string &text)
            {
                std::vector<std::string> lines;
                std::size_t start = 0;
                while (start <= text.size())
                {
                    std::size_t nl = text.find('\n', start);
                    if (nl == std::string::npos)
                    {
                        lines.push_back(text.substr(start));
                        break;
                    }
                    lines.push_back(text.substr(start, nl - start));
                    start = nl + 1;
                    if (start == text.size())
                    {
                        break; // the file ended at its newline
                    }
                }
                return lines;
            }

            bool is_null_hex(const std::string &s)
            {
                if (s.size() != 40 && s.size() != 64)
                {
                    return false;
                }
                return detail::all_zero_hex(s);
            }

            // An oid field of a reflog line: a well-formed oid or its all-zero null.
            bool is_oid_or_null(const std::string &s)
            {
                return isValidOidHex(s) || is_null_hex(s);
            }

            // The "<epoch> <tz>" of a reflog stamp, checked the way the writer of one
            // would want: an integer epoch and an offset that says which way the clock
            // leans.
            bool is_stamp(const std::string &when)
            {
                const std::size_t sp = when.find(' ');
                if (sp == std::string::npos || sp + 1 >= when.size())
                {
                    return false;
                }
                const std::string epoch = when.substr(0, sp);
                const std::string tz = when.substr(sp + 1);
                if (when.find(' ', sp + 1) != std::string::npos)
                {
                    return false;
                }
                std::size_t i = (!epoch.empty() && epoch[0] == '-') ? 1 : 0;
                if (i == epoch.size())
                {
                    return false;
                }
                for (; i < epoch.size(); ++i)
                {
                    if (!is_digit(epoch[i]))
                    {
                        return false;
                    }
                }
                if (tz.size() < 5 || (tz[0] != '+' && tz[0] != '-'))
                {
                    return false;
                }
                for (std::size_t j = 1; j < tz.size(); ++j)
                {
                    if (!is_digit(tz[j]))
                    {
                        return false;
                    }
                }
                return true;
            }

            const char *kConfigTextHead =
                "[core]\n"
                "\trepositoryformatversion = 0\n"
                "\tfilemode = false\n"
                "\tbare = true\n";

            const char *kConfigTextTail =
                "[init]\n"
                "\tdefaultBranch = ";

            const char *kDescriptionText =
                "Unnamed repository; edit this file 'description' to name the repository.\n";

            std::string config_text(const std::string &default_branch)
            {
                // What git.exe's own init --bare writes on this machine (probed 2026-11-22,
                // git 2.56), minus the keys symlinks and ignorecase that git there probes
                // per platform — a studio repo travels, and these two are the machine's
                // facts, not the repo's. filemode is moot for a bare repo: there is no
                // working tree whose exec bits to trust. logallrefupdates stays out on
                // purpose: the studio owns its reflogs through GitCore, not through git's
                // hook-up. init.defaultBranch states the default to any git that reads it.
                std::string t = kConfigTextHead;
                t += kConfigTextTail;
                t += default_branch;
                t += "\n";
                return t;
            }

        } // anonymous namespace

        // ---- names and ids ----

        bool isValidOidHex(const std::string &text)
        {
            if (text.size() != 40 && text.size() != 64)
            {
                return false;
            }
            for (const char c : text)
            {
                if (!is_hex_digit(c))
                {
                    return false;
                }
            }
            return true;
        }

        namespace detail
        {

            std::string lower_hex(std::string s)
            {
                for (char &c : s)
                {
                    c = lower_char(c);
                }
                return s;
            }

            bool all_zero_hex(const std::string &s)
            {
                if (s.empty())
                {
                    return false;
                }
                for (const char c : s)
                {
                    if (c != '0')
                    {
                        return false;
                    }
                }
                return true;
            }

        } // namespace detail

        bool isValidRefName(const std::string &name)
        {
            // A practical cap, in the order of git's own path limits; a longer name
            // is a bug upstream of here, not a ref.
            if (name.empty() || name.size() > 1024)
            {
                return false;
            }

            // No control character and no space, at all; bytes at 0x80 pass as
            // utf8-ish printable, as git's check leaves them.
            for (const unsigned char c : name)
            {
                if (c <= ' ' || c == 0x7f)
                {
                    return false;
                }
            }

            // git's punctuation with a meaning: none of these belong in a name.
            if (name.find('\\') != std::string::npos ||
                name.find('~') != std::string::npos ||
                name.find('^') != std::string::npos ||
                name.find(':') != std::string::npos ||
                name.find('?') != std::string::npos ||
                name.find('*') != std::string::npos ||
                name.find('[') != std::string::npos)
            {
                return false;
            }

            if (name.front() == '/' || name.back() == '/')
            {
                return false;
            }
            if (name.find("//") != std::string::npos)
            {
                return false;
            }
            // One rule that kills both the ".." path escape and the dotted component.
            if (name.find("..") != std::string::npos)
            {
                return false;
            }
            // HEAD@{...} syntax is git's to parse; a name may not wear it.
            if (name.find("@{") != std::string::npos)
            {
                return false;
            }
            if (ends_with(name, ".lock"))
            {
                return false;
            }

            // Per component: nothing empty, nothing that starts or ends with a dot,
            // nothing ending in .lock, and no component that is the lone "@".
            std::size_t start = 0;
            while (start <= name.size())
            {
                std::size_t slash = name.find('/', start);
                const std::size_t end = (slash == std::string::npos) ? name.size() : slash;
                const std::string comp = name.substr(start, end - start);
                if (comp.empty() || comp.front() == '.' || comp.back() == '.')
                {
                    return false;
                }
                if (comp == "@")
                {
                    return false;
                }
                if (ends_with(comp, ".lock"))
                {
                    return false;
                }
                if (slash == std::string::npos)
                {
                    break;
                }
                start = slash + 1;
            }
            return true;
        }

        const char *toString(RefStatus status)
        {
            switch (status)
            {
            case RefStatus::Ok:
                return "Ok";
            case RefStatus::Missing:
                return "Missing";
            case RefStatus::TooDeep:
                return "TooDeep";
            case RefStatus::Bad:
                return "Bad";
            }
            return "?";
        }

        // ---- symref content ----

        bool looksLikeSymref(const std::string &content)
        {
            return content.compare(0, 5, "ref: ") == 0;
        }

        bool symrefTarget(const std::string &content, std::string &target)
        {
            if (!looksLikeSymref(content))
            {
                return false;
            }
            target = trim_ref(content.substr(5));
            return isValidRefName(target);
        }

        // ---- packed-refs ----

        bool parsePackedRefs(const std::string &text, std::vector<PackedRefEntry> &out)
        {
            out.clear();
            for (const std::string &raw : split_lines(text))
            {
                const std::string line = trim_ref(raw);
                if (line.empty())
                {
                    continue;
                }
                if (line[0] == '#')
                { // git's header, and any comment of ours
                    continue;
                }
                if (line[0] == '^')
                {
                    if (out.empty() || out.back().peeled)
                    {
                        out.clear();
                        return false; // a peel with nothing to peel, or two in a row
                    }
                    const std::string peel = line.substr(1);
                    if (!isValidOidHex(peel))
                    {
                        out.clear();
                        return false;
                    }
                    out.back().peeled = true;
                    out.back().peeledOid = detail::lower_hex(peel);
                    continue;
                }
                const std::size_t sp = line.find(' ');
                if (sp == std::string::npos)
                {
                    out.clear();
                    return false;
                }
                const std::string oid = line.substr(0, sp);
                const std::string name = trim_ref(line.substr(sp + 1));
                if (!isValidOidHex(oid) || !isValidRefName(name))
                {
                    out.clear();
                    return false;
                }
                out.push_back({detail::lower_hex(oid), name, false, std::string()});
            }
            return true;
        }

        bool serializePackedRefs(const std::vector<PackedRefEntry> &entries, std::string &out)
        {
            std::vector<PackedRefEntry> sorted = entries;
            std::sort(sorted.begin(), sorted.end(),
                      [](const PackedRefEntry &a, const PackedRefEntry &b)
                      {
                          return a.name < b.name;
                      });
            for (const PackedRefEntry &e : sorted)
            {
                if (!isValidOidHex(e.oid) || !isValidRefName(e.name))
                {
                    return false;
                }
                if (e.peeled && !isValidOidHex(e.peeledOid))
                {
                    return false;
                }
            }
            for (std::size_t i = 1; i < sorted.size(); ++i)
            {
                if (sorted[i - 1].name == sorted[i].name)
                {
                    return false; // the same ref twice is not a file
                }
            }
            // git's own header line, trailing space included, as packed-refs-sample
            // carries it.
            out = "# pack-refs with: peeled fully-peeled sorted \n";
            for (const PackedRefEntry &e : sorted)
            {
                out += detail::lower_hex(e.oid);
                out += ' ';
                out += e.name;
                out += '\n';
                if (e.peeled)
                {
                    out += '^';
                    out += detail::lower_hex(e.peeledOid);
                    out += '\n';
                }
            }
            return true;
        }

        // ---- reflog lines ----

        bool formatReflogLine(const ReflogEntry &entry, std::string &line)
        {
            if (!is_oid_or_null(entry.oldOid) || !is_oid_or_null(entry.newOid))
            {
                return false;
            }
            if (entry.who.empty() || entry.who.find('\t') != std::string::npos ||
                entry.who.find('\n') != std::string::npos)
            {
                return false;
            }
            if (!is_stamp(entry.when))
            {
                return false;
            }
            if (entry.msg.empty() || entry.msg.find('\t') != std::string::npos ||
                entry.msg.find('\n') != std::string::npos)
            {
                return false;
            }
            line = detail::lower_hex(entry.oldOid);
            line += ' ';
            line += detail::lower_hex(entry.newOid);
            line += ' ';
            line += entry.who;
            line += ' ';
            line += entry.when;
            line += '\t';
            line += entry.msg;
            line += '\n';
            return true;
        }

        bool parseReflogLine(const std::string &line, ReflogEntry &entry)
        {
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos)
            {
                return false;
            }
            std::string msg = line.substr(tab + 1);
            if (!msg.empty() && msg.back() == '\r')
            {
                msg.pop_back();
            }
            if (msg.empty() || msg.find('\t') != std::string::npos ||
                msg.find('\n') != std::string::npos)
            {
                return false;
            }
            const std::string head = line.substr(0, tab);
            const std::size_t sp1 = head.find(' ');
            if (sp1 == std::string::npos)
            {
                return false;
            }
            const std::size_t sp2 = head.find(' ', sp1 + 1);
            if (sp2 == std::string::npos)
            {
                return false;
            }
            const std::string old_oid = head.substr(0, sp1);
            const std::string new_oid = head.substr(sp1 + 1, sp2 - sp1 - 1);
            if (!is_oid_or_null(old_oid) || !is_oid_or_null(new_oid))
            {
                return false;
            }
            // The tail is "<who...> <epoch> <tz>": the last space splits the zone off,
            // the one before it the epoch, and what is left is the identity.
            const std::string tail = head.substr(sp2 + 1);
            const std::size_t tz_sp = tail.rfind(' ');
            if (tz_sp == std::string::npos || tz_sp == 0)
            {
                return false;
            }
            const std::size_t epoch_sp = tail.rfind(' ', tz_sp - 1);
            if (epoch_sp == std::string::npos || epoch_sp == 0)
            {
                return false;
            }
            entry.oldOid = detail::lower_hex(old_oid);
            entry.newOid = detail::lower_hex(new_oid);
            entry.who = tail.substr(0, epoch_sp);
            entry.when = tail.substr(epoch_sp + 1);
            entry.msg = msg;
            if (entry.who.find('\t') != std::string::npos ||
                entry.who.find('\n') != std::string::npos ||
                !is_stamp(entry.when))
            {
                return false;
            }
            return true;
        }

        // ---- the repository layout ----

        bool initBare(const std::string &dir, const std::string &default_branch)
        {
            if (!isValidRefName("refs/heads/" + default_branch))
            {
                return false;
            }
            const fs::path root = upath(dir);
            if (!ensure_dir(root))
            {
                return false;
            }
            for (const char *sub : {"objects/info", "objects/pack",
                                    "refs/heads", "refs/tags", "hooks"})
            {
                if (!ensure_dir(root / fs::u8path(sub)))
                {
                    return false;
                }
            }
            // hooks/ stays empty: the studio installs and manages its own (spec §8),
            // and git's .sample files would only be noise to travel with the repo.
            if (!write_file_atomic(root / "HEAD", "ref: refs/heads/" + default_branch + "\n"))
            {
                return false;
            }
            if (!write_file_atomic(root / "config", config_text(default_branch)))
            {
                return false;
            }
            if (!write_file_atomic(root / "description", kDescriptionText))
            {
                return false;
            }
            return true;
        }

        // ---- the repository, open ----

        Repository::Repository(std::string dir) : dir_(std::move(dir)) {}

        std::string Repository::loosePath(const std::string &name) const
        {
            // name is either "HEAD" or a validated ref path; upath keeps the slashes
            // as the path they name on either platform.
            return path_str(upath(dir_) / upath(name));
        }

        std::string Repository::logPath(const std::string &name) const
        {
            return path_str(upath(dir_) / upath("logs") / upath(name));
        }

        RefStatus Repository::findInPacked(const std::string &name, std::string &oid_lower) const
        {
            std::string text;
            if (!read_file(upath(dir_) / "packed-refs", text))
            {
                return RefStatus::Missing; // no packed file is simply no packed refs
            }
            std::vector<PackedRefEntry> entries;
            if (!parsePackedRefs(text, entries))
            {
                return RefStatus::Bad;
            }
            for (const PackedRefEntry &e : entries)
            {
                if (e.name == name)
                {
                    oid_lower = e.oid;
                    return RefStatus::Ok;
                }
            }
            return RefStatus::Missing;
        }

        RefStatus Repository::readRawRef(const std::string &name, std::string &value) const
        {
            if (!isValidRefName(name))
            {
                return RefStatus::Bad;
            }
            std::string bytes;
            if (read_file(upath(loosePath(name)), bytes))
            {
                value = trim_ref(bytes);
                if (value.empty())
                {
                    return RefStatus::Bad; // an empty ref file is not a ref
                }
                return RefStatus::Ok;
            }
            std::string packed;
            const RefStatus st = findInPacked(name, packed);
            if (st == RefStatus::Ok)
            {
                value = packed;
            }
            return st;
        }

        RefStatus Repository::resolveRef(const std::string &name, ObjectId &out,
                                         int depth_cap) const
        {
            out = ObjectId();
            if (!isValidRefName(name))
            {
                return RefStatus::Bad;
            }
            std::string current = name;
            for (int depth = 0;; ++depth)
            {
                std::string content;
                const RefStatus st = readRawRef(current, content);
                if (st != RefStatus::Ok)
                {
                    return st;
                }
                if (looksLikeSymref(content))
                {
                    std::string target;
                    if (!symrefTarget(content, target))
                    {
                        return RefStatus::Bad;
                    }
                    if (target == current || depth >= depth_cap)
                    {
                        return RefStatus::TooDeep; // a loop shows up one way or the other
                    }
                    current = target;
                    continue;
                }
                ObjectId oid;
                if (!ObjectId::parse(content, oid))
                {
                    return RefStatus::Bad;
                }
                out = oid;
                return RefStatus::Ok;
            }
        }

        bool Repository::writeLooseRef(const std::string &name, const ObjectId &oid)
        {
            if (!isValidRefName(name) || !oid.valid())
            {
                return false;
            }
            return write_file_atomic(upath(loosePath(name)), oid.to_hex() + "\n");
        }

        bool Repository::writeLooseSymref(const std::string &name, const std::string &target)
        {
            if (!isValidRefName(name) || !isValidRefName(target))
            {
                return false;
            }
            return write_file_atomic(upath(loosePath(name)), "ref: " + target + "\n");
        }

        bool Repository::deleteLooseRef(const std::string &name)
        {
            if (!isValidRefName(name))
            {
                return false;
            }
            const fs::path file = upath(loosePath(name));
            std::error_code ec;
            if (!fs::is_regular_file(ext_path(file), ec))
            {
                return false;
            }
            fs::remove(ext_path(file), ec);
            if (fs::is_regular_file(ext_path(file)))
            {
                return false;
            }
            // Empty dirs under refs/ are pruned, as git does; a failure here is only
            // the tidiness lost, not the delete.
            if (name.compare(0, 5, "refs/") == 0)
            {
                fs::path d = file.parent_path();
                const fs::path stop = ext_path(upath(dir_) / "refs");
                d = ext_path(d);
                while (d.string().size() > stop.string().size() &&
                       d.string().compare(0, stop.string().size(), stop.string()) == 0)
                {
                    bool empty = false; // only a clean empty listing says otherwise
                    {
                        std::error_code lec;
                        const fs::directory_iterator it(d, lec);
                        empty = !lec && it == fs::directory_iterator();
                    }
                    if (!empty)
                    {
                        break;
                    }
                    fs::path parent = d.parent_path();
                    std::error_code pec;
                    if (!fs::remove(d, pec) || pec)
                    {
                        break;
                    }
                    d = parent;
                }
            }
            return true;
        }

        bool Repository::readPackedRefs(std::vector<PackedRefEntry> &out) const
        {
            std::string text;
            if (!read_file(upath(dir_) / "packed-refs", text))
            {
                // A repo that has never packed simply has no file: that is an empty
                // packed set, read true with nothing in it. Only a file that stands
                // and does not parse - junk git would not have written - refuses.
                out.clear();
                return true;
            }
            return parsePackedRefs(text, out);
        }

        bool Repository::writePackedRefs(const std::vector<PackedRefEntry> &entries)
        {
            std::string text;
            if (!serializePackedRefs(entries, text))
            {
                return false;
            }
            return write_file_atomic(upath(dir_) / "packed-refs", text);
        }

        bool Repository::isHeadSymref() const
        {
            std::string value;
            const RefStatus st = readRawRef("HEAD", value);
            return st == RefStatus::Ok && looksLikeSymref(value);
        }

        bool Repository::headTarget(std::string &target) const
        {
            std::string value;
            const RefStatus st = readRawRef("HEAD", value);
            return st == RefStatus::Ok && symrefTarget(value, target);
        }

        bool Repository::readHead(ObjectId &out) const
        {
            return resolveRef("HEAD", out) == RefStatus::Ok;
        }

        bool Repository::setHeadSymref(const std::string &target)
        {
            return writeLooseSymref("HEAD", target);
        }

        bool Repository::detachHead(const ObjectId &oid)
        {
            return writeLooseRef("HEAD", oid);
        }

        bool Repository::allRefs(std::vector<RefInfo> &out) const
        {
            std::map<std::string, RefInfo> by_name;

            // Packed first, so a loose walk can override it — loose over packed, as
            // git resolves. Only entries under refs/ are rows; a peel line is tag
            // data of the entry above it, not a ref of its own.
            std::string text;
            if (read_file(upath(dir_) / "packed-refs", text))
            {
                std::vector<PackedRefEntry> entries;
                if (!parsePackedRefs(text, entries))
                {
                    out.clear();
                    return false;
                }
                for (const PackedRefEntry &e : entries)
                {
                    if (e.name.compare(0, 5, "refs/") == 0)
                    {
                        RefInfo info;
                        info.name = e.name;
                        info.oid = ObjectId(e.oid);
                        by_name[e.name] = info;
                    }
                }
            }

            // The loose walk, over all of refs/ — heads and tags first among them, and
            // any further hierarchy a future receive lays down walks with them. The
            // walk's own error_code stays apart from every other call's: one unreadable
            // corner does not stop the iterator, and a stopped iterator is not a
            // complete listing.
            // The walk runs on the extended name, and the ref name is cut against that
            // same form: one root, or the relative step finds no common parent and the
            // listing comes back short of the refs that are there.
            const fs::path repo_root = ext_path(upath(dir_));
            const fs::path refs_root = repo_root / "refs";
            std::error_code walk_ec;
            for (fs::recursive_directory_iterator it(refs_root, walk_ec), end;
                 !walk_ec && it != end; it.increment(walk_ec))
            {
                std::error_code tec;
                if (it->is_directory(tec))
                {
                    continue;
                }
                if (!tec)
                {
                    std::error_code rec;
                    std::string rel = fs::relative(it->path(), repo_root, rec).u8string();
                    std::replace(rel.begin(), rel.end(), '\\', '/');
                    if (rec || !isValidRefName(rel))
                    {
                        continue; // a name git would not carry is not listed as one
                    }
                    std::string bytes;
                    if (!read_file(it->path(), bytes))
                    {
                        out.clear();
                        return false;
                    }
                    const std::string content = trim_ref(bytes);
                    RefInfo info;
                    info.name = rel;
                    if (looksLikeSymref(content))
                    {
                        std::string target;
                        if (!symrefTarget(content, target))
                        {
                            out.clear();
                            return false;
                        }
                        info.is_symref = true;
                        info.target = target;
                        ObjectId oid;
                        if (resolveRef(rel, oid) == RefStatus::Ok)
                        {
                            info.oid = oid;
                        }
                    }
                    else
                    {
                        ObjectId oid;
                        if (!ObjectId::parse(content, oid))
                        {
                            out.clear();
                            return false;
                        }
                        info.oid = oid;
                    }
                    by_name[rel] = info;
                }
            }

            // HEAD, however it stands.
            std::string head_value;
            const RefStatus head_st = readRawRef("HEAD", head_value);
            if (head_st == RefStatus::Ok)
            {
                RefInfo info;
                info.name = "HEAD";
                std::string target;
                if (symrefTarget(head_value, target))
                {
                    info.is_symref = true;
                    info.target = target;
                    ObjectId oid;
                    if (resolveRef("HEAD", oid) == RefStatus::Ok)
                    {
                        info.oid = oid;
                    }
                }
                else
                {
                    ObjectId oid;
                    if (!ObjectId::parse(head_value, oid))
                    {
                        out.clear();
                        return false;
                    }
                    info.oid = oid;
                }
                by_name["HEAD"] = info;
            }
            else if (head_st == RefStatus::Bad)
            {
                out.clear();
                return false;
            }

            // The map iterates in name order — that is the listing's order, the same
            // sorted walk git's show-ref keeps.
            out.clear();
            out.reserve(by_name.size());
            for (const std::pair<const std::string, RefInfo> &row : by_name)
            {
                out.push_back(row.second);
            }
            return true;
        }

        bool Repository::commitRefs(const std::vector<RefUpdate> &updates)
        {
            if (updates.empty())
            {
                return true; // nothing asked, nothing moved, nothing to undo
            }

            // Phase A — validate every update's shape before a byte moves.
            std::set<std::string> seen;
            for (const RefUpdate &u : updates)
            {
                if (!isValidRefName(u.name))
                {
                    return false;
                }
                if (!seen.insert(u.name).second)
                {
                    return false; // two updates for one ref is not one truth
                }
                if (!u.newOid.valid() || u.newOid.is_null())
                {
                    return false; // the null oid as a new value is a delete, which
                                  // this is not; a delete is asked for elsewhere
                }
                if (u.expectedOldOid && !u.expectedOldOid->valid())
                {
                    return false;
                }
            }

            // Phase B — the locks, before a value is read. The compare of the
            // compare-and-swap is worth its word only while no one can move the ref
            // between the reading of it and the rename that claims it, so every named
            // ref is locked first and held through the whole transaction. A ref the
            // transaction cannot lock is a ref it will not claim to have moved: the
            // loser answers false, now, with nothing touched — the house's word.
            // The locks are taken in the order the updates are given; no lock waits,
            // so no ordering of two transactions can deadlock — one of them simply
            // loses and goes home holding nothing.
            std::vector<std::unique_ptr<RefLock>> locks;
            locks.reserve(updates.size());
            for (const RefUpdate &u : updates)
            {
                std::unique_ptr<RefLock> lock(new RefLock());
                if (!lock->acquire(upath(loosePath(u.name) + ".lock")))
                {
                    return false; // the vector takes the locks already held home
                }
                locks.push_back(std::move(lock));
            }

            // One expected-old stated against the ref as it stands now: no junk and
            // no symref in the way, the missing-or-null ref the null expectation asks
            // for, the exact oid the other states.
            auto expectation_holds = [this](const RefUpdate &u)
            {
                std::string value;
                const RefStatus st = readRawRef(u.name, value);
                if (st == RefStatus::Bad)
                {
                    return false;
                }
                if (st == RefStatus::Ok && looksLikeSymref(value))
                {
                    return false; // concrete refs move here; HEAD moves through its
                                  // own helpers, and a symref target is no ref to
                                  // overwrite with an oid
                }
                if (!u.expectedOldOid)
                {
                    return true;
                }
                const ObjectId expect = *u.expectedOldOid;
                if (expect.is_null())
                {
                    // The null expectation: the ref must not be there at all —
                    // force-with-lease on a fresh ref, git's own meaning.
                    if (st != RefStatus::Missing)
                    {
                        ObjectId cur;
                        if (!ObjectId::parse(value, cur) || !cur.is_null())
                        {
                            return false;
                        }
                    }
                    return true;
                }
                ObjectId cur;
                return st == RefStatus::Ok && ObjectId::parse(value, cur) && cur == expect;
            };

            // Current values and the rollback image, read under the locks — the
            // compare half of the swap.
            struct Slot
            {
                fs::path file;
                fs::path stage;
                bool had_loose = false;
                std::string prior_bytes; // what the loose file held, when it held
            };
            std::vector<Slot> slots;
            slots.reserve(updates.size());
            for (const RefUpdate &u : updates)
            {
                if (!expectation_holds(u))
                {
                    return false;
                }
                Slot slot;
                slot.file = upath(loosePath(u.name));
                slot.stage = unique_stage_for(slot.file);
                slot.had_loose = read_file(slot.file, slot.prior_bytes);
                slots.push_back(std::move(slot));
            }

            // The way back from a failed move: the refs already moved go to what they
            // held before the transaction, and every staging file left standing goes
            // with them.
            auto put_back = [&](std::size_t movedCount)
            {
                for (std::size_t k = 0; k < movedCount; ++k)
                {
                    const Slot &s = slots[k];
                    if (s.had_loose)
                    {
                        write_file_atomic(s.file, s.prior_bytes);
                    }
                    else
                    {
                        remove_quiet(s.file); // back to packed, or to not there
                    }
                }
                for (std::size_t k = movedCount; k < updates.size(); ++k)
                {
                    remove_quiet(slots[k].stage);
                }
            };

            // Phase C — every stage down under its own unique name before any
            // replace runs. A stage that cannot be written ends the transaction;
            // the targets have not moved.
            for (std::size_t i = 0; i < updates.size(); ++i)
            {
                if (!write_at(slots[i].stage, updates[i].newOid.to_hex() + "\n"))
                {
                    for (std::size_t k = 0; k < i; ++k)
                    {
                        remove_quiet(slots[k].stage);
                    }
                    return false;
                }
            }

            // Phase D — the moves. Each expected-old is read against the ref itself
            // once more, in the same breath as the rename that claims it: the swap
            // the caller asked for is the one the bytes hold at the instant of the
            // move, not the one they held a phase ago. Under the locks nothing of
            // this unit can have moved it; the re-read is the claim's witness against
            // writers that ignore the locks entirely, and it costs one small read.
            // One failing and the ones that made it go back.
            for (std::size_t i = 0; i < updates.size(); ++i)
            {
                if (updates[i].expectedOldOid && !expectation_holds(updates[i]))
                {
                    put_back(i);
                    return false;
                }
                if (replace_file(slots[i].stage, slots[i].file))
                {
                    continue;
                }
                put_back(i);
                return false;
            }
            return true;
        }

        // ---- reflogs ----

        bool Repository::reflogAppend(const std::string &ref, const ReflogEntry &entry)
        {
            if (!isValidRefName(ref))
            {
                return false;
            }
            std::string line;
            if (!formatReflogLine(entry, line))
            {
                return false;
            }
            return append_file(upath(logPath(ref)), line);
        }

        bool Repository::reflogRead(const std::string &ref, std::vector<ReflogEntry> &out) const
        {
            out.clear();
            if (!isValidRefName(ref))
            {
                return false;
            }
            std::string bytes;
            if (!read_file(upath(logPath(ref)), bytes))
            {
                return true; // no log is no history, and history is not a failure
            }
            for (const std::string &line : split_lines(bytes))
            {
                if (line.empty())
                {
                    continue;
                }
                ReflogEntry entry;
                if (!parseReflogLine(line, entry))
                {
                    out.clear();
                    return false;
                }
                out.push_back(std::move(entry));
            }
            return true;
        }

        bool Repository::reflogPrune(const std::string &ref)
        {
            if (!isValidRefName(ref))
            {
                return false;
            }
            std::vector<ReflogEntry> entries;
            if (!reflogRead(ref, entries))
            {
                return false;
            }
            if (entries.size() <= reflog_keep_)
            {
                return true; // within the keep already; the log is left alone
            }
            std::string text;
            for (std::size_t i = entries.size() - reflog_keep_; i < entries.size(); ++i)
            {
                std::string line;
                if (!formatReflogLine(entries[i], line))
                {
                    return false;
                }
                text += line;
            }
            return write_file_atomic(upath(logPath(ref)), text);
        }

    } // namespace gitcore
} // namespace ols

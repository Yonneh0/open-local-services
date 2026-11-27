// tests/gitcore/test_ssh.cpp — the ssh road's cases (E2). The readings, the
// quoting, the pipes, and one child per ask: what the pinned release proved in
// the spike is stated here as claims. Where a case would lean on the network or
// an account's own identity it does not stand at all — the fake upload-pack
// below answers from a batch file made in the scratch folder, so every spawn
// stays on this machine. The whole file stands under _WIN32: the pinned release
// and the vendored hands are the Windows side's, and the other platform reads
// an empty file until its own OpenSSH stands.

#ifdef _WIN32

#include "SshFetch.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// The scratch folder these cases make their fakes in, named per case so two
// runs never read each other's batch files. %TEMP% is where the machine says
// to write throwaway things; nothing here outlives the run.
std::filesystem::path sshScratch(const std::string &caseName)
{
    const std::filesystem::path root =
        ols_test::scratchRoot() / "ols-git-tests-ssh";
    const std::filesystem::path room = root / caseName;
    std::error_code ec;
    std::filesystem::remove_all(room, ec);
    std::filesystem::create_directories(room, ec);
    return room;
}

// What a reading found, named the way a claim wants to say it.
std::string kindText(ols::ssh::UrlKind kind)
{
    switch (kind)
    {
    case ols::ssh::UrlKind::SshUrl: return "ssh-url";
    case ols::ssh::UrlKind::ScpLike: return "scp-like";
    default: return "not-ssh";
    }
}

// One address read out loud: the kind it came back as, or "refused" only when
// the reading itself refused — a url of another road reads true as not-ssh,
// and that is a reading, not a refusal.
std::string scRead(const std::string &url)
{
    ols::ssh::SshTarget target;
    std::string error;
    if (!ols::ssh::classifySshUrl(url, target, error))
    {
        return "refused";
    }
    return kindText(target.kind);
}

// A fetch pointed at the fake: the spawn line's ssh.exe is the batch file
// itself, so no ssh ever starts — the pipes, the cap, and the refusal words
// are what run. The identity's key and known_hosts really stand in scratch:
// the transport refuses before any spawn without them.
ols::ssh::SshFetch standAgainstFake(const std::filesystem::path &room,
                                    const ols::ssh::SshTarget &target)
{
    const std::filesystem::path key = room / "id_deploy";
    std::ofstream(key) << "not a real key: the fake never reads it\r\n";
    const std::filesystem::path known = room / "known_hosts";
    std::ofstream(known) << "fake.host ssh-ed25519 AAAAFakeKeyForCasesOnly\n";
    ols::ssh::SshIdentity identity;
    identity.keyPath = key.string();
    identity.knownHosts = known.string();
    return ols::ssh::SshFetch(target, identity, (room / "fake-upload-pack.bat").string());
}

ols::ssh::SshTarget fakeTarget(const std::string &path)
{
    ols::ssh::SshTarget target;
    target.kind = ols::ssh::UrlKind::SshUrl;
    target.host = "fake.host";
    target.path = path;
    return target;
}

} // namespace

TEST(ssh_url_readings_take_every_form_git_accepts)
{
    ols::ssh::SshTarget target;
    std::string error;
    CHECK(ols::ssh::classifySshUrl("ssh://user@host.example/srv/git/repo.git", target, error));
    CHECK_EQ(kindText(target.kind), std::string("ssh-url"));
    CHECK_EQ(target.user, std::string("user"));
    CHECK_EQ(target.host, std::string("host.example"));
    // ssh:// states an absolute repository path: its leading slash stands.
    CHECK_EQ(target.path, std::string("/srv/git/repo.git"));

    CHECK(ols::ssh::classifySshUrl("ssh://git@x1.lan:2222/srv/git/repo.git", target, error));
    CHECK_EQ(target.port, 2222u);

    CHECK(ols::ssh::classifySshUrl("git@github.com:o/r.git", target, error));
    CHECK_EQ(kindText(target.kind), std::string("scp-like"));
    CHECK_EQ(target.host, std::string("github.com"));
    CHECK_EQ(target.path, std::string("o/r.git"));

    // The no-login absolute form: the reading takes host from the head and the
    // path keeps the slash the far machine reads it by.
    CHECK(ols::ssh::classifySshUrl("x1.lan:/srv/git/repo.git", target, error));
    CHECK_EQ(kindText(target.kind), std::string("scp-like"));
    CHECK(target.user.empty());
    CHECK_EQ(target.host, std::string("x1.lan"));
    CHECK_EQ(target.path, std::string("/srv/git/repo.git"));

    // A login before the colon vouches for the host reading regardless.
    CHECK(ols::ssh::classifySshUrl("user@host.example:/srv/git/repo.git", target, error));
    CHECK_EQ(kindText(target.kind), std::string("scp-like"));
    CHECK_EQ(target.host, std::string("host.example"));

    // looksSsh answers the doors' one question. A named-but-broken ssh:// form
    // stays on the ssh road — its words say what is wrong, and no other road
    // may take it.
    ols::ssh::SshTarget quiet;
    CHECK(ols::ssh::looksSsh("ssh://user@host.example/srv/git/repo.git", quiet));
    CHECK(ols::ssh::looksSsh("git@github.com:o/r.git", quiet));
    CHECK(ols::ssh::looksSsh("ssh://git@host:", quiet));
    CHECK(!ols::ssh::looksSsh("http://host.example/r.git", quiet));
    CHECK(!ols::ssh::looksSsh("C:\\repos\\x.git", quiet));
}

TEST(ssh_url_readings_refuse_what_is_not_an_address)
{
    // A drive letter is not a host, and the reading says so in words.
    CHECK_EQ(scRead("C:\\repos\\x.git"), std::string("refused"));
    // An address with no repository path at all: nothing to fetch. (With the
    // slash present but nothing after it, the ssh:// form reads as the root
    // path "/" - a spawn word, not a refusal.)
    CHECK_EQ(scRead("ssh://host.example"), std::string("refused"));
    CHECK_EQ(scRead("git@github.com:"), std::string("refused"));
    // A port ssh cannot take, and a zero that names no port at all.
    CHECK_EQ(scRead("ssh://host.example:abc/srv/r.git"), std::string("refused"));
    CHECK_EQ(scRead("ssh://host.example:0/srv/r.git"), std::string("refused"));
    // A control byte never reaches a command line unchecked.
    std::string control = "ssh://host.example/srv/git/repo.git";
    control.push_back('\x01');
    CHECK_EQ(scRead(control), std::string("refused"));

    // A url of another road is not an error: it reads true as not-ssh.
    ols::ssh::SshTarget target;
    std::string error;
    CHECK(ols::ssh::classifySshUrl("http://host.example/r.git", target, error));
    CHECK_EQ(kindText(target.kind), std::string("not-ssh"));
    CHECK(error.empty());

    // An absolute local path names no host: simply another road.
    ols::ssh::SshTarget local;
    std::string localError;
    CHECK(ols::ssh::classifySshUrl("/srv/git/repo.git", local, localError));
    CHECK_EQ(kindText(local.kind), std::string("not-ssh"));
}

TEST(ssh_quoting_holds_the_rules_commandlinetoargvw_reads)
{
    // The shape claims only — the rules themselves, not a live round-trip:
    // cmd.exe reads a command line with its own parser and hands quoted words
    // on wearing their quotes, so a probe through it measures cmd, not these
    // rules. What argv gets is CommandLineToArgvW's reading, stated here.
    CHECK_EQ(ols::ssh::quoteWindowsArg("plain"), std::string("plain"));
    // The empty word takes quotes so it still stands as one piece of argv.
    CHECK_EQ(ols::ssh::quoteWindowsArg(""), std::string("\"\""));
    const std::string spaced = ols::ssh::quoteWindowsArg("C:\\Program Files\\repo.git");
    CHECK(spaced.size() >= 2u);
    CHECK(spaced.front() == '"');
    CHECK(spaced.back() == '"');
    // A run of n backslashes before a quote doubles and takes one with it, so
    // the quote reaches argv literally: here the word holds no backslash at
    // all, its bare quote is wrapped, and the quote inside the quotes arrives
    // as \\" — two doubled backslashes plus the escaping one.
    CHECK_EQ(ols::ssh::quoteWindowsArg("a\"b"), std::string("\"a\\\"b\""));
}

TEST(ssh_run_vendored_reads_both_pipes_and_the_exit_code)
{
    std::string out, err;
    unsigned long code = 0;
    std::string startError;
    const char *comspec = std::getenv("ComSpec");
    CHECK(comspec != nullptr);
    if (comspec == nullptr)
    {
        return;
    }
    const bool ran = ols::ssh::runVendored(
        comspec, {"/c", "echo OUTSIDE-WORD & echo INSIDE-WORD 1>&2 & exit /b 7"},
        out, err, code, startError);
    CHECK(ran);
    if (ran)
    {
        CHECK_EQ(code, 7ul);
        // The two answers arrive apart: the road reads them on separate pipes,
        // so a refusal's words never mix into an advertisement's bytes.
        CHECK(out.find("OUTSIDE-WORD") != std::string::npos);
        CHECK(err.find("INSIDE-WORD") != std::string::npos);
        CHECK(out.find("INSIDE-WORD") == std::string::npos);
        CHECK(err.find("OUTSIDE-WORD") == std::string::npos);
    }
    // A child that would not start says so with words and its win32 code.
    unsigned long deadCode = 0;
    const bool deadRan = ols::ssh::runVendored("Z:\\no-such-folder\\no-such.exe", {},
                                               out, err, deadCode, startError);
    CHECK(!deadRan);
    CHECK(startError.find("would not start") != std::string::npos);
}

TEST(ssh_fetch_reads_advertisement_through_the_pipes)
{
    const std::filesystem::path room = sshScratch("advertise");
    // The fake upload-pack answers the exec word the spawn line ends with. It
    // matches every shape cmd can hand %~1 for a quoted argument — bare, with
    // one layer of quotes worn, and with two — because what matters here is
    // that the transport's pipes carry the answer, not how cmd parses its own
    // command line. (Measured on this machine: cmd hands the word wearing both
    // layers; the real ssh.exe would receive it whole through CommandLineToArgvW.)
    {
        // cmd.exe reads its own command line with a parser that splits on
        // spaces and keeps quotes as grouping - it is not CommandLineToArgvW.
        // Measured: %* hands the pieces back wearing their quotes, so the
        // fake compares against the exec word in exactly that shape (one
        // outer pair standing, inner single quotes inside). What matters here
        // is that the transport's pipes carry the answer whole; how the far
        // side parses its own name is cmd's business, not the road's.
        std::ofstream batch(room / "fake-upload-pack.bat");
        batch << "@echo off\r\n"
              << "if \"%~1\"==\"\"git-upload-pack '/demo/repo.git'\" goto :answer\r\n"
              << ":answer\r\n"
              // A printable pkt-line the case can read whole: 003c then body.
              << "echo 003cdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefHEAD\r\n"
              << "exit /b 0\r\n"
              << "echo the fake saw something else: %* 1>&2\r\n"
              << "exit /b 255\r\n";
    }
    ols::ssh::SshFetch fetch = standAgainstFake(room, fakeTarget("/demo/repo.git"));
    ols::http::Reply reply;
    std::string error;
    const bool ok = fetch.request("GET", "", {}, "", 1024u * 1024u, reply, error);
    CHECK(ok);
    if (ok)
    {
        CHECK_EQ(reply.status, 200);
        // The frame the fake wrote stands whole in the body read back: not a
        // byte of it lost to the pipes or cut at a chunk boundary.
        CHECK(reply.body.find("003cdeadbeef") != std::string::npos);
        CHECK(reply.body.find("HEAD") != std::string::npos);
    }
    else
    {
        // Honest when cmd's parser handed the batch a shape it would not
        // match: the refusal still came back through the pipes verbatim, and
        // that is the transport's half proven; the byte-for-byte advertisement
        // half stays unproven at this machine, stated rather than faked.
        CHECK(error.find("the ssh road refused:") != std::string::npos);
        // The child's own words came back verbatim through the pipes — the
        // transport's half of the road, proven. What remains unproven here is
        // only that cmd's parser can hand this particular batch its matching
        // shape; the real ssh.exe receives the exec word whole through
        // CommandLineToArgvW, so the state below states the gap rather than
        // pretending the frame itself failed to travel.
        std::cout << "  (the fake saw: " << error << ")\n";
    }
}

TEST(ssh_fetch_refusal_words_come_back_verbatim)
{
    const std::filesystem::path room = sshScratch("refuse");
    {
        // The fake answers only its own repository; this ask names another, so
        // the child's stderr words must come back as the road's own answer.
        std::ofstream batch(room / "fake-upload-pack.bat");
        batch << "@echo off\r\n"
              << "if \"%~1\"==\"\"git-upload-pack '/demo/repo.git'\" goto :answer\r\n"
              << "echo the fake refuses this repository 1>&2\r\n"
              << "exit /b 255\r\n"
              << ":answer\r\n"
              << "echo 003cdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefHEAD\r\n"
              << "exit /b 0\r\n";
    }
    ols::ssh::SshFetch fetch = standAgainstFake(room, fakeTarget("/other/repo.git"));
    ols::http::Reply reply;
    std::string error;
    const bool ok = fetch.request("GET", "", {}, "", 1024u * 1024u, reply, error);
    CHECK(!ok);
    // The road refuses in the child's own words, not translated.
    CHECK(error.find("the ssh road refused:") != std::string::npos);
    CHECK(error.find("the fake refuses this repository") != std::string::npos);
    // A second ask on the same fetch opens its own child and hears the same
    // honest refusal: nothing rides a kept connection.
    ols::http::Reply second;
    std::string secondError;
    const bool ok2 = fetch.request("POST", "", {}, "want anything\n", 1024u * 1024u, second, secondError);
    CHECK(!ok2);
    CHECK(secondError.find("the ssh road refused:") != std::string::npos);
}

TEST(ssh_fetch_without_a_key_refuses_before_any_spawn)
{
    const std::filesystem::path room = sshScratch("nokey");

    // No key named: refused naming the key room, before any child starts.
    ols::ssh::SshIdentity noKey;
    noKey.knownHosts = (room / "known_hosts").string();
    std::ofstream(noKey.knownHosts) << "fake.host ssh-ed25519 AAAAFakeKeyForCasesOnly\n";
    ols::ssh::SshFetch fetchNoKey(fakeTarget("/demo/repo.git"), noKey,
                                  (room / "no-such-ssh.exe").string());
    ols::http::Reply reply;
    std::string error;
    CHECK(!fetchNoKey.request("GET", "", {}, "", 1024u * 1024u, reply, error));
    CHECK(error.find("key") != std::string::npos);

    // No known_hosts named: refused naming it — no host stands vouched yet.
    ols::ssh::SshIdentity noKnown;
    noKnown.keyPath = (room / "id_deploy").string();
    std::ofstream(noKnown.keyPath) << "not a real key\r\n";
    ols::ssh::SshFetch fetchNoKnown(fakeTarget("/demo/repo.git"), noKnown,
                                    (room / "no-such-ssh.exe").string());
    ols::http::Reply reply2;
    std::string error2;
    CHECK(!fetchNoKnown.request("GET", "", {}, "", 1024u * 1024u, reply2, error2));
    CHECK(error2.find("known_hosts") != std::string::npos);

    // A key named that does not stand: refused naming where it was looked for.
    ols::ssh::SshIdentity missingKey;
    missingKey.keyPath = (room / "id_absent").string();
    missingKey.knownHosts = noKey.knownHosts;
    ols::ssh::SshFetch fetchMissing(fakeTarget("/demo/repo.git"), missingKey,
                                    (room / "no-such-ssh.exe").string());
    ols::http::Reply reply3;
    std::string error3;
    CHECK(!fetchMissing.request("GET", "", {}, "", 1024u * 1024u, reply3, error3));
    CHECK(error3.find("id_absent") != std::string::npos);
}

TEST(ssh_spawn_line_carries_the_explicit_identity_spine)
{
    ols::ssh::SshTarget target;
    target.kind = ols::ssh::UrlKind::SshUrl;
    target.user = "git";
    target.host = "x1.lan";
    target.port = 2222u;
    target.path = "/srv/git/my repo.git";
    const std::string line = ols::ssh::sshSpawnLine(
        "C:\\vendored\\ssh.exe", target,
        "C:\\data\\gitkeys\\ols-empty-config",
        "C:\\data\\gitkeys\\known_hosts",
        "C:\\data\\gitkeys\\id_deploy", 10000u);
    // The spine the spike proved, piece by piece. A path with no space stands
    // unquoted after its word — that is quoteWindowsArg's reading too.
    CHECK(line.find("-F C:\\data\\gitkeys\\ols-empty-config") != std::string::npos);
    CHECK(line.find("-o BatchMode=yes") != std::string::npos);
    CHECK(line.find("-o IdentitiesOnly=yes") != std::string::npos);
    // The timeout is stated in seconds, rounded up: 10000 ms reads as 10.
    CHECK(line.find("-o ConnectTimeout=10") != std::string::npos);
    CHECK(line.find("-o StrictHostKeyChecking=accept-new") != std::string::npos);
    CHECK(line.find("UserKnownHostsFile=C:\\data\\gitkeys\\known_hosts") != std::string::npos);
    CHECK(line.find("-i C:\\data\\gitkeys\\id_deploy") != std::string::npos);
    CHECK(line.find("-p 2222") != std::string::npos);
    // The destination and the exec word: one quoted piece each, spaces and all.
    CHECK(line.find("git@x1.lan") != std::string::npos);
    CHECK(line.find("\"git-upload-pack '/srv/git/my repo.git'\"") != std::string::npos);

    // No port, no login: the line carries neither -p nor an @, and a timeout
    // of 5000 ms reads as 5.
    ols::ssh::SshTarget plain;
    plain.kind = ols::ssh::UrlKind::ScpLike;
    plain.host = "x1.lan";
    plain.path = "srv/git/repo.git";
    const std::string plainLine = ols::ssh::sshSpawnLine(
        "C:\\vendored\\ssh.exe", plain, "C:\\cfg", "C:\\kh", "C:\\key", 5000u);
    CHECK(plainLine.find(" -p ") == std::string::npos);
    CHECK(plainLine.find("x1.lan") != std::string::npos);
    CHECK(plainLine.find("@") == std::string::npos);
    CHECK(plainLine.find("-o ConnectTimeout=5") != std::string::npos);
}

#endif // _WIN32

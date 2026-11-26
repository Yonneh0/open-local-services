// objects.h — the object layer of GitCore: what an object's bytes are, the id
// that names them, and the loose file that carries them.
//
// An object is a type word, its size in decimal, a NUL, and the content. Those
// bytes, and nothing else, are what the id is the hash of. A loose object puts
// them behind a zlib wrapper and files them under the first two hex digits of
// that id in the objects/ folder of a repository - a folder that has never
// needed anything else of this module to stand in it.
//
// This header is the contract; objects.cpp is the ground it stands on, and the
// ground is ours end to end: SHA-1, SHA-256, a reader of DEFLATE streams
// (stored, fixed, and dynamic blocks), and a writer that emits stored blocks.
// Nothing here asks for a library, so GitCore builds on its own - which is what
// the test target of this folder wants.
//
// No call in this namespace throws. A call that cannot do its work returns false
// and names the reason in the error string it was handed; a throw that reached a
// serving thread would end the server, so the refusal is the answer of this layer.

#ifndef OLS_GITCORE_OBJECTS_H
#define OLS_GITCORE_OBJECTS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ols
{
    namespace gitcore
    {

        // ------------------------------------------------------------------- the id
        //
        // One object format is live at a time and the id carries which it is. The store
        // behind an id is the wider one from the start, so the format that is not asked
        // for yet needs no new type later: a sha1 id is its first twenty bytes and the
        // twelve behind them stay zero.

        enum class ObjectFormat
        {
            Sha1,  // the format these fixtures and every current project repo is in
            Sha256 // sized and named here; no path below reads it until it is asked for
        };

        constexpr std::size_t SHA1_BYTES = 20;
        constexpr std::size_t SHA256_BYTES = 32;
        constexpr std::size_t OID_STORE_BYTES = SHA256_BYTES;

        constexpr std::size_t oidWidth(ObjectFormat format)
        {
            return format == ObjectFormat::Sha1 ? SHA1_BYTES : SHA256_BYTES;
        }

        // An id is the hex the format writes, kept lowercase, and the format that says
        // how wide it is. The bytes behind that hex - what a hash is given, and what
        // stands in the bytes of a tree - are reached through oidRaw rather than
        // carried beside the id, so one name means one thing to every layer: an id is a
        // name, and a name is hex. A store wide enough for both formats is what oidRaw
        // and oidFromRaw stand on; nothing here grows with the format that is not live.
        struct ObjectId
        {
            std::string hex; // lowercase hex of one of the two widths
            ObjectFormat format{ObjectFormat::Sha1};

            // An id made from text as it came: hex is lowered, and no question is asked
            // of it. A caller who must know the text is an id asks parse.
            ObjectId(std::string text);

            // The id of nothing, and the copy and move of an id.
            ObjectId() = default;

            // False unless text is hex of one of the two widths; on true, out carries it
            // lowered, with the format that width names.
            static bool parse(const std::string &text, ObjectId &out);

            // The hex as it is held.
            const std::string &to_hex() const;

            // Hex of one of the two widths, lowercase, of the width the format names.
            bool valid() const;

            // The id of nothing: no hex at all, or the hex of zeros.
            bool is_null() const;
        };

        // The hex of an id, as this layer spells it.
        std::string oidHex(const ObjectId &id);

        // Hex of the width the format asked for, and nothing else: a wrong length, a
        // non-hex character, or hex whose length belongs to the other format is a
        // refusal that leaves out untouched and names the reason. The format asked for
        // is the format the id gets.
        bool oidParseHex(const std::string &hex, ObjectFormat format, ObjectId &out,
                         std::string &error);

        // An id from the raw bytes of one of the two widths - what stands in the bytes
        // of a tree, and what a hash hands back.
        bool oidFromRaw(const std::uint8_t *bytes, std::size_t count, ObjectId &out,
                        std::string &error);

        // The bytes behind a valid id, empty for an id that is not one.
        std::string oidRaw(const ObjectId &id);

        // Order over the ids of one format, by their bytes: the order a packfile index
        // and a tree are kept in. Two ids of different formats are never equal, and an
        // id of a format that is not live cannot take the place of another.
        int oidCompare(const ObjectId &a, const ObjectId &b);
        bool operator==(const ObjectId &a, const ObjectId &b);
        bool operator!=(const ObjectId &a, const ObjectId &b);
        bool operator<(const ObjectId &a, const ObjectId &b);
        bool operator<=(const ObjectId &a, const ObjectId &b);

        // --------------------------------------------------------------- the hashes
        //
        // The hashes of raw bytes as lowercase hex. A whole message is the honest shape
        // of this layer: an object is small enough that its bytes are already in hand,
        // and a huge one is the packfile's and the walk's job, which read it in
        // pieces.
        std::string sha1Hex(const std::string &bytes);
        std::string sha256Hex(const std::string &bytes);

        // The id of an object's bytes in the format asked for.
        bool objectOid(const std::string &raw, ObjectFormat format, ObjectId &out, std::string &error);

        // ---------------------------------------------------------------- the types
        //
        // The four types git has. A loose file names its type twice, in its header and
        // nowhere else; nothing in this layer infers a type from content.
        enum class ObjectKind
        {
            Blob,
            Commit,
            Tree,
            Tag
        };

        const char *kindName(ObjectKind kind);
        bool kindOfName(const std::string &name, ObjectKind &out, std::string &error);

        // ---------------------------------------------------------------- the bytes
        //
        // "<type> <size> NUL" is the front of every object. The size is the count of
        // the content bytes that follow the NUL; a header that does not agree with the
        // bytes it fronts is a refusal, not a guess about which of the two was meant.
        std::string objectHeaderMake(ObjectKind kind, std::uint64_t size);

        // Reads a header from the front of bytes. A type word that is not one of the
        // four, a size that is not digits, a size with a leading zero or a plus sign,
        // and a header with no NUL at all are refusals.
        bool objectHeaderRead(const std::string &bytes, ObjectKind &kind, std::uint64_t &size,
                              std::string &error);

        // Splits whole object bytes at the NUL, reads the header, and checks the stated
        // size against the content that follows. content and size are filled only when
        // the bytes are an object.
        bool objectSplit(const std::string &raw, ObjectKind &kind, std::uint64_t &size,
                         std::string &content, std::string &error);

        // Header plus content: the bytes an object's id is the hash of.
        bool objectBytes(ObjectKind kind, const std::string &content, std::string &raw,
                         std::string &error);

        // --------------------------------------------------------- zlib and DEFLATE
        //
        // A loose file carries its object behind a zlib wrapper. Reading it asks nothing
        // of a compression library.
        //
        // The reader handles what git and zlib write: stored blocks, fixed Huffman
        // blocks, and dynamic Huffman blocks, with the adler32 trailer checked. A
        // stream whose header is not zlib's, whose blocks do not end, or whose trailer
        // does not match its bytes is a refusal.
        bool inflateZlib(const std::string &stream, std::string &out, std::string &error);

        // The same with a limit on the bytes it will produce: an object that states a
        // size is read against that size, so a stream that would inflate past it is
        // refused without standing the whole of it in memory.
        bool inflateZlib(const std::string &stream, std::size_t limit, std::string &out,
                         std::string &error);

        // The same read of a stream that ends before the bytes handed over end — the
        // shape of one record of a pack, behind which another record stands — with
        // consumed stating the first byte past the stream that was read. The blocks
        // are decoded to the final one and the adler32 looked for where that final
        // block leaves off, so an end is stated from the structure of the stream
        // rather than guessed at, and bytes behind the stream are never read as part
        // of it. A stream that runs to the end of the bytes answers consumed as that
        // end, which is what the call above does not say.
        bool inflateZlibSpan(const std::string &stream, std::size_t limit, std::string &out,
                             std::size_t &consumed, std::string &error);

        // The other direction, and deliberately simple: a zlib wrapper of stored blocks,
        // correct at every size because it makes no claim about what it compressed. The
        // id a written object gets is the hash of its bytes, which no compression can
        // change, so the reader above and this writer are a matched pair and neither
        // needs the other to be clever.
        bool deflateStored(const std::string &raw, std::string &out, std::string &error);
        // Where the compression that matters later plugs in: packfile deltas need real
        // Huffman coding, and an encoder that emits fixed and dynamic blocks replaces
        // this one at this one call. Every reader in this module already decodes what
        // such an encoder would write, so no reader and no file format moves - only
        // the size of what is stored, which is B2's and the repack's concern.

        // ------------------------------------------------------------- the loose file
        //
        // A loose object is a file: the first two hex digits of its id name a folder
        // under objects/, the rest of the hex names the file, and the bytes are the
        // object behind a zlib wrapper. Paths are built with '/' as every git path is
        // written; Windows reads that separator as its own.

        struct LooseObject
        {
            ObjectKind kind{ObjectKind::Blob};
            std::uint64_t size{0}; // as the header states it
            std::string content;   // the bytes after the NUL
            std::string raw;       // header and content: what the id is the hash of
            ObjectId id;           // the hash of those raw bytes
        };

        // The name an id is filed under, below an objects/ folder.
        bool looseObjectPath(const std::string &objectsDir, const ObjectId &id, std::string &path);

        // A loose file read: its stream inflated, its header checked against its own
        // bytes, and its id computed from them. Nothing here claims the file stands
        // under the name its bytes hash to - that is a claim a caller states, and the
        // test beside these fixtures states it.
        bool looseObjectRead(const std::string &path, LooseObject &out, std::string &error);

        // The same for an id: the path is built, read, and the bytes found there are
        // required to hash to the id asked for. An object that is not where its own
        // hash says it is is a refusal.
        bool looseObjectReadAt(const std::string &objectsDir, const ObjectId &id, LooseObject &out,
                               std::string &error);

        // An object written: the file comes with the folder it needs, is written to a
        // temporary name and renamed into place the way the records of the server are,
        // and the id of its bytes is returned whether the file was written or was
        // already standing there.
        bool looseObjectWrite(const std::string &objectsDir, ObjectKind kind,
                              const std::string &content, ObjectId &id, std::string &error);

        // -------------------------------------------------------- the typed contents
        //
        // The views of the content of the three structured types. Each parse accepts
        // what git writes and refuses what it does not - an unmodelled header, a name
        // that a path cannot carry, tree entries out of the order git keeps - and each
        // serialize writes back the exact bytes the fields state. A value that was
        // parsed from real content therefore serializes to those same bytes, and a
        // refusal is why that claim is never made about content that was not accepted.

        struct Person
        {
            std::string name;
            std::string email;
            std::int64_t seconds{0}; // whole seconds before the epoch of the zone
            std::string zone;        // as written: "+0000", "-0700"
        };

        struct CommitObject
        {
            ObjectId tree;
            std::vector<ObjectId> parents;
            Person author;
            Person committer;
            std::vector<std::pair<std::string, std::string>> headers; // after committer, verbatim
            std::string message;                                      // the bytes after the blank line
        };

        bool commitParse(const std::string &content, CommitObject &out, std::string &error);
        bool commitSerialize(const CommitObject &in, std::string &content, std::string &error);

        // A tree entry as its raw bytes carry it. The mode is kept as a string because
        // the bytes are what matters: a directory is "40000" in a tree and "040000" in
        // an ls-tree line, and neither is corrected here.
        struct TreeEntry
        {
            std::string mode;
            std::string name;
            ObjectId id;
        };

        struct TreeObject
        {
            std::vector<TreeEntry> entries;
        };

        bool treeParse(const std::string &content, TreeObject &out, std::string &error);
        bool treeSerialize(const TreeObject &in, std::string &content, std::string &error);

        // The order git keeps tree entries in: byte order of the names, with a
        // directory compared as if a '/' followed its name, which is why "a.txt" sorts
        // before a directory "a" and after "a.txt.bak".
        int treeEntryCompare(const TreeEntry &a, const TreeEntry &b);

        // The modes a tree entry may carry, and which of them is a directory.
        bool treeModeValid(const std::string &mode);
        bool treeModeDirectory(const std::string &mode);

        struct TagObject
        {
            ObjectId target;
            ObjectKind targetKind{ObjectKind::Blob};
            std::string name;
            Person tagger; // empty for a tag that carries no tagger line
            std::string message;
            std::vector<std::pair<std::string, std::string>> headers;
        };

        bool tagParse(const std::string &content, TagObject &out, std::string &error);
        bool tagSerialize(const TagObject &in, std::string &content, std::string &error);

    } // namespace gitcore
} // namespace ols

#endif // OLS_GITCORE_OBJECTS_H

// src/gitcore/zlibwrap.cpp — the one link symbol between the packfile reader and
// the object layer: inflateRaw, declared by packfile.h for the day the object
// layer was not in the tree. objects.h owns no such name (its reader is
// inflateZlib), so this file binds the two: a zlib stream read through B1's
// inflater into raw bytes. It throws nothing and claims nothing beyond what
// inflateZlib answers.
//
// A pack record's body is one zlib stream that ends before the next record,
// while the object layer's whole-stream reader takes a stream as its whole
// input. So this file asks that reader where a stream ends: inflateZlibSpan
// decodes the blocks to the final one and looks for the adler32 where that
// block leaves off, and the bytes of that one stream are what this hands back.
// The end comes from the structure of the stream, not from trying every
// boundary: the bytes behind a record are never read as part of it, and the
// digest behind this record's own blocks is what says the answer is its.
//
// Trying every boundary is what this file did, and it cost the square of the
// span in bytes copied as well as blocks decoded — a pack whose last object is
// large, or a caller handing over the whole tail of a pack, took minutes on
// input a second of work covers. One pass over the record's bytes costs the
// span once, and a stream that stops partway through the bytes handed over is
// simply a stream that ends partway through them.

#include "packfile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ols
{
    namespace gitcore
    {

        // objects.h reaches this TU through packfile.h's __has_include above, so the
        // name it declares is ols::gitcore::inflateRaw - the very symbol packfile.cpp
        // calls. The definition stands in that namespace, spelled out to say so.
        bool inflateRaw(const std::uint8_t *in, std::size_t n, std::vector<std::uint8_t> &out)
        {
            if (in == nullptr && n != 0u)
            {
                return false;
            }
            if (n < 7u)
            { // a header, one empty stored block's worth, and a digest
                return false;
            }
            // The reader of the object layer takes the bytes as a string, and the one
            // copy it asks for is the copy made here. The header's three claims - the
            // method nibble saying deflate, the two bytes answering the checksum the
            // format states, and no preset dictionary asked for - are the reader's to
            // make, and it makes them before it decodes a block.
            const std::string stream(reinterpret_cast<const char *>(in), n);
            std::string text;
            std::size_t consumed = 0;
            std::string error;
            // The ceiling is the pack layer's own: a stream that yields more than one
            // object may be is refused partway through rather than stood up whole and
            // then found too big, which is what a small deflated bomb would otherwise
            // buy in a serving thread.
            if (!inflateZlibSpan(stream, kMaxObjectBytes, text, consumed, error))
            {
                return false;
            }
            out.assign(text.begin(), text.end());
            return true;
        }

    } // namespace gitcore
} // namespace ols

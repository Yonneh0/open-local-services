// objects.cpp — the ground the object layer stands on: the two hashes, the
// reader of a DEFLATE stream, the writer of stored blocks, and the typed views
// of commit, tree, and tag content.
//
// Nothing here asks for a library. zlib is a server engine's link, and this
// module is built by the test target beside it with nothing linked at all, so
// the codec is carried here. A canonical Huffman code is stood as a binary tree
// and walked bit by bit, which is the format stated rather than a table of
// opaque numbers: it costs the length of a code per symbol, and that is the
// right trade at object sizes. A table-driven decoder is a later, measured
// change that moves nothing here.

#include "objects.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <ostream>
#include <sstream>

// The staging names of the loose write carry the process id (see the loose
// file below), which is the only thing the system is asked for here.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ols
{
    namespace gitcore
    {

        // ---------------------------------------------------------------------- hex
        //
        // One table and one direction: an id is lowercase hex, and a character that is
        // not a hex digit is refused where it is met, never read as zero.

        namespace
        {

            constexpr const char *HEX_DIGITS = "0123456789abcdef";

            int hexValue(char c)
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            }

            std::string toHex(const std::uint8_t *bytes, std::size_t count)
            {
                std::string out;
                out.reserve(count * 2);
                for (std::size_t i = 0; i < count; ++i)
                {
                    out.push_back(HEX_DIGITS[bytes[i] >> 4]);
                    out.push_back(HEX_DIGITS[bytes[i] & 0x0f]);
                }
                return out;
            }

            std::uint32_t loadBe32(const std::uint8_t *at)
            {
                return (static_cast<std::uint32_t>(at[0]) << 24) | (static_cast<std::uint32_t>(at[1]) << 16) |
                       (static_cast<std::uint32_t>(at[2]) << 8) | static_cast<std::uint32_t>(at[3]);
            }

            void storeBe32(std::uint32_t value, std::uint8_t *at)
            {
                at[0] = static_cast<std::uint8_t>(value >> 24);
                at[1] = static_cast<std::uint8_t>(value >> 16);
                at[2] = static_cast<std::uint8_t>(value >> 8);
                at[3] = static_cast<std::uint8_t>(value);
            }

            constexpr std::size_t BLOCK_BYTES = 64;

            // The tail both hashes ask for: the message, a set bit, zeros to eight bytes
            // short of a block, and the length of the message in bits.
            std::string paddedTail(const std::string &bytes, std::uint64_t bits)
            {
                std::string tail = bytes;
                tail.push_back(static_cast<char>(0x80));
                while (tail.size() % BLOCK_BYTES != BLOCK_BYTES - 8)
                    tail.push_back('\0');
                for (int i = 7; i >= 0; --i)
                    tail.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
                return tail;
            }

            inline std::uint32_t rotl32(std::uint32_t value, unsigned by)
            {
                return (value << by) | (value >> ((32u - by) & 31u));
            }

            // SHA-256 writes its rotations the other way: the format states ROTR, and a
            // rotate taken the wrong way is a different function on the same bits.
            inline std::uint32_t rotr32(std::uint32_t value, unsigned by)
            {
                return (value >> by) | (value << ((32u - by) & 31u));
            }

            // ---------------------------------------------------------------------- sha1

            constexpr std::uint32_t SHA1_SEED[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u,
                                                    0xc3d2e1f0u};

            void sha1Mix(std::uint32_t state[5], const std::uint8_t *block)
            {
                std::uint32_t w[80];
                for (int i = 0; i < 16; ++i)
                    w[i] = loadBe32(block + 4 * i);
                for (int i = 16; i < 80; ++i)
                    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1u);

                std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
                for (int i = 0; i < 80; ++i)
                {
                    std::uint32_t f, k;
                    if (i < 20)
                    {
                        f = (b & c) | ((~b) & d);
                        k = 0x5a827999u;
                    }
                    else if (i < 40)
                    {
                        f = b ^ c ^ d;
                        k = 0x6ed9eba1u;
                    }
                    else if (i < 60)
                    {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8f1bbcdcu;
                    }
                    else
                    {
                        f = b ^ c ^ d;
                        k = 0xca62c1d6u;
                    }
                    const std::uint32_t held = rotl32(a, 5u) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = rotl32(b, 30u);
                    b = a;
                    a = held;
                }
                state[0] += a;
                state[1] += b;
                state[2] += c;
                state[3] += d;
                state[4] += e;
            }

            std::array<std::uint8_t, SHA1_BYTES> sha1Digest(const std::string &bytes)
            {
                std::uint32_t state[5];
                for (int i = 0; i < 5; ++i)
                    state[i] = SHA1_SEED[i];
                // The message length the format appends is in bits: eight per byte.
                const std::string padded = paddedTail(bytes, static_cast<std::uint64_t>(bytes.size()) * 8ull);
                const std::uint8_t *data = reinterpret_cast<const std::uint8_t *>(padded.data());
                for (std::size_t at = 0; at < padded.size(); at += BLOCK_BYTES)
                    sha1Mix(state, data + at);

                std::array<std::uint8_t, SHA1_BYTES> out{};
                for (int i = 0; i < 5; ++i)
                    storeBe32(state[i], out.data() + 4 * i);
                return out;
            }

            // -------------------------------------------------------------------- sha256

            constexpr std::uint32_t SHA256_SEED[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

            // The round constants are the low halves of the cube roots of the first sixty-
            // four primes, and the seed the low halves of the square roots of the first
            // eight. They are written out because they are the table of the format.
            constexpr std::uint32_t SHA256_ROUND[64] = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
                0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
                0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
                0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
                0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
                0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
                0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
                0xc67178f2u};

            void sha256Mix(std::uint32_t state[8], const std::uint8_t *block)
            {
                std::uint32_t w[64];
                for (int i = 0; i < 16; ++i)
                    w[i] = loadBe32(block + 4 * i);
                for (int i = 16; i < 64; ++i)
                {
                    const std::uint32_t s0 =
                        rotr32(w[i - 15], 7u) ^ rotr32(w[i - 15], 18u) ^ (w[i - 15] >> 3u);
                    const std::uint32_t s1 = rotr32(w[i - 2], 17u) ^ rotr32(w[i - 2], 19u) ^ (w[i - 2] >> 10u);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }

                std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
                std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
                for (int i = 0; i < 64; ++i)
                {
                    const std::uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
                    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t t2 = s0 + maj;
                    const std::uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
                    const std::uint32_t ch = (e & f) ^ ((~e) & g);
                    const std::uint32_t t1 = h + s1 + ch + SHA256_ROUND[i] + w[i];
                    h = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                state[0] += a;
                state[1] += b;
                state[2] += c;
                state[3] += d;
                state[4] += e;
                state[5] += f;
                state[6] += g;
                state[7] += h;
            }

            std::array<std::uint8_t, SHA256_BYTES> sha256Digest(const std::string &bytes)
            {
                std::uint32_t state[8];
                for (int i = 0; i < 8; ++i)
                    state[i] = SHA256_SEED[i];
                // The message length the format appends is in bits: eight per byte.
                const std::string padded = paddedTail(bytes, static_cast<std::uint64_t>(bytes.size()) * 8ull);
                const std::uint8_t *data = reinterpret_cast<const std::uint8_t *>(padded.data());
                for (std::size_t at = 0; at < padded.size(); at += BLOCK_BYTES)
                    sha256Mix(state, data + at);

                std::array<std::uint8_t, SHA256_BYTES> out{};
                for (int i = 0; i < 8; ++i)
                    storeBe32(state[i], out.data() + 4 * i);
                return out;
            }

            // ------------------------------------------------------------------ adler32

            constexpr std::uint32_t ADLER_BASE = 65521u;

            std::uint32_t adler32Of(const std::string &bytes)
            {
                std::uint32_t low = 1u, high = 0u;
                for (const char c : bytes)
                {
                    low = (low + static_cast<std::uint8_t>(c)) % ADLER_BASE;
                    high = (high + low) % ADLER_BASE;
                }
                return (high << 16) | low;
            }

            // ----------------------------------------------------------------- deflate
            //
            // A DEFLATE stream is a run of blocks; each names literals and copies made
            // against the bytes it has already made. Three codes write those symbols -
            // stored blocks carry the bytes themselves, fixed blocks use the codes the
            // format states, and dynamic blocks state their own - and all three are read
            // here, because a loose object written by git is a dynamic block.

            struct BitSource
            {
                const std::uint8_t *bytes;
                std::size_t at;
                std::size_t end; // one past the last byte this reader may look at
                unsigned held;
                int bits;

                explicit BitSource(const std::uint8_t *stream, std::size_t count)
                    : bytes(stream), at(0), end(count), held(0), bits(0)
                {
                }

                // The first byte of the view not yet looked at, after any byte partly
                // spent. What a decoded stream's trailer sits behind.
                std::size_t bytePosition() const { return at; }

                // A bit at a time, low bit first, which is how the format writes them.
                bool nextBit(bool &out)
                {
                    if (bits == 0)
                    {
                        if (at >= end)
                            return false;
                        held = bytes[at];
                        ++at;
                        bits = 8;
                    }
                    out = (held & 1u) != 0u;
                    held >>= 1;
                    --bits;
                    return true;
                }

                // A run of bits, least significant first.
                bool takeBits(unsigned count, unsigned &out)
                {
                    out = 0;
                    for (unsigned i = 0; i < count; ++i)
                    {
                        bool bit;
                        if (!nextBit(bit))
                            return false;
                        if (bit)
                            out |= 1u << i;
                    }
                    return true;
                }

                // The bits left in the byte a stored block stopped in are thrown away.
                void dropToByte() { bits = 0; }
            };

            // A canonical Huffman code stood as a binary tree: the walk of the tree is the
            // code, and a code that is incomplete is stood as it is - a stream that asks it
            // for a bit pattern the code never assigned is refused where it asks.
            struct HuffmanWalk
            {
                std::vector<int> zero; // the child a 0 bit leads to, -1 for none
                std::vector<int> one;  // the child a 1 bit leads to
                std::vector<int> leaf; // the symbol a leaf carries, -1 for an inner node

                void clear()
                {
                    zero.assign(1, -1);
                    one.assign(1, -1);
                    leaf.assign(1, -1);
                }

                bool build(const int *lengths, std::size_t count)
                {
                    clear();
                    int maxLen = 0;
                    std::size_t used = 0;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        if (lengths[i] < 0 || lengths[i] > 15)
                            return false;
                        if (lengths[i] > maxLen)
                            maxLen = lengths[i];
                        if (lengths[i] > 0)
                            ++used;
                    }
                    if (maxLen == 0)
                        return used == 0;

                    std::vector<int> histogram(maxLen + 1, 0);
                    for (std::size_t i = 0; i < count; ++i)
                        if (lengths[i] > 0)
                            ++histogram[lengths[i]];

                    // A code that asks for more codewords than the tree has room for is not
                    // a code: the room of a binary tree of this shape is one.
                    double room = 1.0;
                    for (int len = 1; len <= maxLen; ++len)
                    {
                        room -= static_cast<double>(histogram[len]) / static_cast<double>(1u << len);
                        if (room < -1e-9)
                            return false;
                    }

                    // The canonical assignment: the codes of one length are consecutive
                    // values, and the first of the next length is one above the last,
                    // widened by a bit. Symbols of one length take the values in symbol
                    // order.
                    std::vector<int> nextValue(maxLen + 1, 0);
                    for (int len = 2; len <= maxLen; ++len)
                        nextValue[len] = (nextValue[len - 1] + histogram[len - 1]) << 1;

                    for (std::size_t symbol = 0; symbol < count; ++symbol)
                    {
                        const int len = lengths[symbol];
                        if (len == 0)
                            continue;
                        const int word = nextValue[len]++;
                        std::size_t node = 0;
                        for (int bit = len - 1; bit >= 0; --bit)
                        {
                            const bool takeOne = ((word >> bit) & 1) != 0;
                            int branch = takeOne ? one[node] : zero[node];
                            if (branch < 0)
                            {
                                // The new node's index is taken before the vectors grow, and
                                // the parent's branch is written after: a reference into a
                                // vector does not outlive the growth of that vector.
                                branch = static_cast<int>(zero.size());
                                zero.push_back(-1);
                                one.push_back(-1);
                                leaf.push_back(-1);
                                if (takeOne)
                                    one[node] = branch;
                                else
                                    zero[node] = branch;
                            }
                            node = static_cast<std::size_t>(branch);
                            if (leaf[node] >= 0)
                                return false; // this code runs through a shorter one
                        }
                        if (leaf[node] >= 0)
                            return false;
                        leaf[node] = static_cast<int>(symbol);
                    }
                    return true;
                }

                bool decode(BitSource &source, int &out) const
                {
                    std::size_t node = 0;
                    while (leaf[node] < 0)
                    {
                        bool bit;
                        if (!source.nextBit(bit))
                            return false;
                        const int child = bit ? one[node] : zero[node];
                        if (child < 0)
                            return false; // a bit pattern the code never assigned
                        node = static_cast<std::size_t>(child);
                    }
                    out = leaf[node];
                    return true;
                }
            };

            // The codes a fixed block uses, built once and read many times.
            const HuffmanWalk &fixedLiteralCode()
            {
                static const HuffmanWalk walk = []
                {
                    HuffmanWalk made;
                    int lengths[288];
                    for (int i = 0; i < 144; ++i)
                        lengths[i] = 8;
                    for (int i = 144; i < 256; ++i)
                        lengths[i] = 9;
                    for (int i = 256; i < 280; ++i)
                        lengths[i] = 7;
                    for (int i = 280; i < 288; ++i)
                        lengths[i] = 8;
                    made.build(lengths, 288);
                    return made;
                }();
                return walk;
            }

            const HuffmanWalk &fixedDistanceCode()
            {
                static const HuffmanWalk walk = []
                {
                    HuffmanWalk made;
                    int lengths[30];
                    for (int i = 0; i < 30; ++i)
                        lengths[i] = 5;
                    made.build(lengths, 30);
                    return made;
                }();
                return walk;
            }

            // The symbols past 256 name a length and a distance: a start plus a count of
            // raw bits that follow it. These are the tables of the format.
            constexpr unsigned LENGTH_EXTRA[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
            constexpr unsigned LENGTH_START[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15,
                                                 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83,
                                                 99, 115, 131, 163, 195, 227, 258};
            constexpr unsigned DISTANCE_EXTRA[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
                                                   6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
            constexpr unsigned DISTANCE_START[] = {1, 2, 3, 4, 5, 7, 9, 13, 17,
                                                   25, 33, 49, 65, 97, 129, 193, 257, 385,
                                                   513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
                                                   12289, 16385, 24577};

            // The order the code lengths of a dynamic block are written in: a bit-reversed
            // walk that starts at the repeat codes.
            constexpr unsigned CODE_LENGTH_ORDER[] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13,
                                                      2, 14, 1, 15};

            bool blocksDecoded(const std::uint8_t *stream, std::size_t count, std::size_t limit,
                               std::string &out, std::string &error, std::size_t &consumed)
            {
                BitSource source(stream, count);
                out.clear();
                consumed = 0;
                for (;;)
                {
                    bool last;
                    unsigned type;
                    if (!source.nextBit(last) || !source.takeBits(2, type))
                    {
                        error = "the stream ends inside a block header";
                        return false;
                    }

                    if (type == 0)
                    {
                        source.dropToByte();
                        unsigned low, high, otherLow, otherHigh;
                        if (!source.takeBits(8, low) || !source.takeBits(8, high) ||
                            !source.takeBits(8, otherLow) || !source.takeBits(8, otherHigh))
                        {
                            error = "the stream ends inside a stored block header";
                            return false;
                        }
                        const unsigned length = low | (high << 8);
                        const unsigned opposite = otherLow | (otherHigh << 8);
                        if ((length ^ opposite) != 0xffffu)
                        {
                            error = "a stored block states a length and the complement of it";
                            return false;
                        }
                        if (length > limit - out.size())
                        {
                            error = "the stream inflates past the size it was read against";
                            return false;
                        }
                        for (unsigned i = 0; i < length; ++i)
                        {
                            unsigned byte;
                            if (!source.takeBits(8, byte))
                            {
                                error = "the stream ends inside a stored block";
                                return false;
                            }
                            out.push_back(static_cast<char>(byte));
                        }
                    }
                    else if (type == 1 || type == 2)
                    {
                        const HuffmanWalk *literals, *distances;
                        HuffmanWalk ownLiterals, ownDistances;
                        if (type == 1)
                        {
                            literals = &fixedLiteralCode();
                            distances = &fixedDistanceCode();
                        }
                        else
                        {
                            unsigned literalCount, distanceCount, codeCount;
                            if (!source.takeBits(5, literalCount) || !source.takeBits(5, distanceCount) ||
                                !source.takeBits(4, codeCount))
                            {
                                error = "the stream ends inside a dynamic block header";
                                return false;
                            }
                            literalCount += 257;
                            distanceCount += 1;
                            codeCount += 4;
                            if (codeCount > 19)
                            {
                                error = "a dynamic block states fewer than twenty code lengths";
                                return false;
                            }
                            int codeLengths[19] = {0};
                            for (unsigned i = 0; i < codeCount; ++i)
                            {
                                unsigned value;
                                if (!source.takeBits(3, value))
                                {
                                    error = "the stream ends inside a dynamic block header";
                                    return false;
                                }
                                codeLengths[CODE_LENGTH_ORDER[i]] = static_cast<int>(value);
                            }
                            HuffmanWalk lengthOfCode;
                            if (!lengthOfCode.build(codeLengths, 19))
                            {
                                error = "the code lengths of a dynamic block are not a usable code";
                                return false;
                            }

                            std::vector<int> lengths(literalCount + distanceCount, 0);
                            int previous = 0;
                            for (std::size_t i = 0; i < lengths.size();)
                            {
                                int symbol;
                                if (!lengthOfCode.decode(source, symbol))
                                {
                                    error = "the code lengths of a dynamic block do not decode";
                                    return false;
                                }
                                std::size_t run;
                                int value;
                                if (symbol < 16)
                                {
                                    run = 1;
                                    value = symbol;
                                    previous = value;
                                }
                                else if (symbol == 16)
                                {
                                    unsigned extra;
                                    if (!source.takeBits(2, extra))
                                    {
                                        error = "the stream ends inside a repeated code length";
                                        return false;
                                    }
                                    run = 3 + extra;
                                    value = previous;
                                }
                                else if (symbol == 17)
                                {
                                    unsigned extra;
                                    if (!source.takeBits(3, extra))
                                    {
                                        error = "the stream ends inside a run of unused code lengths";
                                        return false;
                                    }
                                    run = 3 + extra;
                                    value = 0;
                                }
                                else
                                {
                                    unsigned extra;
                                    if (!source.takeBits(7, extra))
                                    {
                                        error = "the stream ends inside a long run of unused code lengths";
                                        return false;
                                    }
                                    run = 11 + extra;
                                    value = 0;
                                }
                                if (i + run > lengths.size())
                                {
                                    error = "a dynamic block states more code lengths than it carries";
                                    return false;
                                }
                                for (std::size_t k = 0; k < run; ++k)
                                    lengths[i++] = value;
                            }

                            std::vector<int> literalLengths(literalCount);
                            std::vector<int> distanceLengths(distanceCount);
                            for (std::size_t i = 0; i < literalCount; ++i)
                                literalLengths[i] = lengths[i];
                            for (std::size_t i = 0; i < distanceCount; ++i)
                                distanceLengths[i] = lengths[literalCount + i];
                            if (!ownLiterals.build(literalLengths.data(), literalCount))
                            {
                                error = "the literals of a dynamic block are not a usable code";
                                return false;
                            }
                            if (!ownDistances.build(distanceLengths.data(), distanceCount))
                            {
                                error = "the distances of a dynamic block are not a usable code";
                                return false;
                            }
                            literals = &ownLiterals;
                            distances = &ownDistances;
                        }

                        for (;;)
                        {
                            int symbol;
                            if (!literals->decode(source, symbol))
                            {
                                error = "the literals of a block do not decode";
                                return false;
                            }
                            if (symbol == 256)
                                break;
                            if (symbol < 256)
                            {
                                if (out.size() >= limit)
                                {
                                    error = "the stream inflates past the size it was read against";
                                    return false;
                                }
                                out.push_back(static_cast<char>(symbol));
                                continue;
                            }

                            const std::size_t which = static_cast<std::size_t>(symbol - 257);
                            if (which >= sizeof(LENGTH_EXTRA) / sizeof(LENGTH_EXTRA[0]))
                            {
                                error = "a length symbol past the ones the format carries";
                                return false;
                            }
                            unsigned extra;
                            if (!source.takeBits(LENGTH_EXTRA[which], extra))
                            {
                                error = "the stream ends inside a length";
                                return false;
                            }
                            const std::size_t length = LENGTH_START[which] + extra;

                            int distanceSymbol;
                            if (!distances->decode(source, distanceSymbol))
                            {
                                error = "the distance of a copy does not decode";
                                return false;
                            }
                            if (distanceSymbol < 0 ||
                                static_cast<std::size_t>(distanceSymbol) >=
                                    sizeof(DISTANCE_EXTRA) / sizeof(DISTANCE_EXTRA[0]))
                            {
                                error = "a distance symbol past the ones the format carries";
                                return false;
                            }
                            unsigned distanceExtra;
                            if (!source.takeBits(DISTANCE_EXTRA[static_cast<std::size_t>(distanceSymbol)],
                                                 distanceExtra))
                            {
                                error = "the stream ends inside a distance";
                                return false;
                            }
                            const std::size_t distance =
                                DISTANCE_START[static_cast<std::size_t>(distanceSymbol)] + distanceExtra;
                            if (distance == 0 || distance > out.size())
                            {
                                error = "a copy reaches before the bytes the stream has made";
                                return false;
                            }
                            if (length > limit - out.size())
                            {
                                error = "the stream inflates past the size it was read against";
                                return false;
                            }
                            const std::size_t from = out.size() - distance;
                            for (std::size_t i = 0; i < length; ++i)
                            {
                                // The byte is taken before the push: a copy that reaches back
                                // into the string it is filling must not hold a reference
                                // across the growth of that string.
                                const char copied = out[from + i];
                                out.push_back(copied);
                            }
                        }
                    }
                    else
                    {
                        error = "a block of a type the format does not carry";
                        return false;
                    }

                    if (last)
                    {
                        // Where this stream ends in the bytes it was read from: the reader
                        // has fetched nothing past the final block, so the trailer the
                        // format writes behind it starts at the next byte.
                        consumed = source.bytePosition();
                        return true;
                    }
                }
            }

            // The three claims a zlib wrapper's first two bytes make, and the shortest
            // length that can carry a wrapper at all.
            bool zlibHeaderRead(const std::string &stream, unsigned &first, unsigned &second,
                                std::string &error)
            {
                if (stream.size() < 7)
                {
                    error = "a zlib stream is a header, at least one block, and an adler32";
                    return false;
                }
                first = static_cast<unsigned char>(stream[0]);
                second = static_cast<unsigned char>(stream[1]);
                // The first byte is the window size in its high nibble and the compression
                // method in its low one: it is the method that must say deflate.
                if ((first & 0x0fu) != 8u)
                {
                    error = "the stream is not a deflate stream";
                    return false;
                }
                if ((((first << 8) | second) % 31u) != 0u)
                {
                    error = "the header of the stream is not one zlib writes";
                    return false;
                }
                if ((second & 0x20u) != 0u)
                {
                    error = "the stream asks for a preset dictionary that the stream does not carry";
                    return false;
                }
                return true;
            }

            // The adler32 the format writes big-endian at a stated place.
            std::uint32_t adlerAt(const std::string &stream, std::size_t at)
            {
                return (static_cast<std::uint32_t>(static_cast<unsigned char>(stream[at])) << 24) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(stream[at + 1])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(stream[at + 2])) << 8) |
                       static_cast<unsigned char>(stream[at + 3]);
            }

            bool zlibInflated(const std::string &stream, std::size_t limit, std::string &out,
                              std::string &error)
            {
                out.clear();
                unsigned first = 0;
                unsigned second = 0;
                if (!zlibHeaderRead(stream, first, second, error))
                    return false;
                // A whole stream: the four bytes behind the blocks are its adler32 and are
                // never block payload, so the reader is given the bytes between the header
                // and the trailer and nothing more.
                std::size_t unused = 0;
                if (!blocksDecoded(reinterpret_cast<const unsigned char *>(stream.data()) + 2,
                                   stream.size() - 6, limit, out, error, unused))
                    return false;

                const std::uint32_t stated = adlerAt(stream, stream.size() - 4);
                if (stated != adler32Of(out))
                {
                    error = "the adler32 the stream carries is not the one its bytes have";
                    return false;
                }
                return true;
            }

            struct BitSink
            {
                std::string &bytes;
                unsigned held;
                int bits;

                explicit BitSink(std::string &out) : bytes(out), held(0), bits(0) {}

                bool writeBit(unsigned bit)
                {
                    held |= (bit & 1u) << bits;
                    ++bits;
                    if (bits == 8)
                    {
                        bytes.push_back(static_cast<char>(held));
                        held = 0;
                        bits = 0;
                    }
                    return true;
                }

                bool writeBits(unsigned value, unsigned count)
                {
                    for (unsigned i = 0; i < count; ++i)
                        writeBit((value >> i) & 1u);
                    return true;
                }

                void padToByte()
                {
                    if (bits != 0)
                    {
                        bytes.push_back(static_cast<char>(held));
                        held = 0;
                        bits = 0;
                    }
                }
            };

            // The writer of the object layer: stored blocks only, which makes no claim
            // about what it compressed and therefore holds at every size. A block carries
            // at most 65535 bytes, so a large object is a run of them.
            bool storedWrapped(const std::string &raw, std::string &out, std::string &error)
            {
                out.clear();
                out.push_back('\x78');
                out.push_back('\x01'); // the wrapper git writes: deflate, no dictionary, easiest
                BitSink sink(out);
                const std::size_t STORED_MAX = 65535u;
                for (std::size_t at = 0;; at += STORED_MAX)
                {
                    const std::size_t left = raw.size() - at;
                    const std::size_t length = left < STORED_MAX ? left : STORED_MAX;
                    const bool last = (at + length) >= raw.size();
                    if (!sink.writeBit(last ? 1u : 0u) || !sink.writeBits(0u, 2u))
                    {
                        error = "a stored block cannot be started";
                        return false;
                    }
                    sink.padToByte();
                    out.push_back(static_cast<char>(length & 0xffu));
                    out.push_back(static_cast<char>(length >> 8));
                    out.push_back(static_cast<char>((~length) & 0xffu));
                    out.push_back(static_cast<char>((~length >> 8) & 0xffu));
                    out.append(raw, at, length);
                    if (last)
                        break;
                }
                sink.padToByte();
                const std::uint32_t check = adler32Of(raw);
                out.push_back(static_cast<char>(check >> 24));
                out.push_back(static_cast<char>(check >> 16));
                out.push_back(static_cast<char>(check >> 8));
                out.push_back(static_cast<char>(check));
                return true;
            }

            // ------------------------------------------------------- shared reading steps
            //
            // The header lines of a commit or a tag: everything before the first blank
            // line, split at newlines, with the lines that continue the one before them -
            // a leading space - folded back in before any line is read.
            bool headLines(const std::string &content, std::vector<std::string> &lines,
                           std::string &message, std::string &error)
            {
                const std::size_t blank = content.find("\n\n");
                if (blank == std::string::npos)
                {
                    error = "the content has no blank line, which is where a message starts";
                    return false;
                }
                const std::string head = content.substr(0, blank);
                message = content.substr(blank + 2);

                std::vector<std::string> raw;
                std::size_t at = 0;
                for (;;)
                {
                    const std::size_t next = head.find('\n', at);
                    if (next == std::string::npos)
                    {
                        raw.push_back(head.substr(at));
                        break;
                    }
                    raw.push_back(head.substr(at, next - at));
                    at = next + 1;
                }
                for (const std::string &line : raw)
                {
                    if (!line.empty() && line[0] == ' ' && !lines.empty())
                    {
                        // The fold is kept as the bytes have it: the newline and the space
                        // that opens the continuation are part of the value, so a header
                        // read here and written again gives the same bytes.
                        lines.back() += '\n';
                        lines.back() += line;
                        continue;
                    }
                    lines.push_back(line);
                }
                return true;
            }

            // The word of a line and the value after it.
            bool lineField(const std::string &line, std::string &word, std::string &value)
            {
                const std::size_t space = line.find(' ');
                if (space == std::string::npos || space == 0)
                    return false;
                word = line.substr(0, space);
                value = line.substr(space + 1);
                return true;
            }

            // The brackets split the line the way git splits it: the FIRST '<' and the
            // LAST '>' (git's ident_split: strchr for the one, strrchr for the other).
            // A name carrying a '<' therefore ends where the first bracket says, and
            // what follows stands as email — which is safe for everything this store
            // writes, because the persons this module reaches are its own authors and
            // they refuse '<': personWrite is only ever handed names that do not carry
            // one, and content that does is git's, read by git's rule.
            bool personRead(const std::string &text, Person &out, std::string &error)
            {
                const std::size_t open = text.find('<');
                const std::size_t close = text.rfind('>');
                if (open == std::string::npos || close == std::string::npos || close < open || open < 1 ||
                    text[open - 1] != ' ')
                {
                    error = "a person is a name, an email between angle brackets, a count of seconds, and a "
                            "zone";
                    return false;
                }
                out.name = text.substr(0, open - 1);
                out.email = text.substr(open + 1, close - open - 1);
                if (out.name.empty())
                {
                    error = "a person without a name is not a person";
                    return false;
                }
                const std::string tail = text.substr(close + 1);
                if (tail.size() < 2 || tail[0] != ' ')
                {
                    error = "a person is followed by a count of seconds and a zone";
                    return false;
                }
                const std::size_t space = tail.find(' ', 1);
                if (space == std::string::npos)
                {
                    error = "a zone follows the count of seconds";
                    return false;
                }
                const std::string seconds = tail.substr(1, space - 1);
                out.zone = tail.substr(space + 1);
                if (out.zone.size() != 5 || (out.zone[0] != '+' && out.zone[0] != '-'))
                {
                    error = "a zone is a sign and four digits";
                    return false;
                }
                for (std::size_t i = 1; i < out.zone.size(); ++i)
                {
                    if (out.zone[i] < '0' || out.zone[i] > '9')
                    {
                        error = "a zone is a sign and four digits";
                        return false;
                    }
                }
                if (seconds.empty())
                {
                    error = "a person is made at a counted second";
                    return false;
                }
                const bool negative = seconds[0] == '-';
                const std::size_t from = negative ? 1 : 0;
                if (from >= seconds.size())
                {
                    error = "a count of seconds is digits";
                    return false;
                }
                std::int64_t counted = 0;
                for (std::size_t i = from; i < seconds.size(); ++i)
                {
                    if (seconds[i] < '0' || seconds[i] > '9')
                    {
                        error = "a count of seconds is digits";
                        return false;
                    }
                    // A commit's author line is text from the wire or from disk: a stamp of
                    // thirty digits would run this count off the end of a signed number,
                    // and the walk off the end of a signed number is a crash, not a
                    // refusal. Past what fits, the line is refused.
                    constexpr std::int64_t kStampMax = (std::numeric_limits<std::int64_t>::max)();
                    const std::int64_t digit = seconds[i] - '0';
                    if (counted > (kStampMax - digit) / 10)
                    {
                        error = "a count of seconds is a count of seconds, not a number of any size";
                        return false;
                    }
                    counted = counted * 10 + digit;
                }
                out.seconds = negative ? -counted : counted;
                return true;
            }

            std::string personWrite(const Person &person)
            {
                return person.name + " <" + person.email + "> " + std::to_string(person.seconds) + " " +
                       person.zone;
            }

        } // anonymous namespace

        // --------------------------------------------------------- the public codec

        bool inflateZlib(const std::string &stream, std::string &out, std::string &error)
        {
            return zlibInflated(stream, (std::numeric_limits<std::size_t>::max)(), out, error);
        }

        bool inflateZlib(const std::string &stream, std::size_t limit, std::string &out,
                         std::string &error)
        {
            return zlibInflated(stream, limit, out, error);
        }

        bool inflateZlibSpan(const std::string &stream, std::size_t limit, std::string &out,
                             std::size_t &consumed, std::string &error)
        {
            out.clear();
            consumed = 0;
            unsigned first = 0;
            unsigned second = 0;
            if (!zlibHeaderRead(stream, first, second, error))
                return false;
            // One stream among other bytes — the shape of a record of a pack, with
            // another record behind it. The reader is given everything behind the
            // header, and where it leaves off is where the stream ends: the adler32 is
            // looked for there, not at the end of the bytes handed over, so the bytes
            // behind a stream are neither part of it nor its digest.
            std::size_t at = 0;
            if (!blocksDecoded(reinterpret_cast<const unsigned char *>(stream.data()) + 2,
                               stream.size() - 2, limit, out, error, at))
                return false;
            if (at + 6u > stream.size())
            {
                error = "the stream is cut off before its adler32";
                return false;
            }
            if (adlerAt(stream, at + 2u) != adler32Of(out))
            {
                error = "the adler32 the stream carries is not the one its bytes have";
                return false;
            }
            consumed = at + 6u;
            return true;
        }

        bool deflateStored(const std::string &raw, std::string &out, std::string &error)
        {
            return storedWrapped(raw, out, error);
        }

        // -------------------------------------------------------------- ids and hex

        ObjectId::ObjectId(std::string text)
        {
            hex.clear();
            for (const char c : text)
                hex.push_back(c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c);
            format = hex.size() == SHA256_BYTES * 2 ? ObjectFormat::Sha256 : ObjectFormat::Sha1;
        }

        bool ObjectId::parse(const std::string &text, ObjectId &out)
        {
            const ObjectFormat format =
                text.size() == SHA256_BYTES * 2 ? ObjectFormat::Sha256 : ObjectFormat::Sha1;
            std::string unused;
            return oidParseHex(text, format, out, unused);
        }

        const std::string &ObjectId::to_hex() const { return hex; }

        bool ObjectId::valid() const
        {
            if (hex.size() != oidWidth(format) * 2)
                return false;
            for (const char c : hex)
            {
                if (hexValue(c) < 0)
                    return false;
            }
            return true;
        }

        bool ObjectId::is_null() const
        {
            if (hex.empty())
                return true;
            if (!valid())
                return false;
            for (const char c : hex)
            {
                if (c != '0')
                    return false;
            }
            return true;
        }

        std::string oidHex(const ObjectId &id) { return id.hex; }

        bool oidParseHex(const std::string &hex, ObjectFormat format, ObjectId &out, std::string &error)
        {
            const std::size_t width = oidWidth(format);
            if (hex.size() != width * 2)
            {
                error = "an id is " + std::to_string(width * 2) + " hex digits, this is " +
                        std::to_string(hex.size());
                return false;
            }
            std::array<std::uint8_t, OID_STORE_BYTES> bytes{};
            for (std::size_t i = 0; i < width; ++i)
            {
                const int high = hexValue(hex[2 * i]);
                const int low = hexValue(hex[2 * i + 1]);
                if (high < 0 || low < 0)
                {
                    error = "an id is hex, and this is not";
                    return false;
                }
                bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
            }
            out.hex = toHex(bytes.data(), width);
            out.format = format;
            return true;
        }

        bool oidFromRaw(const std::uint8_t *bytes, std::size_t count, ObjectId &out, std::string &error)
        {
            if (count != SHA1_BYTES && count != SHA256_BYTES)
            {
                error = "the bytes of an id are twenty, or thirty-two for the wider format";
                return false;
            }
            out.hex = toHex(bytes, count);
            out.format = count == SHA1_BYTES ? ObjectFormat::Sha1 : ObjectFormat::Sha256;
            return true;
        }

        std::string oidRaw(const ObjectId &id)
        {
            if (!id.valid())
                return std::string();
            const std::size_t width = oidWidth(id.format);
            std::string bytes;
            bytes.reserve(width);
            for (std::size_t i = 0; i < width; ++i)
            {
                const int high = hexValue(id.hex[2 * i]);
                const int low = hexValue(id.hex[2 * i + 1]);
                bytes.push_back(static_cast<char>((high << 4) | low));
            }
            return bytes;
        }

        // Two ids of one format compare by their bytes, which for lowercase hex of one
        // length is the same as comparing the hex.
        int oidCompare(const ObjectId &a, const ObjectId &b)
        {
            if (a.format != b.format)
                return a.format < b.format ? -1 : 1;
            if (a.hex != b.hex)
                return a.hex < b.hex ? -1 : 1;
            return 0;
        }

        bool operator==(const ObjectId &a, const ObjectId &b) { return oidCompare(a, b) == 0; }
        bool operator!=(const ObjectId &a, const ObjectId &b) { return oidCompare(a, b) != 0; }
        bool operator<(const ObjectId &a, const ObjectId &b) { return oidCompare(a, b) < 0; }
        bool operator<=(const ObjectId &a, const ObjectId &b) { return oidCompare(a, b) <= 0; }

        std::string sha1Hex(const std::string &bytes)
        {
            const std::array<std::uint8_t, SHA1_BYTES> digest = sha1Digest(bytes);
            return toHex(digest.data(), digest.size());
        }

        std::string sha256Hex(const std::string &bytes)
        {
            const std::array<std::uint8_t, SHA256_BYTES> digest = sha256Digest(bytes);
            return toHex(digest.data(), digest.size());
        }

        bool objectOid(const std::string &raw, ObjectFormat format, ObjectId &out, std::string &error)
        {
            if (format == ObjectFormat::Sha256)
            {
                error = "the sha256 format is named and sized; its digest is not written yet";
                return false;
            }
            out.hex = sha1Hex(raw);
            out.format = format;
            return true;
        }

        // ------------------------------------------------------------------ headers

        const char *kindName(ObjectKind kind)
        {
            if (kind == ObjectKind::Blob)
                return "blob";
            if (kind == ObjectKind::Commit)
                return "commit";
            if (kind == ObjectKind::Tree)
                return "tree";
            return "tag";
        }

        bool kindOfName(const std::string &name, ObjectKind &out, std::string &error)
        {
            if (name == "blob")
                out = ObjectKind::Blob;
            else if (name == "commit")
                out = ObjectKind::Commit;
            else if (name == "tree")
                out = ObjectKind::Tree;
            else if (name == "tag")
                out = ObjectKind::Tag;
            else
            {
                error = "the type word of an object is one of blob, commit, tree, tag; this is neither";
                return false;
            }
            return true;
        }

        std::string objectHeaderMake(ObjectKind kind, std::uint64_t size)
        {
            return std::string(kindName(kind)) + " " + std::to_string(size) + std::string(1, '\0');
        }

        bool objectHeaderRead(const std::string &bytes, ObjectKind &kind, std::uint64_t &size,
                              std::string &error)
        {
            const std::size_t nul = bytes.find('\0');
            if (nul == std::string::npos)
            {
                error = "an object header ends at a NUL, and these bytes carry none";
                return false;
            }
            const std::string header = bytes.substr(0, nul);
            const std::size_t space = header.find(' ');
            if (space == std::string::npos)
            {
                error = "an object header is a type word, a space, and a size";
                return false;
            }
            if (header.find(' ', space + 1) != std::string::npos)
            {
                error = "an object header carries one space, between the type and the size";
                return false;
            }
            if (!kindOfName(header.substr(0, space), kind, error))
                return false;
            const std::string digits = header.substr(space + 1);
            if (digits.empty())
            {
                error = "an object header states the size of its content";
                return false;
            }
            if (digits[0] == '+')
            {
                error = "a size is written without a sign";
                return false;
            }
            if (digits.size() > 1 && digits[0] == '0')
            {
                error = "a size is written without a leading zero";
                return false;
            }
            std::uint64_t counted = 0;
            for (const char c : digits)
            {
                if (c < '0' || c > '9')
                {
                    error = "a size is decimal digits";
                    return false;
                }
                if (counted > (std::numeric_limits<std::uint64_t>::max)() / 10ull)
                {
                    error = "a size beyond a countable one is not an object";
                    return false;
                }
                counted = counted * 10ull + static_cast<std::uint64_t>(c - '0');
            }
            size = counted;
            return true;
        }

        bool objectSplit(const std::string &raw, ObjectKind &kind, std::uint64_t &size,
                         std::string &content, std::string &error)
        {
            if (!objectHeaderRead(raw, kind, size, error))
                return false;
            const std::size_t nul = raw.find('\0');
            const std::uint64_t following = static_cast<std::uint64_t>(raw.size() - nul - 1);
            if (following != size)
            {
                error = "the size an object states is not the count of bytes behind its header: " +
                        std::to_string(size) + " against " + std::to_string(following);
                return false;
            }
            content = raw.substr(nul + 1);
            return true;
        }

        bool objectBytes(ObjectKind kind, const std::string &content, std::string &raw, std::string &error)
        {
            raw = objectHeaderMake(kind, content.size());
            raw.append(content);
            error.clear();
            return true;
        }

        // --------------------------------------------------------------- the loose file

        namespace
        {

// The path the file API is handed — the same qualification refs.cpp makes for
// every ref file (refs.cpp's ext_path/open_name block), reached here too:
// past 260 characters the plain narrow name on Windows is refused by the API
// before it looks at the disk, and an objects/ folder under a few nested
// folders passes that line with room to spare. A loose object that cannot be
// read past it is a missing object, and a missing object the next receive
// writes over without knowing it. Every path here that reaches a file API —
// the ifstream's, the ofstream's, and the std::filesystem calls beside them —
// goes in the extended form: absolute, backslashes only, under the \\?\
// prefix that carries no length limit and takes the name exactly as it
// stands. On POSIX the path is the name.
#ifdef _WIN32
            std::filesystem::path ext_path(const std::string &s)
            {
                const std::filesystem::path p = std::filesystem::u8path(s);
                std::error_code ec;
                std::filesystem::path full = std::filesystem::absolute(p, ec).lexically_normal();
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
                    return std::filesystem::path(std::wstring(L"\\\\?\\UNC\\") + w.substr(2u)); // a share keeps its server
                }
                return std::filesystem::path(std::wstring(L"\\\\?\\") + w);
            }

            std::wstring open_name(const std::string &s) { return ext_path(s).wstring(); }
#else
            std::filesystem::path ext_path(const std::string &s) { return std::filesystem::u8path(s); }
            const std::string &open_name(const std::string &s) { return s; }
#endif

            // Orphan staging files: a write that died between its temp and its rename
            // leaves a "<hex>.writingN" beside the object it was reaching (and, since
            // temps carry the process id too, a "<hex>.writing<pid>.<n>"). Every write
            // sweeps the siblings of the folder it is filing into, best effort and to a
            // bound: at most kOrphanSweepLimit directory entries are examined per write,
            // so a fanout holding thousands of objects cannot turn a write into a walk;
            // of those, a file is removed only when its name is staging of this writer —
            // ".writing" opening onto a run of digits and dots, which no object's own
            // hex name ever is — and it stands older than kOrphanTempAge. The age is the
            // live writer's half of the bound: a temp that young is someone's in-flight
            // write, not garbage. A removal that catches a writer somehow running past
            // the age costs that write a failed rename — a returned false, which is how
            // every refusal here travels — and nothing else.
            constexpr std::chrono::minutes kOrphanTempAge{10};
            constexpr std::size_t kOrphanSweepLimit = 64;

            void sweep_orphan_temps(const std::filesystem::path &fanout)
            {
                std::error_code ec;
                std::size_t examined = 0;
                for (std::filesystem::directory_iterator it(fanout, ec), end; !ec && it != end;
                     it.increment(ec))
                {
                    if (examined >= kOrphanSweepLimit)
                    {
                        break; // the bound: this write has asked the folder enough
                    }
                    ++examined;
                    const std::string name = it->path().filename().u8string();
                    const std::size_t at = name.find(".writing");
                    if (at == std::string::npos || at + 8u >= name.size())
                    {
                        continue;
                    }
                    bool staging = true;
                    for (std::size_t i = at + 8u; i < name.size(); ++i)
                    {
                        const char c = name[i];
                        if (!((c >= '0' && c <= '9') || c == '.'))
                        {
                            staging = false;
                            break;
                        }
                    }
                    if (!staging)
                    {
                        continue; // a name that wears the word as text is not this writer's staging
                    }
                    std::error_code tec;
                    const auto stamp = std::filesystem::last_write_time(it->path(), tec);
                    if (tec)
                    {
                        continue; // a stamp that cannot be read says in-flight, not dead
                    }
                    if (std::filesystem::file_time_type::clock::now() - stamp < kOrphanTempAge)
                    {
                        continue;
                    }
                    std::error_code removed;
                    std::filesystem::remove(it->path(), removed); // best effort, whole
                }
            }

        } // anonymous namespace of the loose-file paths

        bool looseObjectPath(const std::string &objectsDir, const ObjectId &id, std::string &path)
        {
            if (objectsDir.empty())
                return false;
            const std::string hex = oidHex(id);
            path = objectsDir;
            if (path.back() != '/')
                path.push_back('/');
            path += hex.substr(0, 2);
            path += '/';
            path += hex.substr(2);
            return true;
        }

        bool looseObjectRead(const std::string &path, LooseObject &out, std::string &error)
        {
            out = LooseObject{};
            std::ifstream file(open_name(path), std::ios::binary);
            if (!file.good())
            {
                error = "the loose object at " + path + " is not there";
                return false;
            }
            const std::string stream((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
            if (!file.good())
            {
                error = "the loose object at " + path + " could not be read to its end";
                return false;
            }

            std::string inflated;
            if (!zlibInflated(stream, (std::numeric_limits<std::size_t>::max)(), inflated, error))
                return false;
            if (!objectSplit(inflated, out.kind, out.size, out.content, error))
                return false;
            if (!objectOid(inflated, ObjectFormat::Sha1, out.id, error))
                return false;
            out.raw = inflated;
            return true;
        }

        bool looseObjectReadAt(const std::string &objectsDir, const ObjectId &id, LooseObject &out,
                               std::string &error)
        {
            std::string path;
            if (!looseObjectPath(objectsDir, id, path))
            {
                error = "an id with no folder to be read from has no path";
                return false;
            }
            if (!looseObjectRead(path, out, error))
                return false;
            if (!(out.id == id))
            {
                error = "the bytes filed at " + path + " do not hash to the name they stand under";
                return false;
            }
            return true;
        }

        bool looseObjectWrite(const std::string &objectsDir, ObjectKind kind,
                              const std::string &content, ObjectId &id, std::string &error)
        {
            std::string raw;
            objectBytes(kind, content, raw, error);
            if (!objectOid(raw, ObjectFormat::Sha1, id, error))
                return false;
            std::string path;
            if (!looseObjectPath(objectsDir, id, path))
            {
                error = "an id with no folder to be written to has no path";
                return false;
            }

            std::error_code made{};
            const std::filesystem::path qualified = ext_path(path);
            std::filesystem::create_directories(qualified.parent_path(), made);
            if (made)
            {
                error = "the folder an object is filed under could not be made: " + made.message();
                return false;
            }

            // The bounded sibling sweep (see kOrphanSweepLimit above): a write that
            // died mid-flight left its staging name here, and this is the step that
            // takes it out — before the exists shortcut, so even a re-write of a
            // standing object tidies the folder it lands in.
            sweep_orphan_temps(qualified.parent_path());

            std::error_code found{};
            if (std::filesystem::exists(qualified, found) && !found)
                return true; // the object the bytes hash to is already standing there

            std::string packed;
            if (!storedWrapped(raw, packed, error))
                return false;

            // Beside the file and under a name it will not collide with, then renamed
            // in: the settled name never holds half an object, the way the records of
            // the server are written. The process id rides the name so two processes
            // reaching the same object at once hold two staging files, never one;
            // the per-process counter keeps this process's own writes apart.
            static std::atomic<unsigned> writing{0};
#ifdef _WIN32
            const unsigned hostPid = static_cast<unsigned>(::GetCurrentProcessId());
#else
            const unsigned hostPid = static_cast<unsigned>(::getpid());
#endif
            std::ostringstream temp;
            temp << path << ".writing" << hostPid << '.' << ++writing;
            const std::string tempName = temp.str();
            {
                std::ofstream out(open_name(tempName), std::ios::binary);
                if (!out.good())
                {
                    error = "a temporary file beside " + path + " could not be opened";
                    return false;
                }
                out << packed;
                out.flush();
                if (!(out.good() || out.eof()))
                {
                    error = "the object could not be written";
                    std::error_code removed{};
                    std::filesystem::remove(ext_path(tempName), removed);
                    return false;
                }
            }
            std::error_code moved{};
            std::filesystem::rename(ext_path(tempName), qualified, moved);
            if (moved)
            {
                error = "the object could not be put where its hash says: " + moved.message();
                std::error_code dropped{};
                std::filesystem::remove(ext_path(tempName), dropped);
                return false;
            }
            return true;
        }

        // ------------------------------------------------------- the typed contents

        bool commitParse(const std::string &content, CommitObject &out, std::string &error)
        {
            out = CommitObject{};
            std::vector<std::string> lines;
            if (!headLines(content, lines, out.message, error))
                return false;
            if (lines.empty())
            {
                error = "a commit is headers, a blank line, and a message";
                return false;
            }

            std::size_t index = 0;
            {
                std::string word, value;
                if (!lineField(lines[index], word, value) || word != "tree")
                {
                    error = "a commit starts with the tree it holds";
                    return false;
                }
                if (!oidParseHex(value, ObjectFormat::Sha1, out.tree, error))
                    return false;
                ++index;
            }

            while (index < lines.size())
            {
                std::string word, value;
                if (!lineField(lines[index], word, value) || word != "parent")
                    break;
                ObjectId parent;
                if (!oidParseHex(value, ObjectFormat::Sha1, parent, error))
                    return false;
                out.parents.push_back(parent);
                ++index;
            }

            {
                std::string word, value;
                if (index >= lines.size() || !lineField(lines[index], word, value) || word != "author")
                {
                    error = "a commit names its author after the parents it carries";
                    return false;
                }
                if (!personRead(value, out.author, error))
                    return false;
                ++index;
            }
            {
                std::string word, value;
                if (index >= lines.size() || !lineField(lines[index], word, value) ||
                    word != "committer")
                {
                    error = "a commit names its committer after its author";
                    return false;
                }
                if (!personRead(value, out.committer, error))
                    return false;
                ++index;
            }

            for (; index < lines.size(); ++index)
            {
                std::string word, value;
                if (!lineField(lines[index], word, value))
                {
                    error = "a header after the committer is a word and a value";
                    return false;
                }
                out.headers.emplace_back(word, value);
            }
            return true;
        }

        bool commitSerialize(const CommitObject &in, std::string &content, std::string &error)
        {
            if (in.tree.format != ObjectFormat::Sha1)
            {
                error = "the id of a commit's tree is in a format the repository is in";
                return false;
            }
            for (const ObjectId &parent : in.parents)
            {
                if (parent.format != ObjectFormat::Sha1)
                {
                    error = "the id of a commit's parent is in a format the repository is in";
                    return false;
                }
            }
            if (in.author.name.empty() || in.committer.name.empty())
            {
                error = "a commit names both an author and a committer";
                return false;
            }

            content.clear();
            content += "tree " + oidHex(in.tree) + "\n";
            for (const ObjectId &parent : in.parents)
                content += "parent " + oidHex(parent) + "\n";
            content += "author " + personWrite(in.author) + "\n";
            content += "committer " + personWrite(in.committer) + "\n";
            for (const auto &extra : in.headers)
            {
                if (extra.first.empty() || extra.first.find('\n') != std::string::npos)
                {
                    error = "a header carried by a commit has a name and a body";
                    return false;
                }
                content += extra.first + ' ';
                const std::string &body = extra.second;
                for (std::size_t at = 0; at < body.size(); ++at)
                {
                    if (body[at] == '\n' && (at + 1 >= body.size() || body[at + 1] != ' '))
                    {
                        error = "a header carried by a commit continues a line with a leading space";
                        return false;
                    }
                }
                content += body;
                content += '\n';
            }
            content += '\n';
            content += in.message;
            return true;
        }

        bool treeModeValid(const std::string &mode)
        {
            return mode == "100644" || mode == "100755" || mode == "120000" || mode == "40000";
        }

        bool treeModeDirectory(const std::string &mode) { return mode == "40000"; }

        // The names are compared as bytes, and a directory as its name with a slash
        // after it - the reason "a.txt" is before a directory called "a" and after a
        // file called "a.txt.x".
        int treeEntryCompare(const TreeEntry &a, const TreeEntry &b)
        {
            const std::string left = a.name + (treeModeDirectory(a.mode) ? "/" : std::string());
            const std::string right = b.name + (treeModeDirectory(b.mode) ? "/" : std::string());
            if (left != right)
                return left < right ? -1 : 1;
            return oidCompare(a.id, b.id);
        }

        bool treeParse(const std::string &content, TreeObject &out, std::string &error)
        {
            out = TreeObject{};
            const std::size_t width = oidWidth(ObjectFormat::Sha1);
            std::size_t at = 0;
            while (at < content.size())
            {
                const std::size_t nul = content.find('\0', at);
                if (nul == std::string::npos)
                {
                    error = "a tree entry is a mode, a space, a name, a NUL, and an id";
                    return false;
                }
                std::string word, value;
                if (!lineField(content.substr(at, nul - at), word, value))
                {
                    error = "a tree entry is a mode, a space, a name, a NUL, and an id";
                    return false;
                }
                TreeEntry entry;
                entry.mode = word;
                entry.name = value;
                if (!treeModeValid(entry.mode))
                {
                    error = "a tree entry carries one of the modes git writes: 100644, 100755, 120000, "
                            "40000";
                    return false;
                }
                if (entry.name.empty() || entry.name.find('/') != std::string::npos)
                {
                    error = "a tree entry names a thing in one folder, not a path through several";
                    return false;
                }
                if (nul + 1 + width > content.size())
                {
                    error = "a tree entry is followed by a whole id";
                    return false;
                }
                if (!oidFromRaw(reinterpret_cast<const std::uint8_t *>(content.data() + nul + 1), width,
                                entry.id, error))
                    return false;
                if (!out.entries.empty())
                {
                    const int order = treeEntryCompare(out.entries.back(), entry);
                    if (order == 0)
                    {
                        error = "a tree carries one entry per name";
                        return false;
                    }
                    if (order > 0)
                    {
                        error = "a tree is not kept in the order git keeps it: byte order of the names, "
                                "with a directory compared as if a slash followed its name";
                        return false;
                    }
                }
                out.entries.push_back(entry);
                at = nul + 1 + width;
            }
            return true;
        }

        bool treeSerialize(const TreeObject &in, std::string &content, std::string &error)
        {
            content.clear();
            for (std::size_t i = 0; i < in.entries.size(); ++i)
            {
                const TreeEntry &entry = in.entries[i];
                if (!treeModeValid(entry.mode))
                {
                    error = "a tree entry carries one of the modes git writes";
                    return false;
                }
                if (entry.name.empty() || entry.name.find('/') != std::string::npos ||
                    entry.name.find('\0') != std::string::npos)
                {
                    error = "a tree entry names a thing in one folder";
                    return false;
                }
                if (entry.id.format != ObjectFormat::Sha1 || !entry.id.valid())
                {
                    error = "the id of a tree entry is in a format the repository is in";
                    return false;
                }
                if (i > 0)
                {
                    const int order = treeEntryCompare(in.entries[i - 1], entry);
                    if (order == 0)
                    {
                        error = "a tree carries one entry per name";
                        return false;
                    }
                    if (order > 0)
                    {
                        error = "a tree is not kept in the order git keeps it";
                        return false;
                    }
                }
                content += entry.mode;
                content += ' ';
                content += entry.name;
                content += '\0';
                content += oidRaw(entry.id);
            }
            return true;
        }

        bool tagParse(const std::string &content, TagObject &out, std::string &error)
        {
            out = TagObject{};
            std::vector<std::string> lines;
            if (!headLines(content, lines, out.message, error))
                return false;
            if (lines.empty())
            {
                error = "a tag is headers, a blank line, and its message";
                return false;
            }

            std::size_t index = 0;
            {
                std::string word, value;
                if (!lineField(lines[index], word, value) || word != "object")
                {
                    error = "a tag names the object it stands for";
                    return false;
                }
                if (!oidParseHex(value, ObjectFormat::Sha1, out.target, error))
                    return false;
                ++index;
            }
            {
                std::string word, value;
                if (index >= lines.size() || !lineField(lines[index], word, value) || word != "type")
                {
                    error = "a tag states the type of the object it stands for";
                    return false;
                }
                if (!kindOfName(value, out.targetKind, error))
                    return false;
                ++index;
            }
            if (index < lines.size())
            {
                std::string word, value;
                if (lineField(lines[index], word, value) && word == "tag")
                {
                    out.name = value;
                    ++index;
                }
            }
            if (index < lines.size())
            {
                std::string word, value;
                if (lineField(lines[index], word, value) && word == "tagger")
                {
                    if (!personRead(value, out.tagger, error))
                        return false;
                    ++index;
                }
            }
            for (; index < lines.size(); ++index)
            {
                std::string word, value;
                if (!lineField(lines[index], word, value))
                {
                    error = "a header of a tag is a word and a value";
                    return false;
                }
                out.headers.emplace_back(word, value);
            }
            return true;
        }

        bool tagSerialize(const TagObject &in, std::string &content, std::string &error)
        {
            if (in.target.format != ObjectFormat::Sha1)
            {
                error = "the id a tag stands for is in a format the repository is in";
                return false;
            }
            if (in.name.find('\n') != std::string::npos)
            {
                error = "the name a tag carries is one line";
                return false;
            }
            content.clear();
            content += "object " + oidHex(in.target) + "\n";
            content += std::string("type ") + kindName(in.targetKind) + "\n";
            if (!in.name.empty())
                content += "tag " + in.name + "\n";
            if (!in.tagger.name.empty())
                content += "tagger " + personWrite(in.tagger) + "\n";
            for (const auto &extra : in.headers)
            {
                if (extra.first.empty() || extra.first.find('\n') != std::string::npos)
                {
                    error = "a header of a tag has a name and a body";
                    return false;
                }
                content += extra.first + ' ';
                const std::string &body = extra.second;
                for (std::size_t at = 0; at < body.size(); ++at)
                {
                    if (body[at] == '\n' && (at + 1 >= body.size() || body[at + 1] != ' '))
                    {
                        error = "a header of a tag continues a line with a leading space";
                        return false;
                    }
                }
                content += body;
                content += '\n';
            }
            content += '\n';
            content += in.message;
            return true;
        }

    } // namespace gitcore
} // namespace ols

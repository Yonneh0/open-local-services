#pragma once
// The verification records of one weights room.
//
// A model's file counts as present and checked only on evidence about its own
// bytes, never on the moment those bytes were last touched: a file that came
// over the link, was copied again, or was written by another machine keeps its
// content and loses any claim its dates might have made. So a record holds the
// file's length and the sums of its fixed chunks, and a machine that already
// holds those sums never reads the file again until something about it changes.
//
// The two questions this room answers are kept apart because they cost wildly
// different things:
//   proved()  reads nothing. It says whether a row stands for these bytes as
//             they are right now - which is what a page that lists a dozen
//             models asks every few seconds.
//   prove()   reads the file once and writes down what it saw - which is what a
//             job that is about to open a model, or a download that has just
//             laid a file down, asks.
// Nothing here reads a weight to answer a page. A room of a hundred-plus
// gigabytes is read once per file, and every answer after that is a lookup in a
// file of a few kilobytes.
//
// Two kinds of row stand in one records file, and both mean the same thing -
// what this machine saw about these bytes:
//   a proved row   {"bytes":N,"chunk":C,"whole":<sum>,"parts":[<sum per chunk>],"touched":<date>,"vouch":<what stands for it>}
//   an unpaid row  {"bytes":N,"unverified":<date>} - these bytes were read and
//                  nothing vouched for them, so saying "not proved" costs no
//                  further reading until they change.
// A row's date is only ever a short cut for "these bytes have not been touched
// since they were read"; it is never the answer on its own.
#ifndef OLS_VERIFIED_ROOM_H
#define OLS_VERIFIED_ROOM_H

#include <string>

namespace ols
{
  namespace room
  {
    // The one word for the voucher a format leaves its reader when it states no
    // sum of its own: the file's own header says its tensors are all there. It is
    // a weaker word than a card's sum, and it is stated in the row like any other
    // voucher, so a reader can see which of the two a file stands on.
    const char *kVoucherWhole();

    class VerifiedRoom
    {
    public:
      explicit VerifiedRoom(const std::string &root);

      // Is this file here, and do its bytes stand proved as they are right now?
      // Reads the records and the file's own length, never its content and
      // never its date - a date moved by a copy says nothing about bytes.
      bool proved(const std::string &path);

      // Read this file once and write down what was seen; the answer is whether
      // these bytes stand proved when it returns. What vouches for them is the
      // sum a card states - which the bytes must answer to - or kVoucherWhole()
      // when the caller has itself settled the file is whole, or nothing at all,
      // which records the rows and leaves them unproved. Bytes that answer to no
      // stated sum are written unpaid, so the next answer about the same bytes
      // costs no reading. The whole-file sum is handed back when asked for, so a
      // caller that has to compare it reads the file no more than once.
      bool prove(const std::string &path, const std::string &voucher = std::string(),
                 std::string *wholeHex = nullptr);

      // Stand a voucher under the rows already written for a file, once the
      // caller has settled what no sum could: a card that states no sum, and a
      // header that says the file is whole. Only bytes standing as they were
      // read can be vouched afterwards.
      bool vouch(const std::string &path, const std::string &voucher);

    private:
      std::string m_root;
    };

    // One shared room per weights root, so a module that works on a weights room
    // asks the same records every time and never keeps a copy of its own.
    VerifiedRoom &roomFor(const std::string &root);
  } // namespace room
} // namespace ols

#endif // OLS_VERIFIED_ROOM_H

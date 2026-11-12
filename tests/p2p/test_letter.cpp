// Letters open only for their recipient and purpose, carry their sender's key, refuse changed,
// foreign-version and oversized text, and mailboxes are 64 lowercase hex digits.
#include "lucent/p2p/letter.h"

#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>

int main() {
  using namespace lucent::p2p;
  const crypto::KeyPair host;
  const crypto::KeyPair guest;
  const crypto::KeyPair stranger;

  const std::string letter = write_letter(guest, host.public_key(), LetterPurpose::Ask, "room");
  const auto opened = read_letter(host, letter, LetterPurpose::Ask);
  assert(opened && opened->plain == "room" && crypto::same_bytes(opened->from, guest.public_key()));
  assert(!read_letter(host, letter, LetterPurpose::Answer));
  assert(!read_letter(stranger, letter, LetterPurpose::Ask));
  assert(!read_letter(guest, letter, LetterPurpose::Ask));
  // Two letters with the same text differ: each has its own nonce.
  assert(write_letter(guest, host.public_key(), LetterPurpose::Ask, "room") != letter);

  std::string changed = letter;
  changed.back() = changed.back() == '0' ? '1' : '0';
  assert(!read_letter(host, changed, LetterPurpose::Ask));
  std::string version = letter;
  version[1] = '2';
  assert(!read_letter(host, version, LetterPurpose::Ask));
  // A letter claiming the stranger as sender does not open under the guest's key.
  std::string claimed = letter;
  claimed.replace(2, 64, mailbox_of(stranger.public_key()));
  assert(!read_letter(host, claimed, LetterPurpose::Ask));
  assert(!read_letter(host, std::string(kMaxLetterTextBytes + 2, '0'), LetterPurpose::Ask));
  assert(!read_letter(host, "01", LetterPurpose::Ask));

  const std::string longest(kMaxLetterPlainBytes, 'x');
  const std::string full = write_letter(guest, host.public_key(), LetterPurpose::Answer, longest);
  assert(full.size() == kMaxLetterTextBytes);
  const auto full_opened = read_letter(host, full, LetterPurpose::Answer);
  assert(full_opened && full_opened->plain == longest);
  bool thrown = false;
  try {
    write_letter(guest, host.public_key(), LetterPurpose::Answer, longest + "x");
  } catch (const std::invalid_argument &) {
    thrown = true;
  }
  assert(thrown);

  assert(valid_mailbox(mailbox_of(host.public_key())));
  assert(!valid_mailbox(std::string(64, 'A')));
  assert(!valid_mailbox(std::string(63, 'a')));
  std::puts("letters open only for their recipient and purpose, and refuse changed, foreign and "
            "oversized text");
  return 0;
}

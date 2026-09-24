#pragma once

#include <string>

namespace ql
{

    // Checks that `text` is an OpenSSH public key line -- "<type> <key data>
    // [comment]", as found in a ~/.ssh/*.pub file -- before it's saved as an
    // SSH user's key (see AddUserFromForm), so a mistake is caught while the
    // admin is still at the form rather than showing up later as a login
    // that just fails. Recognizes the usual mistakes (a private key, a
    // PuTTY-format key, a missing type, a line cut short while copying) and
    // explains each. On success, sets `normalized` to the line with its
    // whitespace tidied (surrounding space trimmed, one space between
    // fields) and returns true; otherwise sets `error` and returns false.
    // Only checks the key's format -- it doesn't need libssh, so it works
    // the same in every build.
    bool ValidatePublicKey(const std::string& text, std::string* normalized, std::string* error);

}  // namespace ql

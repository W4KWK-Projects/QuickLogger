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

    // How a key is told apart from others, the way `ssh-keygen -l` shows it:
    // its type ("ED25519", "RSA", "ECDSA"...), its SHA-256 fingerprint
    // ("SHA256:zSpp/AdO..."), and the comment at the end of its line
    // ("wes@laptop", often the only thing a person recognizes). Takes a line
    // ValidatePublicKey accepted; for anything else the fingerprint is blank.
    struct PublicKeyDescription
    {
        std::string type;
        std::string fingerprint;
        std::string comment;
    };
    PublicKeyDescription DescribePublicKey(const std::string& line);

    // True if two key lines hold the same key -- the same type and key data,
    // whatever their comments say.
    bool SamePublicKey(const std::string& a, const std::string& b);

}  // namespace ql

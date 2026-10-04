#include "public_key.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <vector>

namespace ql
{

    // What a correct key line looks like, appended to every error.
    static const char* kExample =
        " It should be the single line from your .pub file, like: ssh-ed25519 AAAAC3NzaC1lZDI1NTE5"
        "AAAA... you@laptop";

    // The key types OpenSSH writes to a .pub file.
    static bool IsKnownKeyType(const std::string& type)
    {
        return type == "ssh-ed25519" || type == "ssh-rsa" || type == "ecdsa-sha2-nistp256" ||
               type == "ecdsa-sha2-nistp384" || type == "ecdsa-sha2-nistp521" || type == "sk-ssh-ed25519@openssh.com" ||
               type == "sk-ecdsa-sha2-nistp256@openssh.com";
    }

    static int Base64Value(char c)
    {
        if (c >= 'A' && c <= 'Z')
        {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z')
        {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9')
        {
            return c - '0' + 52;
        }
        if (c == '+')
        {
            return 62;
        }
        if (c == '/')
        {
            return 63;
        }
        return -1;
    }

    // Strict base64 decoding: a whole number of 4-character groups, '='
    // padding only at the very end. False on anything else.
    static bool DecodeBase64(const std::string& text, std::vector<unsigned char>* out)
    {
        if (text.empty() || text.size() % 4 != 0)
        {
            return false;
        }
        out->clear();
        for (std::size_t i = 0; i < text.size(); i += 4)
        {
            bool last_group = i + 4 == text.size();
            int values[4];
            int padding = 0;
            for (int j = 0; j < 4; ++j)
            {
                char c = text[i + static_cast<std::size_t>(j)];
                if (c == '=' && last_group && j >= 2)
                {
                    values[j] = 0;
                    ++padding;
                    continue;
                }
                if (padding > 0)
                {
                    return false;  // Data after padding.
                }
                values[j] = Base64Value(c);
                if (values[j] < 0)
                {
                    return false;
                }
            }
            std::uint32_t group = (static_cast<std::uint32_t>(values[0]) << 18) |
                                  (static_cast<std::uint32_t>(values[1]) << 12) |
                                  (static_cast<std::uint32_t>(values[2]) << 6) | static_cast<std::uint32_t>(values[3]);
            out->push_back(static_cast<unsigned char>(group >> 16));
            if (padding < 2)
            {
                out->push_back(static_cast<unsigned char>((group >> 8) & 0xFF));
            }
            if (padding < 1)
            {
                out->push_back(static_cast<unsigned char>(group & 0xFF));
            }
        }
        return true;
    }

    // The decoded key is a run of length-prefixed fields, the first naming
    // the key type. True if it's exactly that -- every field whole, nothing
    // left over (which is what a line cut short while copying fails) -- with
    // the first field equal to `type` and at least one field after it.
    static bool KeyDataMatchesType(const std::vector<unsigned char>& data, const std::string& type)
    {
        std::size_t position = 0;
        int fields = 0;
        while (position < data.size())
        {
            if (data.size() - position < 4)
            {
                return false;
            }
            std::size_t length = (static_cast<std::size_t>(data[position]) << 24) |
                                 (static_cast<std::size_t>(data[position + 1]) << 16) |
                                 (static_cast<std::size_t>(data[position + 2]) << 8) |
                                 static_cast<std::size_t>(data[position + 3]);
            position += 4;
            if (length > data.size() - position)
            {
                return false;
            }
            if (fields == 0 && std::string(data.begin() + static_cast<std::ptrdiff_t>(position),
                                           data.begin() + static_cast<std::ptrdiff_t>(position + length)) != type)
            {
                return false;
            }
            position += length;
            ++fields;
        }
        return fields >= 2;
    }

    bool ValidatePublicKey(const std::string& text, std::string* normalized, std::string* error)
    {
        std::istringstream stream(text);
        std::vector<std::string> words;
        std::string word;
        while (stream >> word)
        {
            words.push_back(word);
        }

        if (words.empty())
        {
            *error = std::string("Paste the user's public key.") + kExample;
            return false;
        }
        if (text.find("PRIVATE KEY") != std::string::npos)
        {
            *error =
                "That's a private key -- never share it. Paste the matching public key instead "
                "(the file ending in .pub, e.g. ~/.ssh/id_ed25519.pub).";
            return false;
        }
        if (text.find("BEGIN SSH2 PUBLIC KEY") != std::string::npos)
        {
            *error =
                "That's a PuTTY/SSH2-format key. Convert it with: ssh-keygen -i -f key.pub (or "
                "in PuTTYgen, copy the box labeled \"Public key for pasting into OpenSSH "
                "authorized_keys file\").";
            return false;
        }

        const std::string& type = words[0];
        if (type.compare(0, 4, "AAAA") == 0)
        {
            *error = std::string("The key type is missing from the start of the line.") + kExample;
            return false;
        }
        if (!IsKnownKeyType(type))
        {
            *error = "\"" + type +
                     "\" isn't an SSH key type (expected ssh-ed25519, ssh-rsa or "
                     "ecdsa-sha2-nistp256/384/521)." +
                     kExample;
            return false;
        }
        if (words.size() < 2)
        {
            *error = std::string("The key data after \"") + type + "\" is missing." + kExample;
            return false;
        }
        std::vector<unsigned char> data;
        if (!DecodeBase64(words[1], &data) || !KeyDataMatchesType(data, type))
        {
            *error = std::string("The key data after \"") + type +
                     "\" isn't valid -- it may have been cut off or changed while copying." + kExample;
            return false;
        }

        *normalized = type + " " + words[1];
        for (std::size_t i = 2; i < words.size(); ++i)
        {
            *normalized += " " + words[i];
        }
        return true;
    }

    static std::uint32_t RotateRight(std::uint32_t value, int bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }

    // SHA-256 (FIPS 180-4), for key fingerprints. Written out here rather
    // than taken from libssh so fingerprints show in every build, including
    // one without the SSH server.
    static std::vector<unsigned char> Sha256(const std::vector<unsigned char>& data)
    {
        static const std::uint32_t kRoundConstants[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };
        std::uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

        // The message, a 1 bit, zeros to 56 bytes into a 64-byte block, then
        // its length in bits.
        std::vector<unsigned char> message = data;
        std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
        message.push_back(0x80);
        while (message.size() % 64 != 56)
        {
            message.push_back(0);
        }
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            message.push_back(static_cast<unsigned char>(bit_length >> shift));
        }

        for (std::size_t block = 0; block < message.size(); block += 64)
        {
            std::uint32_t words[64];
            for (int i = 0; i < 16; ++i)
            {
                std::size_t at = block + static_cast<std::size_t>(i) * 4;
                words[i] = (static_cast<std::uint32_t>(message[at]) << 24) |
                           (static_cast<std::uint32_t>(message[at + 1]) << 16) |
                           (static_cast<std::uint32_t>(message[at + 2]) << 8) |
                           static_cast<std::uint32_t>(message[at + 3]);
            }
            for (int i = 16; i < 64; ++i)
            {
                std::uint32_t s0 =
                    RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
                std::uint32_t s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
                words[i] = words[i - 16] + s0 + words[i - 7] + s1;
            }

            std::uint32_t a = hash[0];
            std::uint32_t b = hash[1];
            std::uint32_t c = hash[2];
            std::uint32_t d = hash[3];
            std::uint32_t e = hash[4];
            std::uint32_t f = hash[5];
            std::uint32_t g = hash[6];
            std::uint32_t h = hash[7];
            for (int i = 0; i < 64; ++i)
            {
                std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                std::uint32_t choose = (e & f) ^ (~e & g);
                std::uint32_t temp1 = h + s1 + choose + kRoundConstants[i] + words[i];
                std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
                std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                std::uint32_t temp2 = s0 + majority;
                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }
            hash[0] += a;
            hash[1] += b;
            hash[2] += c;
            hash[3] += d;
            hash[4] += e;
            hash[5] += f;
            hash[6] += g;
            hash[7] += h;
        }

        std::vector<unsigned char> digest;
        for (std::uint32_t word : hash)
        {
            digest.push_back(static_cast<unsigned char>(word >> 24));
            digest.push_back(static_cast<unsigned char>(word >> 16));
            digest.push_back(static_cast<unsigned char>(word >> 8));
            digest.push_back(static_cast<unsigned char>(word));
        }
        return digest;
    }

    // Base64 without the '=' padding, as OpenSSH writes fingerprints.
    static std::string EncodeBase64Unpadded(const std::vector<unsigned char>& data)
    {
        static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (std::size_t i = 0; i < data.size(); i += 3)
        {
            std::size_t remaining = data.size() - i;
            std::uint32_t group = static_cast<std::uint32_t>(data[i]) << 16;
            if (remaining > 1)
            {
                group |= static_cast<std::uint32_t>(data[i + 1]) << 8;
            }
            if (remaining > 2)
            {
                group |= static_cast<std::uint32_t>(data[i + 2]);
            }
            out += kAlphabet[(group >> 18) & 0x3F];
            out += kAlphabet[(group >> 12) & 0x3F];
            if (remaining > 1)
            {
                out += kAlphabet[(group >> 6) & 0x3F];
            }
            if (remaining > 2)
            {
                out += kAlphabet[group & 0x3F];
            }
        }
        return out;
    }

    // The type as `ssh-keygen -l` names it.
    static std::string KeyTypeLabel(const std::string& type)
    {
        if (type == "ssh-ed25519")
        {
            return "ED25519";
        }
        if (type == "ssh-rsa")
        {
            return "RSA";
        }
        if (type == "sk-ssh-ed25519@openssh.com")
        {
            return "ED25519-SK";
        }
        if (type == "sk-ecdsa-sha2-nistp256@openssh.com")
        {
            return "ECDSA-SK";
        }
        if (type.compare(0, 6, "ecdsa-") == 0)
        {
            return "ECDSA";
        }
        return type;
    }

    static std::vector<std::string> SplitWords(const std::string& text)
    {
        std::istringstream stream(text);
        std::vector<std::string> words;
        std::string word;
        while (stream >> word)
        {
            words.push_back(word);
        }
        return words;
    }

    PublicKeyDescription DescribePublicKey(const std::string& line)
    {
        PublicKeyDescription description;
        std::vector<std::string> words = SplitWords(line);
        if (words.empty())
        {
            return description;
        }
        description.type = KeyTypeLabel(words[0]);
        std::vector<unsigned char> data;
        if (words.size() >= 2 && DecodeBase64(words[1], &data))
        {
            description.fingerprint = "SHA256:" + EncodeBase64Unpadded(Sha256(data));
        }
        for (std::size_t i = 2; i < words.size(); ++i)
        {
            description.comment += (i > 2 ? " " : "") + words[i];
        }
        return description;
    }

    bool SamePublicKey(const std::string& a, const std::string& b)
    {
        std::vector<std::string> words_a = SplitWords(a);
        std::vector<std::string> words_b = SplitWords(b);
        return words_a.size() >= 2 && words_b.size() >= 2 && words_a[0] == words_b[0] && words_a[1] == words_b[1];
    }

    bool WithPublicKeyComment(const std::string& line, const std::string& comment, std::string* out, std::string* error)
    {
        std::vector<std::string> words = SplitWords(line);
        if (words.size() < 2)
        {
            *error = "That isn't a key line.";
            return false;
        }
        std::string::size_type start = comment.find_first_not_of(" \t");
        std::string trimmed =
            start == std::string::npos ? "" : comment.substr(start, comment.find_last_not_of(" \t") - start + 1);
        for (char c : trimmed)
        {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            {
                *error = "A comment can't have control characters in it.";
                return false;
            }
        }
        if (trimmed.size() > 64)
        {
            *error = "A comment can be up to 64 characters.";
            return false;
        }
        *out = words[0] + " " + words[1] + (trimmed.empty() ? "" : " " + trimmed);
        return true;
    }

}  // namespace ql

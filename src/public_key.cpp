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
               type == "ecdsa-sha2-nistp384" || type == "ecdsa-sha2-nistp521" ||
               type == "sk-ssh-ed25519@openssh.com" || type == "sk-ecdsa-sha2-nistp256@openssh.com";
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
                                  (static_cast<std::uint32_t>(values[2]) << 6) |
                                  static_cast<std::uint32_t>(values[3]);
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
            if (fields == 0 &&
                std::string(data.begin() + static_cast<std::ptrdiff_t>(position),
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
                     "\" isn't valid -- it may have been cut off or changed while copying." +
                     kExample;
            return false;
        }

        *normalized = type + " " + words[1];
        for (std::size_t i = 2; i < words.size(); ++i)
        {
            *normalized += " " + words[i];
        }
        return true;
    }

}  // namespace ql

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace dc {
namespace common {

inline std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline bool IsValidUtf8(const std::string& input) {
    std::size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if ((c & 0x80U) == 0) {
            ++i;
            continue;
        }

        std::size_t need = 0;
        unsigned int codepoint = 0;
        if ((c & 0xE0U) == 0xC0U) {
            need = 1;
            codepoint = c & 0x1FU;
            if (codepoint < 0x02U) {
                return false;  // overlong 2-byte form
            }
        } else if ((c & 0xF0U) == 0xE0U) {
            need = 2;
            codepoint = c & 0x0FU;
        } else if ((c & 0xF8U) == 0xF0U) {
            need = 3;
            codepoint = c & 0x07U;
            if (codepoint > 0x04U) {
                return false;  // beyond U+10FFFF
            }
        } else {
            return false;
        }

        if (i + need >= input.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= need; ++k) {
            unsigned char cc = static_cast<unsigned char>(input[i + k]);
            if ((cc & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (cc & 0x3FU);
        }

        if ((need == 2 && codepoint < 0x800U) || (need == 3 && codepoint < 0x10000U)) {
            return false;
        }
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            return false;
        }
        if (codepoint > 0x10FFFFU) {
            return false;
        }
        i += need + 1;
    }
    return true;
}

inline std::string SanitizeUtf8Lossy(const std::string& input) {
    if (IsValidUtf8(input)) {
        return input;
    }

    std::string out;
    out.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if ((c & 0x80U) == 0) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        bool ok = true;
        std::size_t need = 0;
        unsigned int codepoint = 0;
        if ((c & 0xE0U) == 0xC0U) {
            need = 1;
            codepoint = c & 0x1FU;
            if (codepoint < 0x02U) {
                ok = false;
            }
        } else if ((c & 0xF0U) == 0xE0U) {
            need = 2;
            codepoint = c & 0x0FU;
        } else if ((c & 0xF8U) == 0xF0U) {
            need = 3;
            codepoint = c & 0x07U;
            if (codepoint > 0x04U) {
                ok = false;
            }
        } else {
            ok = false;
        }

        if (ok) {
            if (i + need >= input.size()) {
                ok = false;
            } else {
                for (std::size_t k = 1; k <= need; ++k) {
                    unsigned char cc = static_cast<unsigned char>(input[i + k]);
                    if ((cc & 0xC0U) != 0x80U) {
                        ok = false;
                        break;
                    }
                    codepoint = (codepoint << 6U) | (cc & 0x3FU);
                }
                if ((need == 2 && codepoint < 0x800U) || (need == 3 && codepoint < 0x10000U)) {
                    ok = false;
                }
                if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
                    ok = false;
                }
                if (codepoint > 0x10FFFFU) {
                    ok = false;
                }
            }
        }

        if (ok) {
            out.append(input, i, need + 1);
            i += need + 1;
        } else {
            out.append("\xEF\xBF\xBD");
            ++i;
        }
    }
    return out;
}

}  // namespace common
}  // namespace dc

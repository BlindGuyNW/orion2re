#include "access/text.h"

#include "config/text_encoding.h"

#include <string.h>

namespace access::text {

    namespace {

        bool Is_Whitespace_Command_(unsigned char c) {
            return c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r' || c == ' ';
        }

        bool Is_Function_Sequence_Char_(unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '=' || c == '+' || c == '-' ||
                c == ',' || c == '^';
        }

        // Returns the number of argument bytes that follow a \x18 command letter.
        size_t Dynamic_Argument_Length_(unsigned char command) {
            if (command == 'E' || command == 'G' || (command >= 'Q' && command <= 'T')) {
                return 0;
            }
            return (command >= 'A' && command <= 'P') ? 1 : 0;
        }

        // Returns the number of argument bytes that follow a \x19 condition selector.
        size_t Condition_Argument_Length_(unsigned char selector) {
            if (selector == '0' || selector == '3') {
                return 1;
            }
            if (selector >= '4' && selector <= 'A') {
                return 2;
            }
            return 0;
        }

        // Copies the visible CP850 bytes of `text`, turning whitespace and layout breaks into single spaces.
        std::string Strip_Commands_(const char* text) {
            std::string out;
            size_t len = strlen(text);
            size_t i = 0;

            while (i < len) {
                unsigned char c = (unsigned char)text[i];

                if (c == '\a') {
                    i++;
                    while (i < len && Is_Function_Sequence_Char_((unsigned char)text[i])) {
                        i++;
                    }
                    if (i < len && text[i] == '.') {
                        i++;
                    }
                    continue;
                }

                if (c == 0x17 || c == 0x1A || c == 0x1B || c == 0x1D || c == 0x1F) {
                    i += 2;
                    continue;
                }

                if (c == 0x18) {
                    unsigned char command = (i + 1 < len) ? (unsigned char)text[i + 1] : 0;
                    i += 2 + Dynamic_Argument_Length_(command);
                    continue;
                }

                if (c == 0x19) {
                    unsigned char selector = (i + 1 < len) ? (unsigned char)text[i + 1] : 0;
                    i += 2 + Condition_Argument_Length_(selector);
                    continue;
                }

                if (c == 0x1C) {
                    bool background = i + 1 < len && text[i + 1] == '6';
                    i += background ? 3 : 2;
                    continue;
                }

                // A wrap point inside a word draws nothing unless the line breaks there.
                if (c == '\b') {
                    i++;
                    continue;
                }

                if (Is_Whitespace_Command_(c)) {
                    out.push_back(' ');
                } else if (c >= 0x20) {
                    out.push_back((char)c);
                }
                i++;
            }

            return out;
        }

    } // namespace

    std::string Normalize_Game_Text(const char* text) {
        if (text == nullptr) {
            return std::string();
        }

        std::string visible = Strip_Commands_(text);
        std::string decoded;
        config::Decode_Cp850_To_Utf8(visible.data(), visible.size(), &decoded);
        return Normalize_Spoken_Text(decoded);
    }

    std::string Normalize_Spoken_Text(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        bool pending_space = false;

        for (char ch : text) {
            unsigned char c = (unsigned char)ch;
            if (c < 0x20 || c == ' ' || c == 0x7F) {
                pending_space = !out.empty();
                continue;
            }
            if (pending_space) {
                out.push_back(' ');
                pending_space = false;
            }
            out.push_back(ch);
        }

        return out;
    }

} // namespace access::text

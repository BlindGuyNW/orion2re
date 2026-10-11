#pragma once

#include <string>

namespace access::text {

    // Removes the inline layout, substitution, condition, and style commands from a CP850 game string,
    // decodes it to UTF-8, and collapses it into one spoken line. Substitution arguments cannot be
    // resolved outside their drawing context, so they are dropped.
    std::string Normalize_Game_Text(const char* text);

    // Collapses line structure and whitespace in UTF-8 text the layer is about to speak.
    std::string Normalize_Spoken_Text(const std::string& text);

} // namespace access::text

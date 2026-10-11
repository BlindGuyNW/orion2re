#pragma once

#include <stdint.h>

#include <string>

namespace access::help {

    struct s_help_text {
        std::string title;
        std::string body;
    };

    // Returns the active help list's help id for the box containing the centre of the rectangle,
    // or -1 when the current screen has no help box there.
    int16_t Find_Help_Id(int16_t x, int16_t y, int16_t x_end, int16_t y_end);

    // Reads one record from the current language's help LBX, normalized for speech.
    // Records are cached per language because recipes rebuild several times per keypress.
    bool Read_Help_Text(int16_t help_id, s_help_text* help_text);

} // namespace access::help

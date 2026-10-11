#include "access/vocab.h"

namespace access::vocab {

    namespace {

        const char* const english_strings[] = {
            "Master of Orion 2 screen reader ready",
            "Screen reader on",
            "Screen reader off",
            "Nothing to repeat",
            "No help",
            "Accessibility error in %1: %2",
            "%1 of %2",
            "expanded",
            "collapsed",
            "submenu, %1 items",

            "button",
            "toggle",
            "choice",
            "slider",
            "list item",
            "text field",
            "map area",
            "hotspot",

            "on",
            "off",
            "selected",
            "unavailable",
            "value %1",

            "unlabeled %1",
            "key %1",
            "screen %1",

            "Galaxy",
            "Colony",
            "Ship design",
            "Fleets",
            "Races",
            "Info",
            "Main menu",
            "New game",
            "Hall of fame",
            "Multiplayer",
            "Planet data",
            "Colonies",
            "Leaders",
            "Planets",
            "Research",
            "Reports",
            "Turn summary",
            "Credits",

            "Continue game",
            "Load game",
            "New game",
            "Multiplayer",
            "Hall of fame",
            "Quit game",
            "Loaded mods",

            "Stars",
            "Controls",
            "Other controls",
            "Unexplored %1 star",
            "Black hole",
            "blue",
            "white",
            "yellow",
            "orange",
            "red",
            "brown",
            "capital",
            "1 parsec %1",
            "%1 parsecs %2",
            "north",
            "north-east",
            "east",
            "south-east",
            "south",
            "south-west",
            "west",
            "north-west",
            "1 fleet",
            "%1 fleets",
            "Can't pick out %1 on the map",
            "%1 system",
            "Fleet",
            "Fleet at %1",
        };

        static_assert(sizeof(english_strings) / sizeof(english_strings[0]) == VOCAB_COUNT, "one English string per vocabulary id");

    } // namespace

    const char* Get(e_vocab_string id) {
        if (id < 0 || id >= VOCAB_COUNT) {
            return "";
        }
        return english_strings[id];
    }

    std::string Format(e_vocab_string id, const std::string& first, const std::string& second) {
        std::string out;
        const char* text = Get(id);

        for (size_t i = 0; text[i] != '\0'; ++i) {
            if (text[i] == '%' && (text[i + 1] == '1' || text[i + 1] == '2')) {
                out += (text[i + 1] == '1') ? first : second;
                ++i;
                continue;
            }
            out.push_back(text[i]);
        }

        return out;
    }

} // namespace access::vocab

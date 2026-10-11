#pragma once

#include <string>

namespace access::vocab {

    // Every string the accessibility layer authors itself. Game content (help titles, button text)
    // is already localized and is passed through instead.
    enum e_vocab_string {
        VOCAB_STARTUP,
        VOCAB_LAYER_ON,
        VOCAB_LAYER_OFF,
        VOCAB_NOTHING_TO_REPEAT,
        VOCAB_NO_TOOLTIP,
        VOCAB_FAULT,
        VOCAB_POSITION,
        VOCAB_EXPANDED,
        VOCAB_COLLAPSED,
        VOCAB_SUBMENU,

        VOCAB_ROLE_BUTTON,
        VOCAB_ROLE_TOGGLE,
        VOCAB_ROLE_CHOICE,
        VOCAB_ROLE_SLIDER,
        VOCAB_ROLE_LIST_ITEM,
        VOCAB_ROLE_TEXT_FIELD,
        VOCAB_ROLE_MAP_AREA,
        VOCAB_ROLE_HOTSPOT,

        VOCAB_STATE_ON,
        VOCAB_STATE_OFF,
        VOCAB_STATE_SELECTED,
        VOCAB_STATE_UNAVAILABLE,
        VOCAB_VALUE,

        VOCAB_UNLABELED,
        VOCAB_HOTKEY,
        VOCAB_SCREEN_NUMBER,

        VOCAB_SCREEN_MAIN,
        VOCAB_SCREEN_COLONY,
        VOCAB_SCREEN_DESIGN,
        VOCAB_SCREEN_FLEET,
        VOCAB_SCREEN_RACE,
        VOCAB_SCREEN_INFO,
        VOCAB_SCREEN_MAIN_MENU,
        VOCAB_SCREEN_NEW_GAME,
        VOCAB_SCREEN_HALL_OF_FAME,
        VOCAB_SCREEN_MULTI_PLAYER,
        VOCAB_SCREEN_PLANET_DATA,
        VOCAB_SCREEN_COLONY_SUMMARY,
        VOCAB_SCREEN_OFFICERS,
        VOCAB_SCREEN_PLANET_SUMMARY,
        VOCAB_SCREEN_TECH_CHANGE,
        VOCAB_SCREEN_REPORTS,
        VOCAB_SCREEN_TURN_SUMMARY,
        VOCAB_SCREEN_CREDITS,

        VOCAB_MAIN_MENU_CONTINUE,
        VOCAB_MAIN_MENU_LOAD,
        VOCAB_MAIN_MENU_NEW,
        VOCAB_MAIN_MENU_MULTIPLAYER,
        VOCAB_MAIN_MENU_HALL_OF_FAME,
        VOCAB_MAIN_MENU_QUIT,
        VOCAB_MAIN_MENU_CREDITS,

        VOCAB_COUNT
    };

    const char* Get(e_vocab_string id);

    // Replaces %1 and %2 in the string with the arguments.
    std::string Format(e_vocab_string id, const std::string& first, const std::string& second = std::string());

} // namespace access::vocab

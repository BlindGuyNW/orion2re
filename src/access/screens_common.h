#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "access/screens.h"
#include "access/vocab.h"

// Shared by the screen recipe files: one file per screen or family of screens, each defining
// its recipes, and screens.cpp listing them in match order.
namespace access::screens {

    using graph::AnnouncementKinds::Label;
    using graph::AnnouncementKinds::Role;
    using graph::AnnouncementKinds::Selected;
    using graph::AnnouncementKinds::Value;
    using graph::AnnouncementKinds::Enabled;

    // Ticks a press waits for the redraw or page change it depends on before giving up.
    constexpr int32_t PENDING_PRESS_MAX_TICKS = 30;

    extern const graph::ControlType button_type;
    extern const graph::ControlType toggle_type;
    extern const graph::ControlType choice_type;

    bool Is_Valid_Field(int16_t field_idx);

    bool Is_Usable_String(const char* text);

    // The text the game itself draws on the field, when it draws any.
    std::string Field_Own_Text(const s_field* field);

    int16_t Field_Help_Id(const s_field* field);

    std::string Help_Title(int16_t help_id);

    std::string Hotkey_Text(const s_field* field);

    // Label precedence: what the game draws on the field, then its help title, then its hotkey.
    std::string Field_Label(int16_t field_idx);

    void Speak_Field_Help(int16_t field_idx);

    // A node over one live game field: activation goes through the game's own hotkey path.
    graph::NodeVtable Field_Vtable(int16_t field_idx, const std::string& label);

    void Add_Field_Item(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
        int16_t field_idx, const std::string& label);

    std::string Screen_Name(int16_t screen);

    bool Is_Offscreen(const s_field* field);

    bool Is_Catch_All(const s_field* field);

    // Keyboard-only hotkeys have no visual counterpart, and a full-screen "click anywhere"
    // region is listed in place only when it is the screen's sole control.
    bool Field_Is_Listed(int16_t field_idx, bool has_specific_field);

    // A read-only node: the first line is its label, the rest its value.
    graph::NodeVtable Info_Vtable(const std::vector<std::string>& lines);

    std::string Help_Label_Or(int16_t field_idx, vocab::e_vocab_string fallback);

    // The game keeps a few H_Message strings as printf formats; it formats them the same way.
    std::string H_Text(int16_t message_id);

    // Joins the non-empty parts with commas.
    std::string Join_Values(const std::vector<std::string>& parts);

    // The recipes, in the files of their screens.
    extern const s_screen_recipe text_box_recipe;
    extern const s_screen_recipe confirm_recipe;
    extern const s_screen_recipe input_box_recipe;
    extern const s_screen_recipe science_room_recipe;
    extern const s_screen_recipe officer_level_recipe;
    extern const s_screen_recipe planet_select_recipe;
    extern const s_screen_recipe officer_offer_recipe;
    extern const s_screen_recipe turn_summary_recipe;
    extern const s_screen_recipe landing_recipe;
    extern const s_screen_recipe design_generic_picker_recipe;
    extern const s_screen_recipe design_specials_picker_recipe;
    extern const s_screen_recipe design_recipe;
    extern const s_screen_recipe build_recipe;
    extern const s_screen_recipe colony_recipe;
    extern const s_screen_recipe research_recipe;
    extern const s_screen_recipe main_menu_recipe;
    extern const s_screen_recipe new_game_recipe;
    extern const s_screen_recipe galaxy_recipe;
    extern const s_screen_recipe fields_recipe;

} // namespace access::screens

#pragma once
#include <cstdint>

struct s_list_state;
struct s_list_item;
struct s_colors;

namespace TURNSUM {
    extern s_colors turn_sum_normal_color;

    extern s_colors turn_sum_high_color;

    extern s_list_state g_turn_sum_header;
    extern uint8_t g_turn_message_ix;
    extern s_list_item* g_selected_msg_p;
    // The popup's OK and page buttons while Turn_Summary_Popup waits for input, for the screen
    // reader layer; shown_ok_field is 0 otherwise.
    extern int16_t shown_ok_field;
    extern int16_t shown_page_up_field;
    extern int16_t shown_page_down_field;

    /**
     * Updates the turn summary popup selection state and redraws the visible list text.
     */
    void __cdecl Draw_Turn_Summary_Popup();

    /**
     * Opens the turn summary popup, handles paging and message selection, and restores the caller's screen state.
     */
    void __cdecl Turn_Summary_Popup();
} // namespace TURNSUM
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

// The graph navigator and screen manager (graph a11y spec sections 7 to 9).
//
// Keys while a screen is attached: arrows move, Home/End jump to the ends, Tab/Shift+Tab cycle
// zones, Enter activates, Space reads the focused control's help, Ctrl+L repeats where you are.
// Escape and every other key go to the game as usual.
// Everywhere: Ctrl+R repeats the last thing spoken, Ctrl+Shift+A turns the layer on or off.
namespace access::navigator {

    // Game thread, before the game reads input for the first time.
    void Initialize();
    void Shutdown();

    // Platform thread. Returns true when the layer claims the key, so the game must not see it.
    // Claims are a pure function of state the game thread publishes, so this never blocks on it.
    bool Offer_Key_Down(SDL_Keycode key, SDL_Keymod modifiers, bool repeat);

    // Game thread, from fields::Get_Input: attaches to the current screen, handles queued keys,
    // and announces focus changes.
    void Tick();

    // Game thread. A node's activation asks for its field; Get_Input performs it through the
    // game's own hotkey path on the same call.
    void Request_Field_Activation(int16_t field_idx);
    bool Take_Field_Activation(int16_t* field_idx);

} // namespace access::navigator

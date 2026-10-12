#include "pch.h"

#include "access/screens.h"

#include "access/screens_common.h"

namespace access::screens {

    namespace {

        // Match order: popups that draw over a screen come before the screens they draw over, and
        // the generic field list comes last.
        const s_screen_recipe recipes[] = {
            text_box_recipe,
            confirm_recipe,
            input_box_recipe,
            science_room_recipe,
            officer_level_recipe,
            planet_select_recipe,
            officer_offer_recipe,
            turn_summary_recipe,
            landing_recipe,
            design_generic_picker_recipe,
            design_specials_picker_recipe,
            design_weapons_picker_recipe,
            design_recipe,
            build_recipe,
            colony_recipe,
            research_recipe,
            main_menu_recipe,
            new_game_recipe,
            galaxy_recipe,
            fields_recipe,
        };

    } // namespace

    const s_screen_recipe* Recipes(int32_t* count) {
        *count = (int32_t)(sizeof(recipes) / sizeof(recipes[0]));
        return recipes;
    }

} // namespace access::screens

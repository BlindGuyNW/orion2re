#pragma once

#include <stdint.h>

#include <string>
#include <unordered_map>

#include "access/graph/GraphBuilder.hpp"

namespace access::screens {

    // Filled by a recipe's Build: which game field backs each node, so the navigator can activate
    // it and move the pointer onto it.
    struct s_build_context {
        std::unordered_map<std::string, int16_t> node_fields;
    };

    // An accessible view over one game screen (graph a11y spec section 9).
    struct s_screen_recipe {
        const char* key;
        // Re-derived from live game state on every poll.
        bool (*Is_Active)();
        // Distinguishes instances of the screen; a change re-attaches and announces the screen again.
        std::string (*Instance_Key)();
        // Declares the screen's nodes from live state. Runs several times per keypress.
        void (*Build)(graph::GraphBuilder* builder, s_build_context* context);
        std::string (*Name)();
    };

    // Registered recipes in match order. The last one matches any screen with fields.
    const s_screen_recipe* Recipes(int32_t* count);

} // namespace access::screens

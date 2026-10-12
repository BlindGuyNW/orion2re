#include "pch.h"

#include "access/screens_common.h"

#include "access/vocab.h"

#include <string>

namespace access::screens {

    namespace {

        // ---- Any screen: the live field list -----------------------------------------------------

        bool Fields_Is_Active_() {
            return fields::fields != nullptr && fields::fields_count >= 2;
        }

        std::string Fields_Instance_Key_() {
            const s_field* first = &fields::fields[1];
            char key[96];
            snprintf(key, sizeof(key), "fields/%d/%p/%d,%d,%d", (int)MOX::current_screen,
                (const void*)fields::help_struct_pointer, (int)first->x, (int)first->y, (int)first->field_type);
            return key;
        }

        void Fields_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            bool has_specific_field = false;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (field->field_type != FIELD_TYPE_MULTI_HOTKEY && !Is_Offscreen(field) && !Is_Catch_All(field)) {
                    has_specific_field = true;
                    break;
                }
            }

            for (int16_t i = 1; i < fields::fields_count; ++i) {
                if (!Field_Is_Listed(i, has_specific_field)) {
                    continue;
                }
                const s_field* field = &fields::fields[i];
                char key[48];
                snprintf(key, sizeof(key), "field/%d/%d,%d", (int)i, (int)field->x, (int)field->y);
                Add_Field_Item(builder, context, key, i, Field_Label(i));
            }

            // Some screens close only on a click outside their controls, such as the detailed ship
            // view, so a full-screen region is still listed after them. What it does depends on
            // the screen, so it is named for the gesture.
            if (!has_specific_field) {
                return;
            }
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (!Is_Catch_All(field)) {
                    continue;
                }
                std::string label = Field_Own_Text(field);
                if (label.empty()) {
                    label = Hotkey_Text(field);
                }
                if (label.empty()) {
                    label = vocab::Get(vocab::VOCAB_CLICK_ANYWHERE);
                }
                char key[48];
                snprintf(key, sizeof(key), "field/%d/anywhere", (int)i);
                Add_Field_Item(builder, context, key, i, label);
            }
        }

        std::string Fields_Name_() {
            return Screen_Name(MOX::current_screen);
        }

    } // namespace

    const s_screen_recipe fields_recipe = { "fields", Fields_Is_Active_, Fields_Instance_Key_, Fields_Build_, Fields_Name_ };

} // namespace access::screens

#include "pch.h"

#include "access/screens_common.h"

#include "access/help_text.h"
#include "access/navigator.h"
#include "access/speech.h"
#include "access/text.h"
#include "access/vocab.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace access::screens {

    namespace {

        // ---- Build queue ---------------------------------------------------------------------------

        // The popup's own loop only: refit, auto build, and message popups run nested loops from
        // Evaluate_Input with the screen unchanged.
        bool Build_Is_Active_() {
            return MOX::current_screen == SCREEN_QUEUE_POPUP && COLBLDG::awaiting_input != 0 &&
                COLONY::colony_handle >= 0 && COLONY::colony_handle < MOX::NUM_COLONIES;
        }

        std::string Build_Instance_Key_() {
            return "build/" + std::to_string(COLONY::colony_handle);
        }

        // The title Draw_Build_Queue_Popup prints over the queue.
        std::string Build_Name_() {
            char title[200];
            snprintf(title, sizeof(title), ESTRINGS::E_Strings(ESTR_BUILD_LIST_FOR_S),
                HACCESS::Get_Planet_Name(MOX::colony[COLONY::colony_handle].planet));
            return text::Normalize_Game_Text(title);
        }

        // COLBLDG::Selection_Name without its side effects: for a refit it formats into the game's
        // message buffer and resets the description cache the detail panel draws from.
        std::string Build_Product_Name_(int16_t product) {
            if (COLBLDG::Colony_Production_Is_Queued_Ship(product)) {
                const s_ship_data* ship = &MOX::ship[COLBLDG::Colony_Production_Ship_Index(product)];
                if (ship->status != 4) {
                    return text::Normalize_Game_Text(ship->d.name);
                }
                char name[200];
                snprintf(name, sizeof(name), ESTRINGS::E_Strings(ESTR_S_S), ESTRINGS::E_Strings(ESTR_REFIT_SHIP), ship->d.name);
                return text::Normalize_Game_Text(name);
            }
            return text::Normalize_Game_Text(COLBLDG::Selection_Name(product));
        }

        void Append_Game_Line_(std::string* text, const char* line) {
            std::string normalized = text::Normalize_Game_Text(line);
            if (!normalized.empty()) {
                *text += (text->empty() ? "" : ", ") + normalized;
            }
        }

        // The lines Draw_Cost_And_Time_Info prints under the name in the detail panel, for the
        // items Draw_Current_Selection prints them for.
        std::string Build_Cost_Text_(int16_t product) {
            switch (product) {
                case COLONY_PRODUCTION_NONE:
                case COLONY_PRODUCTION_HOUSING:
                case COLONY_PRODUCTION_TRADE_GOODS:
                case COLONY_PRODUCTION_REPEAT:
                case COLONY_PRODUCTION_REFIT:
                case COLONY_PRODUCTION_SEPARATOR:
                case COLONY_PRODUCTION_CUSTOM_DESIGN:
                case COLONY_PRODUCTION_DOOM_STAR:
                    return std::string();
                default:
                    break;
            }
            int16_t colony_idx = COLONY::colony_handle;
            s_colony* colony = &MOX::colony[colony_idx];
            std::string text;
            char line[200];
            snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_COST_HERE_D), (int32_t)(int16_t)COLCALC::Colony_Product_Cost(colony, product));
            Append_Game_Line_(&text, line);
            if (COLBLDG::Colony_Production_Is_Building(product)) {
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_MAINT_COST_D), (int32_t)TECHDATA::buildings[product].maintenance);
                Append_Game_Line_(&text, line);
            }
            snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_BUILD_TIME_D),
                (int32_t)COLONY::Calculate_Colony_Turn_Count_From_Scrap_For_Prod(colony_idx, product));
            Append_Game_Line_(&text, line);
            if (colony->producing[0] == product) {
                snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_TURNS_LEFT_D), (int32_t)COLONY::Calculate_Current_Production_Turn_Count(colony_idx));
                Append_Game_Line_(&text, line);
            } else if (product == BUILDING_GRAVITY_GENERATOR) {
                int8_t gravity = MOX::planet[colony->planet].gravity_class;
                if (gravity >= 0 && gravity < PLANET_GRAVITY_COUNT) {
                    snprintf(line, sizeof(line), ESTRINGS::E_Strings(ESTR_GRAVITY_HERE_S), MOX::planet_gravity_string[gravity]);
                    Append_Game_Line_(&text, line);
                }
            }
            return text;
        }

        // The paragraph the detail panel prints below the cost lines (Draw_Current_Selection).
        std::string Build_Description_(int16_t product) {
            int16_t app_id = -1;
            switch (product) {
                case COLONY_PRODUCTION_OUTPOST_SHIP: app_id = 109; break;
                case COLONY_PRODUCTION_FREIGHTERS: app_id = 69; break;
                case COLONY_PRODUCTION_COLONY_SHIP: app_id = 41; break;
                case COLONY_PRODUCTION_TRANSPORT_SHIP: app_id = 189; break;
                case COLONY_PRODUCTION_SPY: app_id = 166; break;
                case COLONY_PRODUCTION_SCIENTIST: app_id = 7; break;
                case COLONY_PRODUCTION_WORKER: app_id = 8; break;
                case COLONY_PRODUCTION_FARMER: app_id = 6; break;
                case COLONY_PRODUCTION_HOUSING:
                case COLONY_PRODUCTION_TRADE_GOODS: {
                    std::string text;
                    help::Read_Text_Record(ESTRINGS::E_Strings(ESTR_MAINTEXTLBX), product == COLONY_PRODUCTION_HOUSING ? 13 : 12, 0, &text);
                    return text;
                }
                default:
                    break;
            }
            s_ship_design* design = nullptr;
            if (COLBLDG::Colony_Production_Is_Building(product)) {
                app_id = (int16_t)TECHDATA::buildings[product].tech_app;
            } else if (COLBLDG::Colony_Production_Is_Queued_Ship(product)) {
                design = &MOX::ship[COLBLDG::Colony_Production_Ship_Index(product)].d;
            } else if (COLBLDG::Colony_Production_Is_Ship_Design(product)) {
                design = &MOX::player[MOX::PLAYER_NUM].ship_designs[COLBLDG::Colony_Production_Ship_Design_Index(product)];
            }
            if (design != nullptr) {
                char paragraph[20000];
                STRBUILD::s_builder builder = STRBUILD::Init(paragraph, sizeof(paragraph));
                COLBLDG::Build_Ship_Design_Info_Formatted_Paragraph(design, &builder);
                return text::Normalize_Game_Text(paragraph);
            }
            help::s_help_text help_text;
            return help::Read_Help_Text(app_id, &help_text) ? help_text.body : std::string();
        }

        // The designs Draw_Militarys_List draws dimmed: ones this colony can't build, and large
        // hulls without a starbase to build them at.
        bool Build_Ship_Is_Dim_(int16_t product) {
            if (!COLBLDG::Colony_Production_Is_Ship_Design(product) || COLBLDG::field_mode == 1) {
                return false;
            }
            s_colony* colony = &MOX::colony[COLONY::colony_handle];
            const s_ship_design* design = &MOX::player[MOX::PLAYER_NUM].ship_designs[COLBLDG::Colony_Production_Ship_Design_Index(product)];
            if (colony->buildings[BUILDING_STAR_BASE] == 0 && colony->buildings[BUILDING_STAR_FORTRESS] == 0 &&
                colony->buildings[BUILDING_BATTLESTATION] == 0 && design->size >= SHIP_SIZE_LARGE) {
                return true;
            }
            return !COLCALC::Colony_Can_Build_Product(colony, product);
        }

        int16_t Build_Held_Slot_() {
            return COLBLDG::active_prod[0] >= BUILDING_COUNT ? (int16_t)(COLBLDG::active_prod[0] - BUILDING_COUNT) : -1;
        }

        // A queue entry as the game draws it, with the Repeat slot after an item folded into it.
        struct s_build_entry {
            int16_t slot;
            int16_t product;
            bool repeating;
        };

        std::vector<s_build_entry> Build_Entries_(const int16_t* items) {
            std::vector<s_build_entry> entries;
            for (int16_t slot = 0; slot < 7; ++slot) {
                if (items[slot] == COLONY_PRODUCTION_NONE) {
                    continue;
                }
                if (items[slot] == COLONY_PRODUCTION_REPEAT) {
                    if (!entries.empty() && entries.back().slot == slot - 1) {
                        entries.back().repeating = true;
                    }
                    continue;
                }
                entries.push_back(s_build_entry{ slot, items[slot], false });
            }
            return entries;
        }

        // The queue as it was when a press was requested; the poll speaks what the press changed.
        struct s_build_snapshot {
            int16_t colony;
            int16_t items[7];
            std::string names[7];
            int16_t held_slot;
        };

        s_build_snapshot Build_Snapshot_() {
            s_build_snapshot snapshot;
            snapshot.colony = COLONY::colony_handle;
            snapshot.held_slot = Build_Held_Slot_();
            for (int16_t slot = 0; slot < 7; ++slot) {
                int16_t product = COLBLDG::current_item[slot];
                snapshot.items[slot] = product;
                if (product != COLONY_PRODUCTION_NONE && product != COLONY_PRODUCTION_REPEAT) {
                    snapshot.names[slot] = Build_Product_Name_(product);
                }
            }
            return snapshot;
        }

        enum e_build_removal {
            BUILD_REMOVAL_NONE,
            BUILD_REMOVAL_DROP,
            BUILD_REMOVAL_PICK,
        };

        s_build_snapshot build_before;
        bool build_change_pending = false;
        e_build_removal build_removal = BUILD_REMOVAL_NONE;
        int16_t build_removal_slot = -1;
        int16_t polled_build_held = -1;
        std::string polled_build_held_name;
        uint8_t polled_build_mode = 0;

        void Build_Press_(int16_t field_idx) {
            if (!Is_Valid_Field(field_idx)) {
                return;
            }
            build_before = Build_Snapshot_();
            build_change_pending = true;
            navigator::Request_Field_Activation(field_idx);
        }

        // The game removes a queue entry with two presses, one to pick it up and one on it again;
        // anything already held, or a pending Design or Repeat choice, is let go first. The poll
        // makes each later press once the game has acted on the one before.
        void Build_Remove_(int16_t slot) {
            int16_t held = Build_Held_Slot_();
            build_removal = BUILD_REMOVAL_NONE;
            build_removal_slot = slot;
            if (held == slot && COLBLDG::field_mode == 0) {
                Build_Press_(COLBLDG::queue_fields[slot]);
            } else if (held >= 0 || COLBLDG::field_mode != 0) {
                build_removal = BUILD_REMOVAL_DROP;
                Build_Press_(COLBLDG::fields[0]);
            } else {
                build_removal = BUILD_REMOVAL_PICK;
                Build_Press_(COLBLDG::queue_fields[slot]);
            }
        }

        void Build_Advance_Removal_() {
            int16_t held = Build_Held_Slot_();
            int16_t field_idx = COLBLDG::queue_fields[build_removal_slot];
            if (build_removal == BUILD_REMOVAL_DROP && held < 0 && COLBLDG::field_mode == 0) {
                build_removal = BUILD_REMOVAL_PICK;
                navigator::Request_Field_Activation(field_idx);
                return;
            }
            // A pick-up the game refused, such as a bought item, leaves nothing held.
            if (build_removal == BUILD_REMOVAL_PICK && held == build_removal_slot) {
                navigator::Request_Field_Activation(field_idx);
            }
            build_removal = BUILD_REMOVAL_NONE;
        }

        std::string Build_Change_Text_(const s_build_snapshot& before) {
            std::vector<s_build_entry> old_entries = Build_Entries_(before.items);
            std::vector<s_build_entry> new_entries = Build_Entries_(COLBLDG::current_item);
            size_t common = std::min(old_entries.size(), new_entries.size());
            size_t first = common;
            for (size_t i = 0; i < common; ++i) {
                if (old_entries[i].product != new_entries[i].product || old_entries[i].repeating != new_entries[i].repeating) {
                    first = i;
                    break;
                }
            }
            if (new_entries.size() > old_entries.size()) {
                const s_build_entry& added = new_entries[first];
                std::string text = vocab::Format(vocab::VOCAB_BUILD_ADDED, Build_Product_Name_(added.product), std::to_string(first + 1));
                if (added.repeating) {
                    text += ", " + std::string(vocab::Get(vocab::VOCAB_BUILD_REPEATING));
                }
                return text;
            }
            if (new_entries.size() < old_entries.size()) {
                return vocab::Format(vocab::VOCAB_BUILD_REMOVED, before.names[old_entries[first].slot]);
            }
            if (first == common) {
                return std::string();
            }
            // A move shifts the entries between its two ends; the held one is what moved.
            int16_t moved = before.held_slot >= 0 ? before.items[before.held_slot] : old_entries[first].product;
            size_t position = new_entries.size();
            for (size_t i = 0; i < new_entries.size() && position == new_entries.size(); ++i) {
                if (new_entries[i].product == moved && old_entries[i].product != moved) {
                    position = i;
                }
            }
            if (position == new_entries.size()) {
                return std::string();
            }
            return vocab::Format(vocab::VOCAB_BUILD_MOVED, Build_Product_Name_(moved), std::to_string(position + 1));
        }

        void Speak_Build_Description_(int16_t product) {
            std::string description = Build_Description_(product);
            speech::Say(description.empty() ? std::string(vocab::Get(vocab::VOCAB_NO_TOOLTIP)) : description, true);
        }

        // A product row: Enter presses its field, and the poll speaks what the press changed.
        graph::NodeVtable Build_Product_Vtable_(int16_t product, int16_t field_idx, std::function<std::string()> value) {
            graph::NodeVtable vtable;
            vtable.Type = &button_type;
            vtable.Announcements.push_back(graph::NodeAnnouncement::Static(Build_Product_Name_(product)));
            vtable.Announcements.back().Kind = Label;
            vtable.Announcements.push_back(graph::NodeAnnouncement(value, true, Value));
            vtable.OnActivate = [field_idx]() { Build_Press_(field_idx); };
            vtable.StateText = []() { return std::string(); };
            vtable.OnTooltip = [product]() { Speak_Build_Description_(product); };
            return vtable;
        }

        void Build_Queue_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            std::vector<s_build_entry> entries = Build_Entries_(COLBLDG::current_item);
            if (entries.empty()) {
                builder->AddItem(graph::ControlId::Structural("build/queue/empty"),
                    Info_Vtable(std::vector<std::string>{ vocab::Get(vocab::VOCAB_BUILD_QUEUE_EMPTY) }));
                return;
            }
            for (const s_build_entry& entry : entries) {
                int16_t slot = entry.slot;
                int16_t product = entry.product;
                int16_t field_idx = COLBLDG::queue_fields[slot];
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }
                std::string key = "build/queue/" + std::to_string(slot);
                graph::NodeVtable vtable = Build_Product_Vtable_(product, field_idx, [slot, product]() {
                    bool repeating = slot < 6 && COLBLDG::current_item[slot + 1] == COLONY_PRODUCTION_REPEAT;
                    return Join_Values({
                        Build_Held_Slot_() == slot ? std::string(vocab::Get(vocab::VOCAB_BUILD_HELD)) : std::string(),
                        repeating ? std::string(vocab::Get(vocab::VOCAB_BUILD_REPEATING)) : std::string(),
                        Build_Cost_Text_(product),
                    });
                });
                vtable.OnSecondary = [slot]() { Build_Remove_(slot); };
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        void Build_Buildings_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int16_t i = 0; i < COLBLDG::n_building_indexes; ++i) {
                int16_t product = COLBLDG::building_indexes[i];
                int16_t field_idx = COLBLDG::building_fields[i];
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }
                std::string key = "build/building/" + std::to_string(product);
                graph::NodeVtable vtable = Build_Product_Vtable_(product, field_idx, [product]() {
                    return Join_Values({
                        COLBLDG::Has_Prod_In_Queue(product) ? std::string(vocab::Get(vocab::VOCAB_BUILD_QUEUED)) : std::string(),
                        Build_Cost_Text_(product),
                    });
                });
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        void Build_Ships_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            for (int16_t i = 0; i < COLBLDG::n_military_indexes; ++i) {
                int16_t product = COLBLDG::military_indexes[i];
                int16_t field_idx = COLBLDG::military_fields[i];
                if (product == COLONY_PRODUCTION_SEPARATOR || !Is_Valid_Field(field_idx)) {
                    continue;
                }
                std::string key = "build/ship/" + std::to_string(product);
                graph::NodeVtable vtable = Build_Product_Vtable_(product, field_idx, [product]() {
                    return Join_Values({
                        Build_Ship_Is_Dim_(product) ? std::string(vocab::Get(vocab::VOCAB_STATE_UNAVAILABLE)) : std::string(),
                        Build_Cost_Text_(product),
                    });
                });
                context->node_fields[key] = field_idx;
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }
        }

        void Build_Controls_Stop_(graph::GraphBuilder* builder, s_build_context* context) {
            // Buy and the colony switches are keyboard-only hotkeys, with no help box to name them.
            const struct {
                int32_t slot;
                const char* key;
                vocab::e_vocab_string fallback;
            } controls[] = {
                { 2, "build/auto_build", vocab::VOCAB_COLONY_AUTO_BUILD },
                { 6, "build/repeat", vocab::VOCAB_COUNT },
                { 4, "build/refit", vocab::VOCAB_COUNT },
                { 5, "build/design", vocab::VOCAB_COUNT },
                { 10, "build/buy", vocab::VOCAB_COLONY_BUY },
                { 8, "build/previous", vocab::VOCAB_COLONY_PREVIOUS },
                { 9, "build/next", vocab::VOCAB_COLONY_NEXT },
                { 0, "build/cancel", vocab::VOCAB_COUNT },
                { 3, "build/ok", vocab::VOCAB_COUNT },
            };
            for (const auto& control : controls) {
                int16_t field_idx = COLBLDG::fields[control.slot];
                if (!Is_Valid_Field(field_idx)) {
                    continue;
                }
                std::string label = control.fallback != vocab::VOCAB_COUNT ? Help_Label_Or(field_idx, control.fallback) : Field_Label(field_idx);
                Add_Field_Item(builder, context, control.key, field_idx, label);
            }
        }

        void Build_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            builder->BeginStop("build/queue");
            builder->PushContext(vocab::Get(vocab::VOCAB_BUILD_QUEUE));
            Build_Queue_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("build/buildings");
            builder->PushContext(vocab::Get(vocab::VOCAB_BUILD_BUILDINGS));
            Build_Buildings_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("build/ships");
            builder->PushContext(vocab::Get(vocab::VOCAB_BUILD_SHIPS));
            Build_Ships_Stop_(builder, context);
            builder->PopContext();

            builder->BeginStop("build/controls");
            builder->PushContext(vocab::Get(vocab::VOCAB_COLONY_CONTROLS));
            Build_Controls_Stop_(builder, context);
            builder->PopContext();
        }

        // Speaks what a press changed once the game has acted on it, even after a popup it opened
        // has closed; otherwise what is held, and which choice Design or Repeat Build waits for.
        void Build_Poll_(bool just_attached) {
            if (navigator::Field_Activation_Pending()) {
                return;
            }
            int16_t held = Build_Held_Slot_();
            uint8_t mode = COLBLDG::field_mode;
            if (build_removal != BUILD_REMOVAL_NONE) {
                Build_Advance_Removal_();
            } else {
                std::string text;
                if (build_change_pending) {
                    build_change_pending = false;
                    if (build_before.colony == COLONY::colony_handle) {
                        text = Build_Change_Text_(build_before);
                    }
                }
                if (text.empty() && !just_attached) {
                    if (held != polled_build_held) {
                        if (held >= 0) {
                            text = vocab::Format(vocab::VOCAB_BUILD_HOLDING, Build_Product_Name_(COLBLDG::current_item[held]));
                        } else if (!polled_build_held_name.empty()) {
                            text = vocab::Format(vocab::VOCAB_BUILD_PUT_BACK, polled_build_held_name);
                        }
                    } else if (mode != polled_build_mode) {
                        if (mode == 1) {
                            text = vocab::Get(vocab::VOCAB_BUILD_CHOOSE_DESIGN);
                            navigator::Request_Focus_Stop("build/ships");
                        } else if (mode == 2) {
                            text = vocab::Get(vocab::VOCAB_BUILD_CHOOSE_REPEAT);
                        } else {
                            text = vocab::Get(vocab::VOCAB_BUILD_CANCELLED);
                        }
                    }
                }
                speech::Say(text, true);
            }
            polled_build_held = held;
            polled_build_held_name = held >= 0 ? Build_Product_Name_(COLBLDG::current_item[held]) : std::string();
            polled_build_mode = mode;
        }

    } // namespace

    const s_screen_recipe build_recipe = { "build", Build_Is_Active_, Build_Instance_Key_, Build_Build_, Build_Name_, Build_Poll_ };

} // namespace access::screens

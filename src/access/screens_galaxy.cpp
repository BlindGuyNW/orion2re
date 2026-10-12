#include "pch.h"

#include "access/screens_common.h"

#include "access/navigator.h"
#include "access/speech.h"
#include "access/text.h"
#include "access/vocab.h"

#include <algorithm>
#include <string>
#include <vector>

namespace access::screens {

    namespace {

        // ---- Galaxy map --------------------------------------------------------------------------

        constexpr int16_t GALAXY_GRID_X = 22;
        constexpr int16_t GALAXY_GRID_Y = 22;

        // The main loop hit-tests stars and fleets at the pointer when its map grid is pressed, so
        // a star node puts the pointer on the star and presses the grid.
        bool Galaxy_Is_Active_() {
            if (MOX::current_screen != SCREEN_MAIN || MOX::zoom_in == 1) {
                return false;
            }
            int16_t grid = MOX::main_screen_grid_field;
            return Is_Valid_Field(grid) && fields::fields[grid].field_type == FIELD_TYPE_GRID &&
                fields::fields[grid].x == GALAXY_GRID_X && fields::fields[grid].y == GALAXY_GRID_Y &&
                Is_Valid_Field(MAINSCR::next_button);
        }

        std::string Galaxy_Instance_Key_() {
            return "galaxy";
        }

        std::string Galaxy_Name_() {
            return vocab::Get(vocab::VOCAB_SCREEN_MAIN);
        }

        int16_t Capital_Star_() {
            int16_t planet_idx = MOX::player[MOX::PLAYER_NUM].home_planet_id;
            if (planet_idx < 0 || planet_idx >= MOX::planet_count) {
                return -1;
            }
            int16_t star_idx = MOX::planet[planet_idx].star_index;
            return star_idx >= 0 && star_idx < MOX::NUM_STARS ? star_idx : -1;
        }

        // Mirrors the visibility rules of MAINSCR::Get_Star_Name, which cannot be called here
        // because it changes the font state as it decides.
        bool Star_Name_Is_Shown_(int16_t star_idx) {
            const s_star_data* star = &MOX::star[star_idx];
            if (star->spectral_class == STAR_CLASS_BLACK_HOLE) {
                return false;
            }
            if (HAROLD::Player_Is_Omniscient(MOX::PLAYER_NUM)) {
                return true;
            }
            bool has_contact = star->owner >= 0 && star->owner < MAX_PLAYERS && HAROLD::Contact_With_One_Colony(star_idx);
            return has_contact || HAROLD::Player_Has_Visited(MOX::PLAYER_NUM, star_idx) != 0;
        }

        // The name is drawn in the owner's colour only with contact (or galactic lore).
        int16_t Star_Shown_Owner_(int16_t star_idx) {
            const s_star_data* star = &MOX::star[star_idx];
            if (star->owner < 0 || star->owner >= MAX_PLAYERS || !Star_Name_Is_Shown_(star_idx)) {
                return -1;
            }
            if (star->owner == MOX::PLAYER_NUM || HAROLD::Player_Is_Omniscient(MOX::PLAYER_NUM) ||
                HAROLD::Contact_With_One_Colony(star_idx)) {
                return star->owner;
            }
            return -1;
        }

        std::string Star_Label_(int16_t star_idx) {
            const s_star_data* star = &MOX::star[star_idx];
            if (star->spectral_class == STAR_CLASS_BLACK_HOLE) {
                return vocab::Get(vocab::VOCAB_STAR_BLACK_HOLE);
            }
            if (Star_Name_Is_Shown_(star_idx)) {
                char name[sizeof(star->name) + 1] = {};
                memcpy(name, star->name, sizeof(star->name));
                return text::Normalize_Game_Text(name);
            }
            static const vocab::e_vocab_string colours[] = {
                vocab::VOCAB_STAR_BLUE, vocab::VOCAB_STAR_WHITE, vocab::VOCAB_STAR_YELLOW,
                vocab::VOCAB_STAR_ORANGE, vocab::VOCAB_STAR_RED, vocab::VOCAB_STAR_BROWN,
            };
            int8_t spectral = star->spectral_class;
            std::string colour = spectral >= 0 && spectral < (int8_t)(sizeof(colours) / sizeof(colours[0]))
                ? vocab::Get(colours[spectral])
                : std::string();
            return text::Normalize_Spoken_Text(vocab::Format(vocab::VOCAB_STAR_UNEXPLORED, colour));
        }

        // Eight-way compass direction; map y grows southward. tan(22.5 degrees) is about 0.414.
        vocab::e_vocab_string Direction_(int32_t dx, int32_t dy) {
            int32_t ax = dx < 0 ? -dx : dx;
            int32_t ay = dy < 0 ? -dy : dy;
            if (ay * 1000 <= ax * 414) {
                return dx > 0 ? vocab::VOCAB_DIRECTION_EAST : vocab::VOCAB_DIRECTION_WEST;
            }
            if (ax * 1000 <= ay * 414) {
                return dy > 0 ? vocab::VOCAB_DIRECTION_SOUTH : vocab::VOCAB_DIRECTION_NORTH;
            }
            if (dy < 0) {
                return dx > 0 ? vocab::VOCAB_DIRECTION_NORTH_EAST : vocab::VOCAB_DIRECTION_NORTH_WEST;
            }
            return dx > 0 ? vocab::VOCAB_DIRECTION_SOUTH_EAST : vocab::VOCAB_DIRECTION_SOUTH_WEST;
        }

        std::string Star_Distance_(int16_t star_idx, int16_t capital) {
            if (capital < 0) {
                return std::string();
            }
            if (star_idx == capital) {
                return vocab::Get(vocab::VOCAB_STAR_CAPITAL);
            }
            int32_t parsecs = GEO::Parsecs_Between_Stars(capital, star_idx);
            std::string direction = vocab::Get(Direction_(MOX::star[star_idx].x - MOX::star[capital].x,
                MOX::star[star_idx].y - MOX::star[capital].y));
            return parsecs == 1
                ? vocab::Format(vocab::VOCAB_DISTANCE_ONE, direction)
                : vocab::Format(vocab::VOCAB_DISTANCE, std::to_string(parsecs), direction);
        }

        // Ship icons are built from stacks that Remove_Non_Detected_Ships already filtered, so
        // they are exactly the fleets the player can see.
        std::string Star_Fleets_(int16_t star_idx) {
            int32_t count = 0;
            for (int16_t i = 0; i < MOX::ship_icon_count; ++i) {
                if (MOX::ship_icon[i].star_idx == star_idx) {
                    ++count;
                }
            }
            if (count == 0) {
                return std::string();
            }
            return count == 1 ? std::string(vocab::Get(vocab::VOCAB_FLEET_COUNT_ONE))
                              : vocab::Format(vocab::VOCAB_FLEET_COUNT, std::to_string(count));
        }

        // The main loop hit-tests the pointer when its grid is pressed: fleet icons win over stars
        // unless the fleet box is open, in which case stars win (mainscr_main.cpp).
        bool Point_Picks_Star_(int16_t x, int16_t y, int16_t star_idx) {
            const s_field* grid = &fields::fields[MOX::main_screen_grid_field];
            if (x < grid->x || x > grid->x_end || y < grid->y || y > grid->y_end) {
                return false;
            }
            int16_t star_hit = MAINSCR::Find_Star_At_XY(x, y);
            if (MOVEBOX::Moveable_Box_Selected(2)) {
                return star_hit == star_idx;
            }
            return star_hit == star_idx && MAINSCR::Find_Ship_Icon_At_XY(x, y) == -1;
        }

        constexpr int16_t STAR_CLICK_SEARCH_RADIUS = 12;

        // The star's centre when it resolves to the star, else the nearest point that does; a
        // fleet icon parked beside the star or a neighbouring star can cover the centre.
        bool Star_Click_Point_(int16_t star_idx, int16_t* out_x, int16_t* out_y) {
            if (!MAINSCR::Star_On_Screen(star_idx)) {
                return false;
            }
            int16_t cx = 0;
            int16_t cy = 0;
            MAINSCR::Get_Star_Draw_Coords(star_idx, &cx, &cy);
            for (int16_t ring = 0; ring <= STAR_CLICK_SEARCH_RADIUS; ++ring) {
                for (int16_t dy = (int16_t)-ring; dy <= ring; ++dy) {
                    for (int16_t dx = (int16_t)-ring; dx <= ring; ++dx) {
                        if (dx != -ring && dx != ring && dy != -ring && dy != ring) {
                            continue;
                        }
                        if (Point_Picks_Star_((int16_t)(cx + dx), (int16_t)(cy + dy), star_idx)) {
                            *out_x = (int16_t)(cx + dx);
                            *out_y = (int16_t)(cy + dy);
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        // Fleet icon hit rectangles are cached by the last draw, so after the view moves the
        // press waits until a draw has refreshed them.
        bool Ship_Icons_Are_Drawn_() {
            for (int16_t i = 0; i < MOX::ship_icon_count; ++i) {
                int16_t x = 0;
                int16_t y = 0;
                SHIPS::Get_Ship_Icon_Coords(i, &x, &y);
                if (MOX::ship_icon[i].x != x || MOX::ship_icon[i].y != y) {
                    return false;
                }
            }
            return true;
        }

        bool Press_Star_(int16_t star_idx) {
            int16_t x = 0;
            int16_t y = 0;
            if (!Star_Click_Point_(star_idx, &x, &y)) {
                return false;
            }
            platform::Force_Cursor_Pos(x, y);
            navigator::Request_Field_Activation(MOX::main_screen_grid_field);
            return true;
        }

        // A star the view was moved to, waiting for the redraw before it is pressed.
        int16_t pending_star = -1;
        int32_t pending_star_ticks = 0;

        void Await_Star_(int16_t star_idx);
        std::string Star_Eta_Text_(int16_t star_idx);

        void Say_Star_Unreachable_(int16_t star_idx) {
            speech::Say(vocab::Format(vocab::VOCAB_STAR_UNREACHABLE, Star_Label_(star_idx)), true);
        }

        // The player never has to manage the view: an off-screen star is brought into view the
        // way the game's own mini-map does it, then pressed once the map has been redrawn.
        void Activate_Star_(int16_t star_idx) {
            pending_star = -1;
            if (Ship_Icons_Are_Drawn_() && Press_Star_(star_idx)) {
                Await_Star_(star_idx);
                return;
            }
            if (!MAINSCR::Star_On_Screen(star_idx)) {
                MAINSCR::Center_Map(MOX::star[star_idx].x, MOX::star[star_idx].y);
            }
            pending_star = star_idx;
            pending_star_ticks = 0;
        }

        void Poll_Pending_Star_() {
            if (pending_star < 0) {
                return;
            }
            ++pending_star_ticks;
            bool timed_out = pending_star_ticks >= PENDING_PRESS_MAX_TICKS;
            if (pending_star_ticks < 2 || (!Ship_Icons_Are_Drawn_() && !timed_out)) {
                return;
            }
            int16_t star_idx = pending_star;
            pending_star = -1;
            if (Press_Star_(star_idx)) {
                Await_Star_(star_idx);
            } else {
                Say_Star_Unreachable_(star_idx);
            }
        }

        void Add_Star_Item_(graph::GraphBuilder* builder, s_build_context* context, int16_t star_idx, int16_t capital) {
            char key[24];
            snprintf(key, sizeof(key), "star/%d", (int)star_idx);

            graph::NodeVtable vtable;
            vtable.Announcements.push_back(graph::NodeAnnouncement::Static(Star_Label_(star_idx)));
            vtable.Announcements.back().Kind = Label;

            int16_t owner = Star_Shown_Owner_(star_idx);
            if (owner >= 0) {
                char race[sizeof(MOX::player[owner].race_name) + 1] = {};
                memcpy(race, MOX::player[owner].race_name, sizeof(MOX::player[owner].race_name));
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(text::Normalize_Game_Text(race)));
            }
            std::string distance = Star_Distance_(star_idx, capital);
            if (!distance.empty()) {
                vtable.Announcements.push_back(graph::NodeAnnouncement::Static(distance));
            }
            vtable.Announcements.push_back(graph::NodeAnnouncement([star_idx]() { return Star_Fleets_(star_idx); }, true, Value));
            vtable.Announcements.push_back(graph::NodeAnnouncement([star_idx]() { return Star_Eta_Text_(star_idx); }, true, Value));

            int16_t x = 0;
            int16_t y = 0;
            if (Star_Click_Point_(star_idx, &x, &y)) {
                context->node_points[key] = s_pointer_target{ x, y };
            }
            vtable.OnActivate = [star_idx]() { Activate_Star_(star_idx); };

            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        bool Is_Galaxy_Panel_Field_(int16_t field_idx) {
            const int16_t panel[] = {
                MAINSCR::game_button, MAINSCR::next_button, MAINSCR::zoom_in_button, MAINSCR::zoom_out_button,
                MAINSCR::colonies_button, MAINSCR::planets_button, MAINSCR::fleets_button,
                MAINSCR::leaders_button, MAINSCR::races_button, MAINSCR::info_button,
                MAINSCR::treasury_window_field, MAINSCR::command_points_window_field, MAINSCR::food_window_field,
                MAINSCR::freighters_window_field, MAINSCR::research_window_field,
            };
            for (int16_t panel_field : panel) {
                if (panel_field == field_idx) {
                    return true;
                }
            }
            return false;
        }

        const char* const GALAXY_SYSTEM_STOP = "galaxy/system";
        const char* const GALAXY_FLEET_STOP = "galaxy/fleet";

        int16_t Open_System_() {
            if (!MOVEBOX::Moveable_Box_Selected(0)) {
                return -1;
            }
            int16_t system = MOX::moveable_box[0].id;
            return system >= 0 && system < MOX::NUM_STARS ? system : -1;
        }

        int16_t Open_Fleet_Stack_() {
            if (!MOVEBOX::Moveable_Box_Selected(2)) {
                return -1;
            }
            int16_t stack = MOX::fleet_box_ship_stack;
            return stack >= 0 && stack < MOX::ship_stack_count ? stack : -1;
        }

        std::string Fleet_Box_Label_(int16_t stack) {
            int16_t star_idx = SHIPSTAK::Ship_Stack_Star_Id(MOX::ship_stack_start[stack]);
            return star_idx >= 0 && star_idx < MOX::NUM_STARS
                ? vocab::Format(vocab::VOCAB_FLEET_DISPLAY_AT, Star_Label_(star_idx))
                : std::string(vocab::Get(vocab::VOCAB_FLEET_DISPLAY));
        }

        // ---- Fleet box -----------------------------------------------------------------------------

        // Mirrors FLEETPOP::Get_Fleetpop_Scanned_Ship_Info: the player's ships by
        // design name, other empires' warships only by hull.
        std::string Fleet_Icon_Name_(int16_t icon_idx) {
            int16_t ship_idx = HACCESS::Fleet_Movement_Ship_Id(icon_idx);
            const s_ship_data* ship = &MOX::ship[ship_idx];
            int16_t owner = HACCESS::Ship_Owner(ship_idx);
            if (owner != MOX::PLAYER_NUM && owner <= 8 && HACCESS::Is_Combat_Ship(ship_idx)) {
                return text::Normalize_Game_Text(TECHDATA::hull_data[(int8_t)ship->d.size].name);
            }
            char name[sizeof(ship->d.name) + 1] = {};
            memcpy(name, ship->d.name, sizeof(ship->d.name));
            return text::Normalize_Game_Text(name);
        }

        // The status line FLEETPOP::Print_Fleet_Box_Info draws under the ships. The draw adopts the
        // last chosen icon as the scanned ship one frame before it prints it; this reads the
        // settled result without changing anything.
        std::string Fleet_Box_Status_Text_() {
            if (!MOVEBOX::Moveable_Box_Selected(2) || MOX::fleet_box.count <= 0) {
                return std::string();
            }
            int16_t ship_idx = HACCESS::Fleet_Movement_Ship_Id(0);
            bool icon_selected = FLEETPOP::Some_Icon_Selected();
            int16_t scanned = MOX::fleet_ship_scanned;
            int16_t info_type = 9;
            int16_t value = -1;
            int16_t destination = -1;

            if (MOX::eta_star_id == -1) {
                if (scanned == -1 && !icon_selected && MAINSCR::fleet_icon_chosen != -1) {
                    scanned = MAINSCR::fleet_icon_chosen;
                }
                if (scanned != -1) {
                    info_type = 0;
                } else if (MOX::ship[ship_idx].status == 0) {
                    info_type = 1;
                    value = MOX::ship[ship_idx].location;
                } else {
                    FLEETPOP::Get_Absolute_Location_Scan_Info(ship_idx, &value, &destination, &info_type);
                }
            } else if (MOX::eta_star_id > -1 && icon_selected) {
                const s_ship_move_info* move = &MOX::g_ship_move_info;
                bool visited = HAROLD::Player_Has_Visited(MOX::PLAYER_NUM, MOX::eta_star_id) != 0;
                if (move->moving != 0) {
                    if (move->turns_left == 0) {
                        info_type = 1;
                        value = MOX::eta_star_id;
                    } else {
                        info_type = visited ? 2 : 3;
                        value = (uint8_t)move->turns_left;
                        destination = MOX::eta_star_id;
                    }
                } else if (move->blackhole_blocks != 0) {
                    info_type = 4;
                } else if (move->immobile != 0) {
                    info_type = 5;
                } else if (move->out_of_range != 0) {
                    info_type = visited ? 6 : 7;
                    value = (uint8_t)move->parsecs;
                    destination = MOX::eta_star_id;
                } else if (move->hyperspace_flux != 0) {
                    info_type = 8;
                }
            }

            char text[200] = {};
            const char* star_name = destination >= 0 && destination < MOX::NUM_STARS ? MOX::star[destination].name : "";
            switch (info_type) {
                case 0: return scanned >= 0 && scanned < MOX::fleet_box.count ? Fleet_Icon_Name_(scanned) : std::string();
                case 1: snprintf(text, sizeof(text), HAROLD::H_Message(0x67), MOX::star[value].name); break;
                case 2:
                    if (destination == MOX::NUM_STARS) {
                        strlcpy(text, HAROLD::H_Message(0x68), sizeof(text));
                    } else {
                        snprintf(text, sizeof(text), HAROLD::H_Message(value == 1 ? 0x69 : 0x6a), (int)value, star_name);
                    }
                    break;
                case 3: snprintf(text, sizeof(text), HAROLD::H_Message(value == 1 ? 0x6b : 0x6c), (int)value); break;
                case 4: strlcpy(text, HAROLD::H_Message(0x6d), sizeof(text)); break;
                case 5: strlcpy(text, HAROLD::H_Message(0x6e), sizeof(text)); break;
                case 6: snprintf(text, sizeof(text), HAROLD::H_Message(value == 1 ? 0x6f : 0x70), (int)value, star_name); break;
                case 7:
                    if (value == 0) {
                        strlcpy(text, HAROLD::H_Message(0x71), sizeof(text));
                    } else {
                        snprintf(text, sizeof(text), HAROLD::H_Message(0xeb), (int)value);
                    }
                    break;
                case 8: strlcpy(text, HAROLD::H_Message(0x73), sizeof(text)); break;
                default: break;
            }
            return text::Normalize_Game_Text(text);
        }

        // The ETA line the fleet box shows while the pointer rests on this star with ships selected.
        std::string Star_Eta_Text_(int16_t star_idx) {
            if (!MOVEBOX::Moveable_Box_Selected(2) || MOX::eta_star_id != star_idx || MOX::fleet_ship_scanned != -1) {
                return std::string();
            }
            return Fleet_Box_Status_Text_();
        }

        int16_t Visible_Fleet_Icon_Field_(int16_t icon_idx) {
            int16_t end = (int16_t)(MOX::fleet_icon_buttons_added + MOX::first_fleet_movement_icon);
            if (end > MOX::fleet_box.count) {
                end = MOX::fleet_box.count;
            }
            if (icon_idx < MOX::first_fleet_movement_icon || icon_idx >= end) {
                return 0;
            }
            int16_t field_idx = MOX::fleet_movement_icon[icon_idx].field;
            return Is_Valid_Field(field_idx) ? field_idx : 0;
        }

        // The game selects ships only while the left button is physically held over an icon: the
        // main screen's draw paints the first icon's flipped state across each icon it passes
        // (mainscr_main.cpp). This makes the same call for one icon. A ship scrolled out of the box
        // has no field to paint, so it gets the rule Set_Painted_Fleet_Fields applies.
        void Toggle_Fleet_Ship_(int16_t icon_idx) {
            if (!MOVEBOX::Moveable_Box_Selected(2) || icon_idx < 0 || icon_idx >= MOX::fleet_box.count ||
                !FLEETPOP::Can_Select_Fleet_Box_Ship(0)) {
                return;
            }
            int16_t field_idx = Visible_Fleet_Icon_Field_(icon_idx);
            if (field_idx > 0) {
                MAINSCR::fltpop_field_selection_status = (uint8_t)(FLEETPOP::Icon_Selection_Status(field_idx) == 0);
                MAINSCR::Set_Painted_Fleet_Fields(field_idx);
                return;
            }
            s_ship_node* node = &MOX::ship_node[MOX::fleet_movement_icon[icon_idx].ship_node_idx];
            if (MAINSCR::Ship_Can_Be_Selected(node->ship_idx)) {
                node->selected = (int8_t)(node->selected == 0);
            }
        }

        std::string Fleet_Ship_State_(int16_t icon_idx) {
            if (!MOVEBOX::Moveable_Box_Selected(2) || icon_idx >= MOX::fleet_box.count) {
                return std::string();
            }
            const s_ship_node* node = &MOX::ship_node[MOX::fleet_movement_icon[icon_idx].ship_node_idx];
            return vocab::Get(node->selected != 0 ? vocab::VOCAB_STATE_SELECTED : vocab::VOCAB_STATE_NOT_SELECTED);
        }

        bool Is_Fleet_Box_Field_(int16_t field_idx) {
            const s_movable_box* box = &MOX::moveable_box[2];
            const int16_t box_fields[] = {
                box->close_btn, box->drag_field, box->window_field_id, MOX::fleet_movement_all_button,
                MOX::fleet_box_scroll_up_button, MOX::fleet_box_scroll_dn_button, MOX::fleet_movement_bar_field,
                MOX::fleet_box_outpost_button, MOX::fleet_box_colonize_button, MOX::fleet_box_engage_button,
                MOX::fleet_box_transport_button, MOX::fleet_box_attack_button,
            };
            for (int16_t box_field : box_fields) {
                if (box_field == field_idx) {
                    return true;
                }
            }
            for (int16_t i = 0; i < MOX::fleet_box.count; ++i) {
                if (Visible_Fleet_Icon_Field_(i) == field_idx) {
                    return true;
                }
            }
            return false;
        }

        void Build_Fleet_Stop_(graph::GraphBuilder* builder, s_build_context* context, const char* stop, int16_t stack) {
            char prefix[48];
            snprintf(prefix, sizeof(prefix), "%s/%d", stop, (int)stack);
            std::string base = prefix;

            graph::NodeVtable status;
            status.Announcements.push_back(graph::NodeAnnouncement([]() { return Fleet_Box_Status_Text_(); }, true, Label));
            builder->AddItem(graph::ControlId::Structural(base + "/status"), status);

            bool own = FLEETPOP::Can_Select_Fleet_Box_Ship(0);
            for (int16_t i = 0; i < MOX::fleet_box.count; ++i) {
                int16_t ship_idx = HACCESS::Fleet_Movement_Ship_Id(i);
                std::string key = base + "/ship/" + std::to_string(ship_idx);
                graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ Fleet_Icon_Name_(i) });
                int16_t field_idx = Visible_Fleet_Icon_Field_(i);
                if (own) {
                    vtable.Type = &toggle_type;
                    vtable.Announcements.push_back(graph::NodeAnnouncement([i]() { return Fleet_Ship_State_(i); }, true, Value));
                    vtable.OnActivate = [i]() { Toggle_Fleet_Ship_(i); };
                    // A right click on the player's own ship opens its detailed view.
                    if (field_idx > 0) {
                        vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
                    }
                }
                if (field_idx > 0) {
                    context->node_fields[key] = field_idx;
                    vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
                }
                builder->AddItem(graph::ControlId::Structural(key), vtable);
            }

            if (Is_Valid_Field(MOX::fleet_movement_all_button)) {
                Add_Field_Item(builder, context, base + "/all", MOX::fleet_movement_all_button,
                    Help_Label_Or(MOX::fleet_movement_all_button, vocab::VOCAB_FLEET_ALL));
            }
            const struct {
                int8_t active;
                int16_t field;
                const char* key;
            } actions[] = {
                { MOX::fleet_box.colonize_btn_active, MOX::fleet_box_colonize_button, "/colonize" },
                { MOX::fleet_box.outpost_btn_active, MOX::fleet_box_outpost_button, "/outpost" },
                { MOX::fleet_box.transport_btn_active, MOX::fleet_box_transport_button, "/transport" },
                { MOX::fleet_box.engage_btn_active, MOX::fleet_box_engage_button, "/engage" },
                { MOX::fleet_box.attack_btn_active, MOX::fleet_box_attack_button, "/attack" },
            };
            for (const auto& action : actions) {
                if (action.active != 0 && Is_Valid_Field(action.field)) {
                    Add_Field_Item(builder, context, base + action.key, action.field, Field_Label(action.field));
                }
            }
            if (Is_Valid_Field(MOX::moveable_box[2].close_btn)) {
                Add_Field_Item(builder, context, base + "/close", MOX::moveable_box[2].close_btn, vocab::Get(vocab::VOCAB_CLOSE));
            }
        }

        // ---- System display ----------------------------------------------------------------------

        std::string Star_Name_(int16_t star_idx) {
            char name[sizeof(MOX::star[star_idx].name) + 1] = {};
            memcpy(name, MOX::star[star_idx].name, sizeof(MOX::star[star_idx].name));
            return text::Normalize_Game_Text(name);
        }

        std::string Race_Name_(int16_t player_idx) {
            char race[sizeof(MOX::player[player_idx].race_name) + 1] = {};
            memcpy(race, MOX::player[player_idx].race_name, sizeof(MOX::player[player_idx].race_name));
            return race;
        }

        // The lines SYS::Do_Print_Planet_Info draws when the pointer is over a planet, which can't
        // be called here because it draws as it formats. The first line is the planet's name.
        std::vector<std::string> Planet_Info_Lines_(int16_t planet_idx) {
            std::vector<std::string> lines;
            const s_planet_data* planet = &MOX::planet[planet_idx];
            char line[200];

            if (planet->planet_type == PLANET_TYPE_GAS_GIANT) {
                lines.push_back(H_Text(0x15f));
                return lines;
            }
            if (planet->planet_type == PLANET_TYPE_ASTEROID) {
                lines.push_back(H_Text(0x160));
                return lines;
            }

            const s_colony* colony = planet->colony_index > -1 ? &MOX::colony[planet->colony_index] : nullptr;
            uint8_t climate = (uint8_t)(colony != nullptr ? colony->climate : planet->climate);

            char name[64] = {};
            HACCESS::Do_Get_Planet_Name(planet_idx, name, 0);
            std::string title = name;
            if (colony != nullptr) {
                title += " (" + Race_Name_((int8_t)colony->owner);
                title += colony->outpost_flag != 0 ? std::string(HAROLD::H_Message(0x161)) : std::string(")");
            }
            lines.push_back(text::Normalize_Game_Text(title.c_str()));

            snprintf(line, sizeof(line), "%s, %s", MOX::planet_size_string[(int8_t)planet->size], MOX::planet_climate_string[climate]);
            lines.push_back(text::Normalize_Game_Text(line));

            int16_t max_population = COLCALC::Planet_Max_Population_For_Player(const_cast<s_planet_data*>(planet), MOX::PLAYER_NUM);
            if (colony != nullptr) {
                snprintf(line, sizeof(line), HAROLD::H_Message(0x181), (uint32_t)(uint8_t)colony->n_pops, (int32_t)max_population);
            } else {
                snprintf(line, sizeof(line), HAROLD::H_Message(0x162), (int32_t)max_population);
            }
            lines.push_back(text::Normalize_Game_Text(line));

            if ((int8_t)planet->gravity_class == PLANET_GRAVITY_NORMAL_G) {
                snprintf(line, sizeof(line), "%s", MOX::mineral_class_string[(int8_t)planet->mineral_class]);
            } else {
                snprintf(line, sizeof(line), "%s, %s", MOX::mineral_class_string[(int8_t)planet->mineral_class],
                    MOX::planet_gravity_string[(int8_t)planet->gravity_class]);
            }
            lines.push_back(text::Normalize_Game_Text(line));

            // Mirrors SYS::Print_Special_Building_Data and the visibility rule that gates it.
            bool can_show_buildings = MOX::player_has_ship_in_system != 0 ||
                HAROLD::Player_Has_Colony(MOX::PLAYER_NUM, planet->star_index) ||
                MOX::player[MOX::PLAYER_NUM].traits[TRAIT_OMNISCIENCE] != 0;
            if (colony != nullptr && can_show_buildings) {
                const int16_t buildings[] = {
                    BUILDING_STAR_FORTRESS, BUILDING_STAR_BASE, BUILDING_BATTLESTATION, BUILDING_MISSILE_BASE,
                    BUILDING_GROUND_BATTERIES, BUILDING_STELLAR_CONVERTER, BUILDING_FIGHTER_GARRISON,
                };
                for (int16_t building : buildings) {
                    if (colony->buildings[building] != 0) {
                        lines.push_back(text::Normalize_Game_Text(TECHDATA::buildings[building].name));
                    }
                }
            }

            int32_t special = (int8_t)planet->planet_special;
            if (special != 0 && special != 0xb) {
                lines.push_back(text::Normalize_Game_Text(MOX::planet_special_string[special]));
            }
            return lines;
        }

        // The lines SYS::Do_Print_System_Ship_Data draws for a fleet icon in the system display.
        std::vector<std::string> System_Fleet_Lines_(int16_t ship_node_num) {
            std::vector<std::string> lines;
            char line[200];

            int16_t node = ship_node_num >= SHIP_NODE_UI_TAG_BASE ? (int16_t)(ship_node_num - SHIP_NODE_UI_TAG_BASE) : ship_node_num;
            int16_t ship_idx = MOX::ship_node[node].ship_idx;
            int16_t owner = HACCESS::Ship_Owner(ship_idx);
            if (ship_node_num >= SHIP_NODE_UI_TAG_BASE || owner >= MAX_PLAYERS) {
                char name[sizeof(MOX::ship[ship_idx].d.name) + 1] = {};
                memcpy(name, MOX::ship[ship_idx].d.name, sizeof(MOX::ship[ship_idx].d.name));
                lines.push_back(text::Normalize_Game_Text(name));
                return lines;
            }

            int16_t total = 0;
            int16_t colony_ships = 0;
            int16_t outposts = 0;
            int16_t transports = 0;
            int16_t hulls[SHIP_SIZE_DOOM_STAR + 1];
            SYS::N_Ships_By_Type_In_Stack(ship_node_num, &total, &colony_ships, &outposts, &transports, hulls);

            snprintf(line, sizeof(line), HAROLD::H_Message(0x163), Race_Name_(owner).c_str());
            lines.push_back(text::Normalize_Game_Text(line));
            for (int16_t hull = 0; hull <= SHIP_SIZE_DOOM_STAR; ++hull) {
                if (hulls[hull] > 0) {
                    const char* hull_name = hulls[hull] == 1 ? TECHDATA::hull_data[hull].name : TECHDATA::hull_data[hull].size_name;
                    snprintf(line, sizeof(line), "%d %s", (int32_t)hulls[hull], hull_name);
                    lines.push_back(text::Normalize_Game_Text(line));
                }
            }
            const int16_t counts[] = { colony_ships, outposts, transports };
            const int16_t one_messages[] = { 0x164, 0x166, 0x168 };
            for (int32_t i = 0; i < 3; ++i) {
                if (counts[i] > 0) {
                    snprintf(line, sizeof(line), HAROLD::H_Message(counts[i] == 1 ? one_messages[i] : (int16_t)(one_messages[i] + 1)),
                        (int32_t)counts[i]);
                    lines.push_back(text::Normalize_Game_Text(line));
                }
            }
            return lines;
        }

        // SYS::Print_Gate_Info: gate_slot 0 is a dimensional portal, 3 an Artemis net, each naming
        // the races that own one here.
        std::string Gate_Text_(int16_t star_idx, int16_t gate_slot) {
            std::string text = H_Text(gate_slot == 0 ? 0x17C : 0x17B);
            std::string owners;
            for (int16_t i = 0; i < MOX::NUM_PLAYERS; ++i) {
                bool owns = gate_slot == 0
                    ? HAROLD::Player_Has_Dimensional_Portal(&MOX::player[i], &MOX::star[star_idx])
                    : HAROLD::Player_Has_Artemis_Net(&MOX::player[i], &MOX::star[star_idx]);
                if (owns) {
                    owners += (owners.empty() ? "" : ", ") + Race_Name_(i);
                }
            }
            return text::Normalize_Spoken_Text(text + owners + ")");
        }

        // The wormhole line SYS::Draw_System_Display_Popup prints along the bottom of the display.
        std::string Wormhole_Text_(int16_t star_idx) {
            int16_t other = MOX::star[star_idx].wormhole_star_id;
            if (other < 0 || other >= MOX::NUM_STARS) {
                return std::string();
            }
            if (!HAROLD::Player_Has_Visited(MOX::PLAYER_NUM, other)) {
                return H_Text(0x16d);
            }
            char line[200];
            snprintf(line, sizeof(line), HAROLD::H_Message(0x16c), Star_Name_(other).c_str());
            return text::Normalize_Game_Text(line);
        }

        // An asteroid belt has no field of its own: the main loop hit-tests the pointer against
        // the orbits when the display's grid is pressed (MAINSCR::Scan_System_Display_Fields).
        bool Asteroid_Point_(int32_t orbit, int16_t* out_x, int16_t* out_y) {
            int16_t grid_idx = MOX::system_display_grid_field;
            if (!Is_Valid_Field(grid_idx)) {
                return false;
            }
            const s_field* grid = &fields::fields[grid_idx];
            int16_t cx = (int16_t)(MOX::system_display_base_x + 171);
            int16_t cy = (int16_t)(MOX::system_display_base_y + 134);
            for (int16_t step = 1; step < 200; ++step) {
                const int16_t candidates[2] = { (int16_t)(cx - step), (int16_t)(cx + step) };
                for (int16_t x : candidates) {
                    if (x < grid->x || x > grid->x_end || cy < grid->y || cy > grid->y_end) {
                        continue;
                    }
                    if (GEO::Get_Orbit_From_X_Y(cx - x, 0) == orbit) {
                        *out_x = x;
                        *out_y = cy;
                        return true;
                    }
                }
            }
            return false;
        }

        // Pressing a field the game reads as a click: a planet opens its colony screen when it is
        // the player's, a fleet opens its fleet box. A right click on a planet other than a gas
        // giant opens its colony potential (MAINSCR::Scan_System_Display_Fields).
        void Add_System_Field_Node_(graph::GraphBuilder* builder, s_build_context* context, const std::string& key,
            int16_t field_idx, const std::vector<std::string>& lines, const graph::ControlType* type, bool has_secondary) {
            graph::NodeVtable vtable = Info_Vtable(lines);
            vtable.Type = type;
            vtable.OnActivate = [field_idx]() { navigator::Request_Field_Activation(field_idx); };
            if (has_secondary) {
                vtable.OnSecondary = [field_idx]() { navigator::Request_Field_Secondary(field_idx); };
            }
            vtable.OnTooltip = [field_idx]() { Speak_Field_Help(field_idx); };
            context->node_fields[key] = field_idx;
            builder->AddItem(graph::ControlId::Structural(key), vtable);
        }

        void Add_System_Text_Node_(graph::GraphBuilder* builder, const std::string& key, const std::string& text) {
            builder->AddItem(graph::ControlId::Structural(key), Info_Vtable(std::vector<std::string>{ text }));
        }

        // The display's own fields, which the system stop lists in its own order; everything else
        // in the box (title bar, background, orbit grid) is a drag or click target with no meaning.
        bool Is_System_Field_(int16_t field_idx) {
            const s_movable_box* box = &MOX::moveable_box[0];
            if (field_idx == box->close_btn || field_idx == box->drag_field || field_idx == box->window_field_id ||
                field_idx == MOX::system_display_grid_field) {
                return true;
            }
            for (int16_t i = 0; i < 5; ++i) {
                if (field_idx == MOX::planet_buttons[i]) {
                    return true;
                }
            }
            for (int16_t i = 0; i < 15; ++i) {
                if (MOX::system_display_ships[i].ship_idx > -1 && field_idx == MOX::system_display_ships[i].button) {
                    return true;
                }
            }
            for (int16_t i = 0; i < 4; ++i) {
                if (field_idx == MOX::gate_icon_field[i]) {
                    return true;
                }
            }
            return false;
        }

        void Build_System_Stop_(graph::GraphBuilder* builder, s_build_context* context, const char* stop, int16_t star_idx,
            int16_t close_field, const std::string& close_label) {
            char prefix[48];
            snprintf(prefix, sizeof(prefix), "%s/%d", stop, (int)star_idx);
            std::string base = prefix;

            if (MOX::look_at_colonies == 0 && !SYS::Ok_To_View_System(star_idx, MOX::PLAYER_NUM)) {
                if (!HAROLD::Player_Sighted_Monster(MOX::PLAYER_NUM, star_idx) && SYS::star_description_string[0] != '\0') {
                    Add_System_Text_Node_(builder, base + "/description", text::Normalize_Game_Text(SYS::star_description_string));
                }
            } else {
                // Planets inner orbit first; the display stores them sorted by drawing depth.
                std::vector<int16_t> slots;
                for (int16_t slot = 0; slot < 5; ++slot) {
                    if (MOX::sys_disp != nullptr && MOX::sys_disp[slot].orbit > -1) {
                        slots.push_back(slot);
                    }
                }
                std::stable_sort(slots.begin(), slots.end(), [](int16_t a, int16_t b) {
                    return MOX::planet[MOX::sys_disp[a].orbit].orbit < MOX::planet[MOX::sys_disp[b].orbit].orbit;
                });
                for (int16_t slot : slots) {
                    int16_t planet_idx = MOX::sys_disp[slot].orbit;
                    std::string key = base + "/planet/" + std::to_string(planet_idx);
                    std::vector<std::string> lines = Planet_Info_Lines_(planet_idx);
                    if (MOX::planet[planet_idx].planet_type == PLANET_TYPE_ASTEROID) {
                        int16_t x = 0;
                        int16_t y = 0;
                        if (Asteroid_Point_(MOX::planet[planet_idx].orbit, &x, &y)) {
                            context->node_points[key] = s_pointer_target{ x, y };
                            Add_System_Field_Node_(builder, context, key, MOX::system_display_grid_field, lines, nullptr, false);
                            continue;
                        }
                        builder->AddItem(graph::ControlId::Structural(key), Info_Vtable(lines));
                    } else if (Is_Valid_Field(MOX::planet_buttons[slot])) {
                        Add_System_Field_Node_(builder, context, key, MOX::planet_buttons[slot], lines, nullptr,
                            MOX::planet[planet_idx].planet_type != PLANET_TYPE_GAS_GIANT);
                    } else {
                        builder->AddItem(graph::ControlId::Structural(key), Info_Vtable(lines));
                    }
                }

                // Build_System_Popup_Ships fills one slot per owner from the detection-filtered icons.
                for (int16_t slot = 0; slot < 15; ++slot) {
                    const s_system_display_ships* ships = &MOX::system_display_ships[slot];
                    if (ships->ship_idx <= -1 || ships->pic == nullptr || !Is_Valid_Field(ships->button)) {
                        continue;
                    }
                    Add_System_Field_Node_(builder, context, base + "/fleet/" + std::to_string(slot), ships->button,
                        System_Fleet_Lines_(ships->ship_idx), &button_type, false);
                }

                const int16_t gate_slots[] = { 3, 0 };
                for (int16_t gate_slot : gate_slots) {
                    int16_t field_idx = MOX::gate_icon_field[gate_slot];
                    if (!Is_Valid_Field(field_idx)) {
                        continue;
                    }
                    std::string key = base + "/gate/" + std::to_string(gate_slot);
                    graph::NodeVtable vtable = Info_Vtable(std::vector<std::string>{ Gate_Text_(star_idx, gate_slot) });
                    context->node_fields[key] = field_idx;
                    builder->AddItem(graph::ControlId::Structural(key), vtable);
                }

                std::string wormhole = Wormhole_Text_(star_idx);
                if (!wormhole.empty()) {
                    Add_System_Text_Node_(builder, base + "/wormhole", wormhole);
                }
            }

            if (Is_Valid_Field(close_field)) {
                Add_Field_Item(builder, context, base + "/close", close_field, close_label);
            }
        }

        void Galaxy_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            int16_t capital = Capital_Star_();

            std::vector<int16_t> stars;
            for (int16_t i = 0; i < MOX::NUM_STARS; ++i) {
                stars.push_back(i);
            }
            if (capital >= 0) {
                std::stable_sort(stars.begin(), stars.end(), [capital](int16_t a, int16_t b) {
                    int32_t da = a == capital ? -1 : GEO::Parsecs_Between_Stars(capital, a);
                    int32_t db = b == capital ? -1 : GEO::Parsecs_Between_Stars(capital, b);
                    return da < db;
                });
            }

            builder->BeginStop("galaxy/stars");
            builder->PushContext(vocab::Get(vocab::VOCAB_GALAXY_STARS));
            for (int16_t star_idx : stars) {
                Add_Star_Item_(builder, context, star_idx, capital);
            }
            builder->PopContext();

            builder->BeginStop("galaxy/controls");
            builder->PushContext(vocab::Get(vocab::VOCAB_GALAXY_CONTROLS));
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                if (Is_Galaxy_Panel_Field_(i)) {
                    char key[32];
                    snprintf(key, sizeof(key), "galaxy/control/%d,%d", (int)fields::fields[i].x, (int)fields::fields[i].y);
                    Add_Field_Item(builder, context, key, i, Field_Label(i));
                }
            }
            builder->PopContext();

            // The system display and the fleet box replace each other, so at most one is open. Keys
            // carry the system or stack, so a different one starts at its first control.
            int16_t system = Open_System_();
            if (system >= 0) {
                builder->BeginStop(GALAXY_SYSTEM_STOP);
                builder->PushContext(vocab::Format(vocab::VOCAB_SYSTEM_DISPLAY, Star_Label_(system)));
                Build_System_Stop_(builder, context, GALAXY_SYSTEM_STOP, system, MOX::moveable_box[0].close_btn, vocab::Get(vocab::VOCAB_CLOSE));
                builder->PopContext();
            }

            int16_t fleet_stack = system >= 0 ? -1 : Open_Fleet_Stack_();
            if (fleet_stack >= 0) {
                builder->BeginStop(GALAXY_FLEET_STOP);
                builder->PushContext(Fleet_Box_Label_(fleet_stack));
                Build_Fleet_Stop_(builder, context, GALAXY_FLEET_STOP, fleet_stack);
                builder->PopContext();
            }

            // Any other popup adds its fields to this loop's list.
            std::vector<int16_t> other;
            for (int16_t i = 1; i < fields::fields_count; ++i) {
                const s_field* field = &fields::fields[i];
                if (i == MOX::main_screen_grid_field || Is_Galaxy_Panel_Field_(i) || field->field_type == FIELD_TYPE_CIRCLE ||
                    Is_Catch_All(field) || !Field_Is_Listed(i, true) || (system >= 0 && Is_System_Field_(i)) ||
                    (fleet_stack >= 0 && Is_Fleet_Box_Field_(i))) {
                    continue;
                }
                other.push_back(i);
            }
            if (!other.empty()) {
                builder->BeginStop("galaxy/other");
                builder->PushContext(vocab::Get(vocab::VOCAB_GALAXY_OTHER));
                for (int16_t i : other) {
                    char key[64];
                    snprintf(key, sizeof(key), "galaxy/other/%d/%d,%d", (int)i, (int)fields::fields[i].x, (int)fields::fields[i].y);
                    Add_Field_Item(builder, context, key, i, Field_Label(i));
                }
                builder->PopContext();
            }

            if (!stars.empty()) {
                char start[24];
                snprintf(start, sizeof(start), "star/%d", (int)stars[0]);
                builder->SetStart(graph::ControlId::Structural(start));
            }
        }

        // The system display and fleet box open inside the main loop without moving focus, so the
        // poll asks for focus in them and the differ announces the landing (spec 7.3).
        int16_t polled_system = -1;
        int16_t polled_fleet_stack = -1;
        // A star pressed through its node: its system display takes focus even when it was
        // already open on that star, so Enter is never silent.
        int16_t awaited_star = -1;
        int32_t awaited_star_ticks = 0;

        void Await_Star_(int16_t star_idx) {
            awaited_star = star_idx;
            awaited_star_ticks = 0;
        }

        void Galaxy_Poll_(bool just_attached) {
            if (just_attached) {
                // Arriving on the screen lands on the capital; an already open box stays put.
                polled_system = Open_System_();
                polled_fleet_stack = Open_Fleet_Stack_();
                pending_star = -1;
                awaited_star = -1;
                return;
            }

            if (awaited_star >= 0) {
                ++awaited_star_ticks;
            }
            Poll_Pending_Star_();

            int16_t system = Open_System_();
            int16_t fleet_stack = Open_Fleet_Stack_();
            bool awaited_landed = awaited_star >= 0 && awaited_star_ticks >= 1 && system == awaited_star;

            if (system >= 0 && (system != polled_system || awaited_landed)) {
                navigator::Request_Focus_Stop(GALAXY_SYSTEM_STOP);
                awaited_star = -1;
            } else if (fleet_stack >= 0 && fleet_stack != polled_fleet_stack) {
                navigator::Request_Focus_Stop(GALAXY_FLEET_STOP);
                awaited_star = -1;
            }
            if (awaited_star >= 0 && awaited_star_ticks >= PENDING_PRESS_MAX_TICKS) {
                awaited_star = -1;
            }

            polled_system = system;
            polled_fleet_stack = fleet_stack;
        }

        // ---- Planet selection popup ----------------------------------------------------------------

        // MAINPUPS::New_Colony_Selection_Popup: a system display whose planets are pressed to choose
        // where a colony ship, colony base, outpost, troops, or artificial planet goes.
        bool Planet_Select_Is_Active_() {
            return MAINPUPS::selection_popup_awaiting_input != 0 && COLONY::sys_disp_star_handle >= 0 &&
                COLONY::sys_disp_star_handle < MOX::NUM_STARS;
        }

        // A system special notice Draw_Selection_Popup draws over the display until the next press.
        // A marooned hero draws nothing; the next press opens the officer level popup instead.
        bool Planet_Select_Special_Shown_() {
            return MAINPUPS::g_special != SYSTEM_SPECIAL_NO_SPECIAL && MAINPUPS::g_special != SYSTEM_SPECIAL_MAROONED_HERO &&
                MAINPUPS::g_special < SYSTEM_SPECIAL_COUNT;
        }

        std::string Planet_Select_Instance_Key_() {
            return "planet_select/" + std::to_string(COLONY::sys_disp_star_handle) + (Planet_Select_Special_Shown_() ? "/special" : "");
        }

        // The prompt Draw_Selection_Popup titles the display with.
        std::string Planet_Select_Name_() {
            int16_t string_id = -1;
            switch (MAINPUPS::selection_state_flag) {
                case 0:
                    string_id = MAINPUPS::g_using_colony_ship != 0 ? ESTR_SELECT_PLANET_FOR_COLONY_SHIP_IN_S_SYSTEM
                                                                   : ESTR_SELECT_PLANET_FOR_COLONY_BASE_IN_S_SYSTEM;
                    break;
                case 1: string_id = ESTR_SELECT_PLANET_FOR_AN_OUTPOST_IN_THE_S_SYSTEM; break;
                case 2: string_id = ESTR_SELECT_PLANET_FOR_TROOPS_IN_THE_S_SYSTEM; break;
                case 3: string_id = ESTR_SELECT_PLANET_FOR_ARTIFICIAL_PLANET_CONSTRUCTION_IN_THE_S_SYSTEM; break;
                default: break;
            }
            int16_t star_idx = MAINPUPS::g_star_n;
            if (string_id < 0 || star_idx < 0 || star_idx >= MOX::NUM_STARS) {
                return Star_Name_(COLONY::sys_disp_star_handle);
            }
            char prompt[300];
            snprintf(prompt, sizeof(prompt), ESTRINGS::E_Strings(string_id), MOX::star[star_idx].name);
            return text::Normalize_Game_Text(prompt);
        }

        void Planet_Select_Build_(graph::GraphBuilder* builder, s_build_context* context) {
            if (Planet_Select_Special_Shown_()) {
                uint8_t special = MAINPUPS::g_special;
                builder->AddItem(graph::ControlId::Structural("planet_select/special"), Info_Vtable(std::vector<std::string>{
                    text::Normalize_Game_Text(MOX::planet_special_string[special]),
                    text::Normalize_Game_Text(MAINPUPS::System_Special_Desc(special)) }));
                int16_t close = MAINPUPS::mp_fields[0];
                if (Is_Valid_Field(close)) {
                    Add_Field_Item(builder, context, "planet_select/continue", close, vocab::Get(vocab::VOCAB_CONTINUE));
                }
                return;
            }
            // Its close button is the popup's own cancel, not the galaxy display's.
            Build_System_Stop_(builder, context, "planet_select", COLONY::sys_disp_star_handle, MAINPUPS::mp_fields[2],
                vocab::Get(vocab::VOCAB_CANCEL));
        }

    } // namespace

    const s_screen_recipe planet_select_recipe = { "planet_select", Planet_Select_Is_Active_, Planet_Select_Instance_Key_, Planet_Select_Build_, Planet_Select_Name_ };
    const s_screen_recipe galaxy_recipe = { "galaxy", Galaxy_Is_Active_, Galaxy_Instance_Key_, Galaxy_Build_, Galaxy_Name_, Galaxy_Poll_ };

} // namespace access::screens

#include "pch.h"

#include "access/navigator.h"

#include "access/graph/GraphAnnouncer.hpp"
#include "access/graph/KeyGraph.hpp"
#include "access/screens.h"
#include "access/speech.h"
#include "access/vocab.h"

#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace access::navigator {

    namespace {

        constexpr int32_t KEY_QUEUE_SIZE = 64;
        // A queued key older than this was pressed during a load or a modal loop that never
        // polled fields; replaying it would press a key the user has forgotten about.
        constexpr uint64_t KEY_STALE_MS = 1000;
        // The game thread stops polling fields inside text-entry loops, animations, and loads;
        // keys pass through to the game once the last tick is this old.
        constexpr uint32_t TICK_STALE_MS = 300;
        // Idle differ cadence: rebuilding every frame is wasted work on a static menu (spec A2).
        constexpr uint64_t IDLE_REBUILD_MS = 100;
        constexpr size_t MAX_REMEMBERED_SCREENS = 64;

        struct s_key_event {
            SDL_Keycode key;
            SDL_Keymod modifiers;
            uint64_t time_ms;
        };

        // Written by the game thread, read by the platform thread's claim check.
        SDL_AtomicInt initialized;
        SDL_AtomicInt enabled;
        SDL_AtomicInt claims_navigation;
        // Backspace is claimed only while the focused control has a right-click equivalent.
        SDL_AtomicInt claims_secondary;
        SDL_AtomicU32 last_tick_ms;

        // Never destroyed: the platform thread can still offer keys after the game thread exits.
        SDL_Mutex* key_mutex = nullptr;
        s_key_event key_queue[KEY_QUEUE_SIZE];
        int32_t key_queue_head = 0;
        int32_t key_queue_count = 0;

        // Game thread only from here down.
        const screens::s_screen_recipe* attached = nullptr;
        std::string attached_instance;
        std::map<std::string, graph::GraphState> cursors;
        std::unique_ptr<graph::KeyGraph> key_graph;
        screens::s_build_context build_context;
        // The differ's memory: the focused identity last spoken (spec 7.3).
        graph::ControlId spoken;
        uint64_t last_rebuild_ms = 0;
        bool fault_logged = false;
        // Set on attach and cleared after the recipe's first poll.
        bool poll_just_attached = false;
        // The field a node asked to press, with what it looked like then: a screen that rebuilds its
        // list before the next poll must not have a different control pressed in its place.
        int16_t pending_activation = 0;
        bool pending_activation_secondary = false;
        s_field pending_activation_field;
        // A Tab-stop a recipe asked focus to move into, and how many differ passes it has waited.
        std::string pending_focus_stop;
        int32_t pending_focus_passes = 0;
        constexpr int32_t PENDING_FOCUS_MAX_PASSES = 30;
        // Toggles change state inside Get_Input after the tick, so their feedback waits a tick.
        bool pending_state_feedback = false;

        void Fault_(const char* where, const char* what) {
            if (fault_logged) {
                return;
            }
            fault_logged = true;
            const char* key = attached != nullptr ? attached->key : "?";
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "access: %s %s failed: %s", key, where, what);
            speech::Say(vocab::Format(vocab::VOCAB_FAULT, std::string(key) + " " + where, what), false);
        }

        // Keeps a throwing recipe or kernel misuse from reaching the game loop (spec 7.9).
        // The game's own exit request still propagates.
        bool Guarded_(const char* where, const std::function<void()>& fn) {
            try {
                fn();
                return true;
            } catch (const platform::process_exit_requested&) {
                throw;
            } catch (const std::exception& ex) {
                Fault_(where, ex.what());
            } catch (...) {
                Fault_(where, "unknown exception");
            }
            return false;
        }

        std::unique_ptr<graph::GraphRender> Render_() {
            std::unique_ptr<graph::GraphRender> render;
            if (attached == nullptr) {
                return render;
            }
            Guarded_("build", [&render]() {
                build_context.node_fields.clear();
                build_context.node_points.clear();
                graph::GraphBuilder builder(&cursors[attached_instance].Expanded);
                attached->Build(&builder, &build_context);
                render = builder.Build();
            });
            return render;
        }

        void Say_(const std::string& text, bool interrupt) {
            speech::Say(text, interrupt);
        }

        void Detach_() {
            attached = nullptr;
            attached_instance.clear();
            key_graph.reset();
            spoken = graph::ControlId();
            pending_state_feedback = false;
            pending_focus_stop.clear();
        }

        // Re-derives the active recipe from live state every tick (spec section 9).
        void Sync_() {
            const screens::s_screen_recipe* match = nullptr;
            std::string instance;
            int32_t count = 0;
            const screens::s_screen_recipe* recipes = screens::Recipes(&count);

            for (int32_t i = 0; i < count && match == nullptr; ++i) {
                bool active = false;
                Guarded_("is_active", [&]() { active = recipes[i].Is_Active(); });
                if (active) {
                    match = &recipes[i];
                    Guarded_("instance_key", [&]() { instance = match->Instance_Key(); });
                }
            }

            if (match == attached && instance == attached_instance) {
                return;
            }

            Detach_();
            fault_logged = false;
            if (match == nullptr) {
                return;
            }

            if (cursors.size() >= MAX_REMEMBERED_SCREENS && cursors.find(instance) == cursors.end()) {
                cursors.clear();
            }
            attached = match;
            poll_just_attached = true;
            attached_instance = instance;
            key_graph = std::make_unique<graph::KeyGraph>(Render_, &cursors[attached_instance]);
            last_rebuild_ms = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "access: attached %s (%s)", attached->key, attached_instance.c_str());

            std::string name;
            Guarded_("name", [&]() { name = attached->Name(); });
            Say_(name, false);
        }

        // P11: the game highlights the field under the pointer, so the pointer follows focus.
        void Move_Pointer_To_(const graph::GraphNode* node) {
            if (node == nullptr) {
                return;
            }
            auto point = build_context.node_points.find(node->Id.StructuralKey);
            if (point != build_context.node_points.end()) {
                if (point->second.x >= 0 && point->second.x < 640 && point->second.y >= 0 && point->second.y < 480) {
                    platform::Force_Cursor_Pos(point->second.x, point->second.y);
                }
                return;
            }
            auto it = build_context.node_fields.find(node->Id.StructuralKey);
            if (it == build_context.node_fields.end() || it->second <= 0 || it->second >= fields::fields_count) {
                return;
            }
            const s_field* field = &fields::fields[it->second];
            int16_t x = (int16_t)(field->x + (field->x_end - field->x) / 2);
            int16_t y = (int16_t)(field->y + (field->y_end - field->y) / 2);
            if (x >= 0 && x < 640 && y >= 0 && y < 480) {
                platform::Force_Cursor_Pos(x, y);
            }
        }

        // A keypress-driven move: interrupting, and the differ is told it has been said.
        void Announce_Move_(const graph::MoveResult& result) {
            if (!result.Moved || result.To == nullptr) {
                return;
            }
            std::string text;
            Guarded_("announce", [&]() { text = graph::GraphAnnouncer::Compose(result.From, result.To, result.TransitionLabel); });
            Say_(text, true);
            spoken = result.To->Id;
            Move_Pointer_To_(result.To);
        }

        void Speak_State_Feedback_() {
            if (key_graph == nullptr || !key_graph->Rerender()) {
                return;
            }
            graph::GraphNode* node = key_graph->CurrentNode();
            if (node == nullptr || node->Id != spoken) {
                return;
            }
            std::string text;
            Guarded_("state_text", [&]() {
                if (node->Vtable.StateText) {
                    text = node->Vtable.StateText();
                    return;
                }
                for (const graph::NodeAnnouncement& part : graph::GraphAnnouncer::EffectiveAnnouncements(node)) {
                    if (part.Live && part.Text) {
                        std::string value = part.Text();
                        if (!value.empty()) {
                            text = value;
                        }
                    }
                }
            });
            Say_(text, true);
        }

        void Where_Am_I_() {
            graph::GraphNode* node = key_graph->Rerender() ? key_graph->CurrentNode() : nullptr;
            std::string text;
            if (node != nullptr) {
                Guarded_("announce", [&]() { text = graph::GraphAnnouncer::ComposeFull(node); });
                spoken = node->Id;
            }
            std::string name;
            Guarded_("name", [&]() { name = attached->Name(); });
            Say_(name.empty() ? text : name + ", " + text, true);
        }

        void Dispatch_(SDL_Keycode key, SDL_Keymod modifiers) {
            bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;
            bool ctrl = (modifiers & SDL_KMOD_CTRL) != 0;

            if (ctrl) {
                if (key == SDLK_L) {
                    Where_Am_I_();
                }
                return;
            }

            switch (key) {
                case SDLK_UP:
                    Guarded_("move", [&]() { Announce_Move_(key_graph->Move(graph::GraphDir::Up)); });
                    break;
                case SDLK_DOWN:
                    Guarded_("move", [&]() { Announce_Move_(key_graph->Move(graph::GraphDir::Down)); });
                    break;
                case SDLK_LEFT:
                case SDLK_RIGHT: {
                    int sign = key == SDLK_LEFT ? -1 : 1;
                    bool adjusted = false;
                    Guarded_("adjust", [&]() { adjusted = key_graph->TryAdjust(sign, shift); });
                    if (adjusted) {
                        pending_state_feedback = true;
                    } else {
                        Guarded_("move", [&]() {
                            Announce_Move_(key_graph->Move(sign < 0 ? graph::GraphDir::Left : graph::GraphDir::Right));
                        });
                    }
                } break;
                case SDLK_HOME:
                    Guarded_("move", [&]() { Announce_Move_(key_graph->MoveToEdge(graph::GraphDir::Up)); });
                    break;
                case SDLK_END:
                    Guarded_("move", [&]() { Announce_Move_(key_graph->MoveToEdge(graph::GraphDir::Down)); });
                    break;
                case SDLK_TAB:
                    Guarded_("move", [&]() { Announce_Move_(key_graph->MoveStop(shift ? -1 : 1, true)); });
                    break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER: {
                    bool activated = false;
                    Guarded_("activate", [&]() { activated = key_graph->Activate(); });
                    pending_state_feedback = activated;
                } break;
                case SDLK_BACKSPACE: {
                    bool activated = false;
                    Guarded_("secondary", [&]() { activated = key_graph->Secondary(); });
                    pending_state_feedback = activated;
                } break;
                case SDLK_SPACE: {
                    bool had_tooltip = false;
                    Guarded_("tooltip", [&]() { had_tooltip = key_graph->Tooltip(); });
                    if (!had_tooltip) {
                        Say_(vocab::Get(vocab::VOCAB_NO_TOOLTIP), true);
                    }
                } break;
                default:
                    break;
            }
        }

        bool Pop_Key_(s_key_event* event) {
            SDL_LockMutex(key_mutex);
            bool has_event = key_queue_count > 0;
            if (has_event) {
                *event = key_queue[key_queue_head];
                key_queue_head = (key_queue_head + 1) % KEY_QUEUE_SIZE;
                key_queue_count--;
            }
            SDL_UnlockMutex(key_mutex);
            return has_event;
        }

        void Push_Key_(SDL_Keycode key, SDL_Keymod modifiers) {
            SDL_LockMutex(key_mutex);
            if (key_queue_count == KEY_QUEUE_SIZE) {
                key_queue_head = (key_queue_head + 1) % KEY_QUEUE_SIZE;
                key_queue_count--;
            }
            int32_t tail = (key_queue_head + key_queue_count) % KEY_QUEUE_SIZE;
            key_queue[tail] = { key, modifiers, SDL_GetTicks() };
            key_queue_count++;
            SDL_UnlockMutex(key_mutex);
        }

        void Toggle_Enabled_() {
            bool now_enabled = SDL_GetAtomicInt(&enabled) == 0;
            SDL_SetAtomicInt(&enabled, now_enabled ? 1 : 0);
            Detach_();
            SDL_SetAtomicInt(&claims_navigation, 0);
            SDL_SetAtomicInt(&claims_secondary, 0);
            speech::Say(vocab::Get(now_enabled ? vocab::VOCAB_LAYER_ON : vocab::VOCAB_LAYER_OFF), true);
        }

        bool Is_Toggle_Key_(SDL_Keycode key, SDL_Keymod modifiers) {
            return key == SDLK_A && (modifiers & SDL_KMOD_CTRL) != 0 && (modifiers & SDL_KMOD_SHIFT) != 0;
        }

        bool Is_Repeat_Key_(SDL_Keycode key, SDL_Keymod modifiers) {
            return key == SDLK_R && (modifiers & SDL_KMOD_CTRL) != 0 && (modifiers & SDL_KMOD_SHIFT) == 0;
        }

    } // namespace

    void Initialize() {
        if (key_mutex == nullptr) {
            key_mutex = SDL_CreateMutex();
        }
        speech::Initialize();

        graph::GraphAnnouncer::PositionText = [](int index, int count) {
            return vocab::Format(vocab::VOCAB_POSITION, std::to_string(index), std::to_string(count));
        };
        graph::GraphAnnouncer::ExpandedStateText = [](bool expanded) {
            return std::string(vocab::Get(expanded ? vocab::VOCAB_EXPANDED : vocab::VOCAB_COLLAPSED));
        };
        graph::GraphAnnouncer::FlyoutHintText = [](int count) {
            return vocab::Format(vocab::VOCAB_SUBMENU, std::to_string(count));
        };

        SDL_SetAtomicInt(&enabled, 1);
        SDL_SetAtomicInt(&claims_navigation, 0);
        SDL_SetAtomicInt(&claims_secondary, 0);
        SDL_SetAtomicInt(&initialized, key_mutex != nullptr ? 1 : 0);
        speech::Say(vocab::Get(vocab::VOCAB_STARTUP), false);
    }

    void Shutdown() {
        SDL_SetAtomicInt(&initialized, 0);
        Detach_();
        cursors.clear();
        speech::Shutdown();
    }

    bool Offer_Key_Down(SDL_Keycode key, SDL_Keymod modifiers, bool repeat) {
        if (SDL_GetAtomicInt(&initialized) == 0) {
            return false;
        }
        if ((modifiers & (SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0) {
            return false;
        }

        if (Is_Toggle_Key_(key, modifiers)) {
            if (!repeat) {
                Push_Key_(key, modifiers);
            }
            return true;
        }
        if (SDL_GetAtomicInt(&enabled) == 0) {
            return false;
        }
        if (Is_Repeat_Key_(key, modifiers)) {
            if (!repeat) {
                Push_Key_(key, modifiers);
            }
            return true;
        }

        uint32_t since_tick = (uint32_t)SDL_GetTicks() - SDL_GetAtomicU32(&last_tick_ms);
        if (since_tick > TICK_STALE_MS || SDL_GetAtomicInt(&claims_navigation) == 0) {
            return false;
        }

        bool ctrl = (modifiers & SDL_KMOD_CTRL) != 0;
        bool claimed = false;
        bool repeats = false;
        if (ctrl) {
            claimed = key == SDLK_L && (modifiers & SDL_KMOD_SHIFT) == 0;
        } else {
            switch (key) {
                case SDLK_UP:
                case SDLK_DOWN:
                case SDLK_LEFT:
                case SDLK_RIGHT:
                    claimed = true;
                    repeats = true;
                    break;
                case SDLK_HOME:
                case SDLK_END:
                case SDLK_TAB:
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    claimed = true;
                    break;
                case SDLK_BACKSPACE:
                    claimed = SDL_GetAtomicInt(&claims_secondary) != 0;
                    break;
                default:
                    break;
            }
        }

        if (claimed && (!repeat || repeats)) {
            Push_Key_(key, modifiers);
        }
        return claimed;
    }

    void Tick() {
        if (SDL_GetAtomicInt(&initialized) == 0) {
            return;
        }
        uint64_t now = SDL_GetTicks();
        SDL_SetAtomicU32(&last_tick_ms, (uint32_t)now);

        bool is_enabled = SDL_GetAtomicInt(&enabled) != 0;
        if (is_enabled) {
            Sync_();
            if (attached != nullptr && attached->Poll != nullptr) {
                bool just_attached = poll_just_attached;
                poll_just_attached = false;
                Guarded_("poll", [just_attached]() { attached->Poll(just_attached); });
            }
        }

        // A text field being typed into beats every other arbitration decision (spec section 8).
        bool typing = fields::input_field_active != 0;

        if (pending_state_feedback && key_graph != nullptr) {
            pending_state_feedback = false;
            Speak_State_Feedback_();
        }

        bool handled_key = false;
        s_key_event event;
        while (Pop_Key_(&event)) {
            if (now - event.time_ms > KEY_STALE_MS) {
                continue;
            }
            if (Is_Toggle_Key_(event.key, event.modifiers)) {
                Toggle_Enabled_();
                is_enabled = SDL_GetAtomicInt(&enabled) != 0;
                continue;
            }
            if (!is_enabled) {
                continue;
            }
            if (Is_Repeat_Key_(event.key, event.modifiers)) {
                speech::Repeat_Last();
                continue;
            }
            if (key_graph != nullptr && !typing) {
                Dispatch_(event.key, event.modifiers);
                handled_key = true;
            }
        }

        if (is_enabled && key_graph != nullptr &&
            (handled_key || !pending_focus_stop.empty() || now - last_rebuild_ms >= IDLE_REBUILD_MS)) {
            last_rebuild_ms = now;

            // The differ (spec 7.3): one place announces every focus change no keypress already spoke.
            Guarded_("differ", [&]() {
                if (!key_graph->Rerender()) {
                    return;
                }
                if (!pending_focus_stop.empty()) {
                    // A copy: Focus rebuilds the render, which frees the node the landing came from.
                    graph::GraphNode* landing = key_graph->StopLanding(pending_focus_stop);
                    graph::ControlId landing_id = landing != nullptr ? landing->Id : graph::ControlId();
                    if (landing != nullptr && key_graph->Focus(landing_id)) {
                        pending_focus_stop.clear();
                    } else if (++pending_focus_passes >= PENDING_FOCUS_MAX_PASSES) {
                        pending_focus_stop.clear();
                    }
                }
                graph::GraphNode* node = key_graph->CurrentNode();
                if (node == nullptr || node->Id == spoken) {
                    return;
                }
                std::string text = graph::GraphAnnouncer::Compose(key_graph->Current()->NodeAt(spoken), node);
                Say_(text, false);
                spoken = node->Id;
                Move_Pointer_To_(node);
            });
        }

        // Keys are claimed only while a cursor is seated; otherwise they keep their game meaning.
        bool has_focus = key_graph != nullptr && key_graph->Current() != nullptr && key_graph->CurrentNode() != nullptr;
        SDL_SetAtomicInt(&claims_navigation, is_enabled && has_focus && !typing ? 1 : 0);
        bool has_secondary = has_focus && key_graph->CurrentNode()->Vtable.OnSecondary != nullptr;
        SDL_SetAtomicInt(&claims_secondary, has_secondary ? 1 : 0);
    }

    void Request_Field_Activation(int16_t field_idx) {
        if (field_idx <= 0 || field_idx >= fields::fields_count) {
            return;
        }
        pending_activation = field_idx;
        pending_activation_secondary = false;
        pending_activation_field = fields::fields[field_idx];
    }

    void Request_Field_Secondary(int16_t field_idx) {
        Request_Field_Activation(field_idx);
        pending_activation_secondary = pending_activation == field_idx;
    }

    bool Field_Activation_Pending() {
        return pending_activation > 0;
    }

    void Request_Focus_Stop(const char* stop_key) {
        pending_focus_stop = stop_key != nullptr ? stop_key : "";
        pending_focus_passes = 0;
    }

    bool Take_Field_Activation(int16_t* field_idx, bool* secondary) {
        int16_t requested = pending_activation;
        pending_activation = 0;
        *secondary = pending_activation_secondary;
        if (requested <= 0 || requested >= fields::fields_count) {
            return false;
        }
        const s_field* field = &fields::fields[requested];
        const s_field* expected = &pending_activation_field;
        if (field->x != expected->x || field->y != expected->y || field->x_end != expected->x_end ||
            field->y_end != expected->y_end || field->field_type != expected->field_type) {
            return false;
        }
        *field_idx = requested;
        return true;
    }

} // namespace access::navigator

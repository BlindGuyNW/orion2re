#include "pch.h"

#include "access/speech.h"

#include "access/text.h"
#include "access/vocab.h"

#include <string>
#include <vector>

namespace access::speech {

    namespace {

        // Some screen readers fail on long payloads; only the first chunk may interrupt.
        constexpr size_t MAX_CHUNK_BYTES = 700;

        typedef void (*t_tolk_void)();
        typedef void (*t_tolk_bool)(bool);
        typedef const wchar_t* (*t_tolk_detect)();
        typedef bool (*t_tolk_output)(const wchar_t*, bool);
        typedef bool (*t_tolk_silence)();

        SDL_SharedObject* tolk_library = nullptr;
        t_tolk_void tolk_unload = nullptr;
        t_tolk_output tolk_output = nullptr;
        t_tolk_silence tolk_silence = nullptr;

        FILE* transcript = nullptr;
        uint64_t transcript_start_ms = 0;
        std::string last_spoken;

        void Write_Transcript_(const char* tag, const std::string& text) {
            if (transcript == nullptr) {
                return;
            }
            uint64_t elapsed = SDL_GetTicks() - transcript_start_ms;
            fprintf(transcript, "%8llu.%03llu %s %s\n",
                (unsigned long long)(elapsed / 1000), (unsigned long long)(elapsed % 1000), tag, text.c_str());
            fflush(transcript);
        }

        // Converts UTF-8 to the UTF-16 that Tolk expects on Windows; invalid bytes become U+FFFD.
        std::wstring Utf8_To_Wide_(const std::string& text) {
            std::wstring out;
            size_t i = 0;
            while (i < text.size()) {
                unsigned char c = (unsigned char)text[i];
                uint32_t code = 0xFFFD;
                size_t extra = 0;
                if (c < 0x80) {
                    code = c;
                } else if ((c & 0xE0) == 0xC0) {
                    code = c & 0x1F;
                    extra = 1;
                } else if ((c & 0xF0) == 0xE0) {
                    code = c & 0x0F;
                    extra = 2;
                } else if ((c & 0xF8) == 0xF0) {
                    code = c & 0x07;
                    extra = 3;
                }

                size_t k = 1;
                for (; k <= extra; ++k) {
                    if (i + k >= text.size() || ((unsigned char)text[i + k] & 0xC0) != 0x80) {
                        break;
                    }
                    code = (code << 6) | ((unsigned char)text[i + k] & 0x3F);
                }
                if (k <= extra) {
                    code = 0xFFFD;
                    i += k;
                } else {
                    i += extra + 1;
                }

                if (sizeof(wchar_t) == 2 && code >= 0x10000) {
                    code -= 0x10000;
                    out.push_back((wchar_t)(0xD800 + (code >> 10)));
                    out.push_back((wchar_t)(0xDC00 + (code & 0x3FF)));
                } else {
                    out.push_back((wchar_t)code);
                }
            }
            return out;
        }

        std::string Wide_To_Ascii_(const wchar_t* text) {
            std::string out;
            for (; text != nullptr && *text != 0; ++text) {
                out.push_back(*text < 0x80 ? (char)*text : '?');
            }
            return out;
        }

        // Splits on a sentence end, then whitespace, then a UTF-8 code point boundary.
        std::vector<std::string> Chunk_(const std::string& text) {
            std::vector<std::string> chunks;
            size_t pos = 0;

            while (text.size() - pos > MAX_CHUNK_BYTES) {
                size_t end = std::string::npos;
                for (size_t i = pos + MAX_CHUNK_BYTES; i > pos; --i) {
                    char c = text[i - 1];
                    if ((c == '.' || c == '!' || c == '?') && text[i] == ' ') {
                        end = i;
                        break;
                    }
                }
                if (end == std::string::npos) {
                    size_t space = text.rfind(' ', pos + MAX_CHUNK_BYTES);
                    if (space != std::string::npos && space > pos) {
                        end = space;
                    }
                }
                if (end == std::string::npos) {
                    end = pos + MAX_CHUNK_BYTES;
                    while (end > pos && ((unsigned char)text[end] & 0xC0) == 0x80) {
                        --end;
                    }
                    if (end == pos) {
                        end = pos + MAX_CHUNK_BYTES;
                    }
                }
                chunks.push_back(text.substr(pos, end - pos));
                pos = end;
                while (pos < text.size() && text[pos] == ' ') {
                    ++pos;
                }
            }

            if (pos < text.size()) {
                chunks.push_back(text.substr(pos));
            }
            return chunks;
        }

        void Load_Tolk_() {
#ifdef _WIN32
            std::string path;
            const char* base_path = SDL_GetBasePath();
            if (base_path != nullptr) {
                path = base_path;
            }
            path += "Tolk.dll";

            tolk_library = SDL_LoadObject(path.c_str());
            if (tolk_library == nullptr) {
                tolk_library = SDL_LoadObject("Tolk.dll");
            }
            if (tolk_library == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "access: Tolk.dll not found, speech disabled");
                Write_Transcript_("!", "Tolk.dll not found");
                return;
            }

            t_tolk_void tolk_load = (t_tolk_void)SDL_LoadFunction(tolk_library, "Tolk_Load");
            t_tolk_bool tolk_try_sapi = (t_tolk_bool)SDL_LoadFunction(tolk_library, "Tolk_TrySAPI");
            t_tolk_detect tolk_detect = (t_tolk_detect)SDL_LoadFunction(tolk_library, "Tolk_DetectScreenReader");
            tolk_unload = (t_tolk_void)SDL_LoadFunction(tolk_library, "Tolk_Unload");
            tolk_output = (t_tolk_output)SDL_LoadFunction(tolk_library, "Tolk_Output");
            tolk_silence = (t_tolk_silence)SDL_LoadFunction(tolk_library, "Tolk_Silence");

            if (tolk_load == nullptr || tolk_unload == nullptr || tolk_output == nullptr || tolk_silence == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "access: Tolk.dll is missing exports, speech disabled");
                Write_Transcript_("!", "Tolk.dll is missing exports");
                SDL_UnloadObject(tolk_library);
                tolk_library = nullptr;
                tolk_output = nullptr;
                return;
            }

            if (tolk_try_sapi != nullptr) {
                tolk_try_sapi(true);
            }
            tolk_load();

            std::string reader = tolk_detect != nullptr ? Wide_To_Ascii_(tolk_detect()) : std::string();
            if (reader.empty()) {
                reader = "none";
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "access: Tolk loaded, screen reader: %s", reader.c_str());
            Write_Transcript_("!", "Tolk loaded, screen reader: " + reader);
#else
            Write_Transcript_("!", "No speech backend on this platform");
#endif
        }

    } // namespace

    void Initialize() {
        transcript_start_ms = SDL_GetTicks();
        transcript = fopen("speech.log", "w");
        Load_Tolk_();
    }

    void Shutdown() {
        if (tolk_library != nullptr) {
            tolk_unload();
            SDL_UnloadObject(tolk_library);
            tolk_library = nullptr;
            tolk_output = nullptr;
        }
        if (transcript != nullptr) {
            fclose(transcript);
            transcript = nullptr;
        }
    }

    bool Is_Backend_Loaded() {
        return tolk_output != nullptr;
    }

    void Say(const std::string& text, bool interrupt) {
        std::string clean = text::Normalize_Spoken_Text(text);
        if (clean.empty()) {
            return;
        }
        last_spoken = clean;

        std::vector<std::string> chunks = Chunk_(clean);
        for (size_t i = 0; i < chunks.size(); ++i) {
            bool cut = interrupt && i == 0;
            Write_Transcript_(cut ? "I" : "Q", chunks[i]);
            if (tolk_output != nullptr) {
                std::wstring wide = Utf8_To_Wide_(chunks[i]);
                tolk_output(wide.c_str(), cut);
            }
        }
    }

    void Silence() {
        Write_Transcript_("-", "silence");
        if (tolk_silence != nullptr) {
            tolk_silence();
        }
    }

    void Repeat_Last() {
        Say(last_spoken.empty() ? std::string(vocab::Get(vocab::VOCAB_NOTHING_TO_REPEAT)) : last_spoken, true);
    }

} // namespace access::speech

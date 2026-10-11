#pragma once

#include <string>

namespace access::speech {

    // Loads Tolk.dll from the executable's directory and opens speech.log in the working directory.
    // A missing DLL leaves the transcript as the only output. Game thread only, like every function here.
    void Initialize();
    void Shutdown();
    bool Is_Backend_Loaded();

    // Speaks UTF-8 text. interrupt cuts off current speech; otherwise the text queues.
    void Say(const std::string& text, bool interrupt);
    void Silence();
    void Repeat_Last();

} // namespace access::speech

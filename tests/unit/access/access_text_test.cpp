#include "pch.h"

#include <string>

#include "access/text.h"
#include "access/vocab.h"
#include "doctest/doctest.h"

DOCTEST_TEST_SUITE("access game text normalization") {
    DOCTEST_TEST_CASE("plain text passes through") {
        CHECK_EQ(access::text::Normalize_Game_Text("NEW GAME"), std::string("NEW GAME"));
    }

    DOCTEST_TEST_CASE("null text is empty") {
        CHECK_EQ(access::text::Normalize_Game_Text(nullptr), std::string());
    }

    DOCTEST_TEST_CASE("line breaks and runs of whitespace collapse to one space") {
        CHECK_EQ(access::text::Normalize_Game_Text("  Line 1\n\nLine\t2  "), std::string("Line 1 Line 2"));
    }

    DOCTEST_TEST_CASE("a wrap point inside a word is removed") {
        CHECK_EQ(access::text::Normalize_Game_Text("Hyphen\bated"), std::string("Hyphenated"));
    }

    DOCTEST_TEST_CASE("function sequences end at their period") {
        CHECK_EQ(access::text::Normalize_Game_Text("\aF1.Text"), std::string("Text"));
        CHECK_EQ(access::text::Normalize_Game_Text("\a=0,=+2.Value"), std::string("Value"));
        CHECK_EQ(access::text::Normalize_Game_Text("\aO200,3.Wide"), std::string("Wide"));
    }

    DOCTEST_TEST_CASE("a function sequence ends at a character that cannot continue it") {
        CHECK_EQ(access::text::Normalize_Game_Text("\aF1text"), std::string("text"));
    }

    DOCTEST_TEST_CASE("one-argument layout and style commands are removed") {
        CHECK_EQ(access::text::Normalize_Game_Text("\x17IIndented"), std::string("Indented"));
        CHECK_EQ(access::text::Normalize_Game_Text("\x1A" "2Centered"), std::string("Centered"));
        CHECK_EQ(access::text::Normalize_Game_Text("\x1B\x01" "Colored"), std::string("Colored"));
        CHECK_EQ(access::text::Normalize_Game_Text("Stop\x1FPhere"), std::string("Stophere"));
    }

    DOCTEST_TEST_CASE("foreground and background color commands are removed") {
        CHECK_EQ(access::text::Normalize_Game_Text("\x1C" "2Red"), std::string("Red"));
        CHECK_EQ(access::text::Normalize_Game_Text("\x1C" "61Shaded"), std::string("Shaded"));
    }

    DOCTEST_TEST_CASE("substitutions are dropped with their argument") {
        CHECK_EQ(access::text::Normalize_Game_Text("You have \x18" "B1 credits."), std::string("You have credits."));
        CHECK_EQ(access::text::Normalize_Game_Text("ship\x18" "E here"), std::string("ship here"));
        CHECK_EQ(access::text::Normalize_Game_Text("\x18QUpper\x18T"), std::string("Upper"));
    }

    DOCTEST_TEST_CASE("conditions are dropped with their arguments") {
        CHECK_EQ(access::text::Normalize_Game_Text("\x19" "01Yes"), std::string("Yes"));
        CHECK_EQ(access::text::Normalize_Game_Text("\x19" "1No"), std::string("No"));
        CHECK_EQ(access::text::Normalize_Game_Text("\x19" "512Same"), std::string("Same"));
    }

    DOCTEST_TEST_CASE("a command at the end of the string does not read past it") {
        CHECK_EQ(access::text::Normalize_Game_Text("End\x1C"), std::string("End"));
        CHECK_EQ(access::text::Normalize_Game_Text("End\x18"), std::string("End"));
        CHECK_EQ(access::text::Normalize_Game_Text("End\a"), std::string("End"));
    }

    DOCTEST_TEST_CASE("CP850 characters decode to UTF-8") {
        CHECK_EQ(access::text::Normalize_Game_Text("Sp\x82" "cial"), std::string("Sp\xC3\xA9" "cial"));
    }
}

DOCTEST_TEST_SUITE("access spoken text normalization") {
    DOCTEST_TEST_CASE("control characters become single spaces") {
        CHECK_EQ(access::text::Normalize_Spoken_Text("a\r\nb\x01" "c"), std::string("a b c"));
    }

    DOCTEST_TEST_CASE("UTF-8 sequences are preserved") {
        CHECK_EQ(access::text::Normalize_Spoken_Text("caf\xC3\xA9  ok"), std::string("caf\xC3\xA9 ok"));
    }
}

DOCTEST_TEST_SUITE("access vocabulary") {
    DOCTEST_TEST_CASE("placeholders take their arguments") {
        CHECK_EQ(access::vocab::Format(access::vocab::VOCAB_POSITION, "2", "5"), std::string("2 of 5"));
    }

    DOCTEST_TEST_CASE("an unknown id is empty") {
        CHECK_EQ(std::string(access::vocab::Get(access::vocab::VOCAB_COUNT)), std::string());
    }
}

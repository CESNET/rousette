/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include "http/utils.hpp"

using namespace std::string_literals;

namespace doctest {

template <>
struct StringMaker<std::vector<std::string>> {
    static String convert(const std::vector<std::string>& obj)
    {
        std::ostringstream oss;
        oss << '[';
        std::copy(obj.begin(), obj.end(), std::experimental::make_ostream_joiner(oss, ", "));
        oss << ']';
        return oss.str().c_str();
    }
};
}

TEST_CASE("Accept header")
{
    std::string inp;
    std::vector<std::string> expectedOutput;

    DOCTEST_SUBCASE("Accept all, default quality")
    {
        inp = "*/*";
        expectedOutput = {"*/*"};
    }
    DOCTEST_SUBCASE("Accept any text, default quality")
    {
        inp = "text/*";
        expectedOutput = {"text/*"};
    }
    DOCTEST_SUBCASE("Accept text/plain only, default quality")
    {
        inp = "text/plain";
        expectedOutput = {"text/plain"};
    }
    DOCTEST_SUBCASE("Accept mimetype with plus specifier, default quality")
    {
        inp = "application/yang-data+json";
        expectedOutput = {"application/yang-data+json"};
    }
    DOCTEST_SUBCASE("Accept mimetype with plus specifier and some quality")
    {
        inp = "application/yang-data+xml;q=0.5";
        expectedOutput = {"application/yang-data+xml"};
    }
    DOCTEST_SUBCASE("Invalid mime type (spaces)")
    {
        inp = "* /*;q=1";
    }
    DOCTEST_SUBCASE("Invalid mime type (no slash)")
    {
        inp = "**;q=1";
    }
    DOCTEST_SUBCASE("Invalid mime type (no subtype)")
    {
        inp = "text;q=1";
    }
    DOCTEST_SUBCASE("Multiple types and qualities")
    {
        inp = "*/*, application/yang-data+xml;q=5,text/*;q=15";
        expectedOutput = {
            "text/*",
            "application/yang-data+xml",
            "*/*",
        };
    }
    DOCTEST_SUBCASE("Multiple types and qualities with some extra WS and same qualities")
    {
        inp = " */*;q=0.1, application/yang-data+xml;q=0.8,text/*;q=1   ,  \t application/yang-data+json;q=1 ";
        expectedOutput = {
            "text/*",
            "application/yang-data+json",
            "application/yang-data+xml",
            "*/*",
        };
    }
    DOCTEST_SUBCASE("Spaces around semicolon")
    {
        inp = "audio/*; q=0.2, audio/basic";
        expectedOutput = {
            "audio/basic",
            "audio/*",
        };
    }
    DOCTEST_SUBCASE("Newline between types")
    {
        inp = "text/plain; q=0.5, text/html,\ntext/x-dvi; q=0.8, text/x-c";
        expectedOutput = {
            "text/html",
            "text/x-c",
            "text/x-dvi",
            "text/plain",
        };
    }

    REQUIRE(rousette::http::parseAcceptHeader(inp) == expectedOutput);
};

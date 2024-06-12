/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include "restconf/utils/yang.h"
#include "tests/pretty_printers.h"

using namespace std::string_literals;

TEST_CASE("YANG")
{
    using rousette::restconf::yangDateTime;

    std::tm timeinfo{};
    timeinfo.tm_sec = 12;
    timeinfo.tm_min = 53;
    timeinfo.tm_hour = 10;
    timeinfo.tm_mday = 12;
    timeinfo.tm_mon = 6 - 1;
    timeinfo.tm_year = 2024 - 1900;
    timeinfo.tm_isdst = 1;

    std::mktime(&timeinfo);

    auto tp1 = std::chrono::system_clock::from_time_t(std::mktime(&timeinfo)) + std::chrono::nanoseconds{123456789};
    REQUIRE(yangDateTime<decltype(tp1)::clock, std::chrono::nanoseconds>(tp1) == "2024-06-12T10:53:12.123456789-00:00");

    auto tp2 = std::chrono::system_clock::from_time_t(std::mktime(&timeinfo)) + std::chrono::nanoseconds{123};
    REQUIRE(yangDateTime<decltype(tp2)::clock, std::chrono::nanoseconds>(tp2) == "2024-06-12T10:53:12.000000123-00:00");
}

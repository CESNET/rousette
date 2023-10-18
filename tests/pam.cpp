/*
 * Copyright (C) 2016-2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include "restconf/PAM.h"
#include "configure.cmake.h"

TEST_CASE("URI path parser")
{
    using rousette::authenticate_pam;

    REQUIRE(rousette::authenticate_pam("Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==",
                std::filesystem::path(CMAKE_CURRENT_SOURCE_DIR) / "tests" / "pam",
                "[::1]:666") == "XXX");
}

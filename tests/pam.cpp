/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include "restconf/PAM.h"
#include "configure.cmake.h"

TEST_CASE("URI path parser")
{
    using rousette::auth::authenticate_pam;

    SECTION("success") {
        std::string username, blob;

        SECTION("RFC Aladdin") {
            username = "Aladdin";
            blob = "QWxhZGRpbjpvcGVuIHNlc2FtZQ==";
        }

        SECTION("dwdm") {
            username = "dwdm";
            blob = "ZHdkbTpEV0RN";
        }

        SECTION("root") {
            username = "root";
            blob = "cm9vdDpzZWtyaXQ=";
        }

        SECTION("yangnobody") {
            username = "yangnobody";
            blob = "eWFuZ25vYm9keTpubyBjaGFuY2U=";
        }

        SECTION("norules") {
            username = "norules";
            blob = "bm9ydWxlczplbXB0eQ==";
        }

        SECTION("double colon") {
            username = "foo";
            blob = "Zm9vOmJhcjpiYXo=";
        }

        REQUIRE(authenticate_pam("Basic " + blob,
                    std::filesystem::path(CMAKE_CURRENT_BINARY_DIR) / "tests" / "pam",
                    "[::1]:666") == username);
    }

    SECTION("failed") {
        std::string input;
        std::string error;

        SECTION("invalid auth method") {
            input = "wtf xxx";
            error = "Cannot parse the Basic authorization header";
        }

        SECTION("invalid base64") {
            input = "Basic xxx";
            error = "Cannot parse the user-pass authorization blob";
        }

        SECTION("invalid user") {
            input = "Basic MDox";
            error = "PAM: pam_authenticate: User not known to the underlying authentication module";
        }

        SECTION("wrong password") {
            input = "Basic cm9vdDpyb290";
            error = "PAM: pam_authenticate: Authentication failure";
        }

        REQUIRE_THROWS_WITH_AS(
                authenticate_pam(input, std::filesystem::path(CMAKE_CURRENT_BINARY_DIR) / "tests" / "pam", ""),
                error.c_str(),
                rousette::auth::Error);
    }
}

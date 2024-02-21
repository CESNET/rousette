/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10085";
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"

TEST_CASE("obtaining YANG schemas")
{
    spdlog::set_level(spdlog::level::trace);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};

    SECTION("unsupported methods")
    {
        for (const std::string httpMethod : {"POST", "PUT", "OPTIONS", "PATCH", "DELETE"}) {
            CAPTURE(httpMethod);
            REQUIRE(clientRequest(httpMethod, YANG_ROOT "/ietf-yang-library@2019-01-04", "", {}) == Response{405, noContentTypeHeaders, ""});
        }
    }

    SECTION("loaded modules")
    {
        SECTION("module with revision")
        {
            SECTION("no revision in uri")
            {
                REQUIRE(get(YANG_ROOT "/ietf-system", {}) == Response{404, noContentTypeHeaders, ""});
            }
            SECTION("correct revision in uri")
            {
                auto resp = get(YANG_ROOT "/ietf-system@2014-08-06", {});
                auto expectedShortenedResp = Response{200, yangHeaders, "module ietf-system {\n  namespa"};

                REQUIRE(resp.equalStatusCodeAndHeaders(expectedShortenedResp));
                REQUIRE(resp.data.substr(0, 30) == expectedShortenedResp.data);
            }
            SECTION("wrong revision in uri")
            {
                REQUIRE(get(YANG_ROOT "/ietf-system@1999-12-13", {}) == Response{404, noContentTypeHeaders, ""});
                REQUIRE(get(YANG_ROOT "/ietf-system@abcd-ef-gh", {}) == Response{404, noContentTypeHeaders, ""});
            }
        }

        SECTION("module without revision")
        {
            SECTION("no revision in uri")
            {
                auto resp = get(YANG_ROOT "/example", {});
                auto expectedShortenedResp = Response{200, yangHeaders, "module example {\n  yang-versio"};

                REQUIRE(resp.equalStatusCodeAndHeaders(expectedShortenedResp));
                REQUIRE(resp.data.substr(0, 30) == expectedShortenedResp.data);
            }
            SECTION("revision in uri")
            {
                REQUIRE(get(YANG_ROOT "/example@2020-02-02", {}) == Response{404, noContentTypeHeaders, ""});
            }
        }
    }
}

/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <chrono>
#include <format>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include <thread>
#include "http/utils.hpp"
#include "restconf/Server.h"
#include "restconf/utils/conditional.h"
#include "restconf/utils/sysrepo.h"
#include "tests/aux-utils.h"
#include "tests/restconf-http-calls.h"

using namespace std::chrono_literals;

namespace {
std::optional<std::string> lastModifiedHeader(const Response& response)
{
    if (auto it = response.headers.find("last-modified"); it != response.headers.end()) {
        return it->second.value;
    }
    return std::nullopt;
}

/** @brief The most recent sysrepo timestamp of the top-level nodes, formatted as the HTTP date */
std::string sysrepoLastModified(sysrepo::Session sess, sysrepo::Datastore datastore, const std::string& xpath)
{
    rousette::restconf::ScopedDatastoreSwitch dsSwitch(sess, datastore);
    auto data = sess.getData(xpath, 1, sysrepo::GetOptions::WithLastModification);
    REQUIRE(data);

    std::optional<std::chrono::system_clock::time_point> res;
    for (const auto& node : data->firstSibling().siblings()) {
        for (const auto& meta : node.meta()) {
            if (meta.module().name() == "sysrepo" && meta.name() == "last-modif") {
                auto timestamp = libyang::fromYangTimeFormat<std::chrono::system_clock>(meta.valueStr());
                res = res ? std::max(*res, timestamp) : timestamp;
            }
        }
    }
    REQUIRE(res);
    return rousette::http::httpDate(*res);
}

void waitForNextSecond()
{
    std::this_thread::sleep_until(std::chrono::ceil<std::chrono::seconds>(std::chrono::system_clock::now()));
}
}

TEST_CASE("conditional requests")
{
    spdlog::set_level(spdlog::level::trace);
    sysrepo::setGlobalContextOptions(sysrepo::ContextFlags::LibYangPrivParsed | sysrepo::ContextFlags::NoPrinted, sysrepo::GlobalContextEffect::Immediate);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));
    auto nacmGuard = manageNacm(srSess);

    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};

    srSess.switchDatastore(sysrepo::Datastore::Operational);
    srSess.setItem("/ietf-system:system/hostname", "hostname");
    srSess.setItem("/example:config-nonconfig/nonconfig-node", "foo-config-false");
    srSess.applyChanges();

    srSess.switchDatastore(sysrepo::Datastore::Startup);
    srSess.setItem("/example:top-level-leaf", "startup");
    srSess.applyChanges();

    srSess.switchDatastore(sysrepo::Datastore::Running);
    srSess.setItem("/example:config-nonconfig/config-node", "foo-config-true");
    srSess.applyChanges();

    setupRealNacm(srSess);

    SECTION("Last-Modified")
    {
        DOCTEST_SUBCASE("NMDA datastores")
        {
            const auto expectedRunning = sysrepoLastModified(srSess, sysrepo::Datastore::Running, "/example:config-nonconfig");
            const auto expectedRunningWhole = sysrepoLastModified(srSess, sysrepo::Datastore::Running, "/*");
            const auto expectedStartup = sysrepoLastModified(srSess, sysrepo::Datastore::Startup, "/example:top-level-leaf");

            // the metadata must not leak into the response body
            auto resp = get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {});
            REQUIRE(resp == Response{200, jsonHeaders, R"({
  "example:config-nonconfig": {
    "config-node": "foo-config-true"
  }
}
)"});
            REQUIRE(lastModifiedHeader(resp) == expectedRunning);

            resp = get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {{"accept", "application/yang-data+xml"}});
            REQUIRE(resp == Response{200, xmlHeaders, R"(<config-nonconfig xmlns="http://example.tld/example">
  <config-node>foo-config-true</config-node>
</config-nonconfig>
)"});
            REQUIRE(lastModifiedHeader(resp) == expectedRunning);

            REQUIRE(lastModifiedHeader(head(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {})) == expectedRunning);
            REQUIRE(lastModifiedHeader(get(RESTCONF_ROOT_DS("running"), {AUTH_ROOT})) == expectedRunningWhole);

            resp = get(RESTCONF_ROOT_DS("startup") "/example:top-level-leaf", {});
            REQUIRE(resp == Response{200, jsonHeaders, R"({
  "example:top-level-leaf": "startup"
}
)"});
            REQUIRE(lastModifiedHeader(resp) == expectedStartup);

            REQUIRE(!lastModifiedHeader(get(RESTCONF_ROOT_DS("operational") "/example:config-nonconfig", {})));
        }

        DOCTEST_SUBCASE("writes")
        {
            const auto runningBefore = lastModifiedHeader(get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {}));
            const auto startupBefore = lastModifiedHeader(get(RESTCONF_ROOT_DS("startup") "/example:top-level-leaf", {}));
            REQUIRE(runningBefore);
            REQUIRE(startupBefore);

            waitForNextSecond();
            REQUIRE(put(RESTCONF_ROOT_DS("running") "/example:config-nonconfig/config-node",
                        {AUTH_ROOT, CONTENT_TYPE_JSON},
                        R"({"example:config-node": "changed"}")")
                    == Response{204, noContentTypeHeaders, ""});

            const auto runningAfter = lastModifiedHeader(get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {}));
            REQUIRE(runningAfter == sysrepoLastModified(srSess, sysrepo::Datastore::Running, "/example:config-nonconfig"));
            REQUIRE(runningAfter != runningBefore);

            // other datastores are not affected
            REQUIRE(lastModifiedHeader(get(RESTCONF_ROOT_DS("startup") "/example:top-level-leaf", {})) == startupBefore);

            // Changes of the non-config data do not affect the timestamp
            waitForNextSecond();
            srSess.switchDatastore(sysrepo::Datastore::Operational);
            srSess.setItem("/example:config-nonconfig/nonconfig-node", "changed");
            srSess.applyChanges();
            REQUIRE(lastModifiedHeader(get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {})) == runningAfter);
        }

        DOCTEST_SUBCASE("{+restconf}/data")
        {
            // not implemented yet
            REQUIRE(!lastModifiedHeader(get(RESTCONF_DATA_ROOT, {})));
            REQUIRE(!lastModifiedHeader(get(RESTCONF_DATA_ROOT "/example:config-nonconfig", {})));
            REQUIRE(!lastModifiedHeader(get(RESTCONF_DATA_ROOT "/example:config-nonconfig?content=config", {})));
        }
    }

    SECTION("If-Modified-Since")
    {
        const auto lastModified = lastModifiedHeader(get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {}));
        REQUIRE(lastModified);
        const auto lastModifiedTime = rousette::http::parseHttpDate(*lastModified);
        REQUIRE(lastModifiedTime);

        const auto notModified = Response{304, noContentTypeHeaders, ""};
        const auto fullResponse = Response{200, jsonHeaders, R"({
  "example:config-nonconfig": {
    "config-node": "foo-config-true"
  }
}
)"};

        DOCTEST_SUBCASE("not modified")
        {
            std::string since;

            DOCTEST_SUBCASE("same timestamp")
            {
                since = *lastModified;
            }

            DOCTEST_SUBCASE("later timestamp")
            {
                since = rousette::http::httpDate(*lastModifiedTime + 1h);
            }

            DOCTEST_SUBCASE("obsolete RFC 850 format")
            {
                since = std::format("{:%A, %d-%b-%y %H:%M:%S} GMT", *lastModifiedTime);
            }

            DOCTEST_SUBCASE("obsolete asctime format")
            {
                since = std::format("{:%a %b %e %H:%M:%S %Y}", *lastModifiedTime);
            }

            auto resp = get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {{"if-modified-since", since}});
            REQUIRE(resp == notModified);
            REQUIRE(lastModifiedHeader(resp) == lastModified);

            resp = head(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {{"if-modified-since", since}});
            REQUIRE(resp == notModified);
            REQUIRE(lastModifiedHeader(resp) == lastModified);
        }

        DOCTEST_SUBCASE("whole datastore")
        {
            const auto lastModifiedWhole = lastModifiedHeader(get(RESTCONF_ROOT_DS("running"), {AUTH_ROOT}));
            REQUIRE(lastModifiedWhole);

            auto resp = get(RESTCONF_ROOT_DS("running"), {AUTH_ROOT, {"if-modified-since", *lastModifiedWhole}});
            REQUIRE(resp == notModified);
            REQUIRE(lastModifiedHeader(resp) == lastModifiedWhole);
        }

        DOCTEST_SUBCASE("modified")
        {
            auto resp = get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig",
                            {{"if-modified-since", rousette::http::httpDate(*lastModifiedTime - 1s)}});
            REQUIRE(resp == fullResponse);
            REQUIRE(lastModifiedHeader(resp) == lastModified);

            waitForNextSecond();
            REQUIRE(put(RESTCONF_ROOT_DS("running") "/example:config-nonconfig/config-node",
                        {AUTH_ROOT, CONTENT_TYPE_JSON},
                        R"({"example:config-node": "changed"}")")
                    == Response{204, noContentTypeHeaders, ""});

            resp = get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {{"if-modified-since", *lastModified}});
            REQUIRE(resp == Response{200, jsonHeaders, R"({
  "example:config-nonconfig": {
    "config-node": "changed"
  }
}
)"});
            REQUIRE(lastModifiedHeader(resp) != lastModified);
            REQUIRE(get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {{"if-modified-since", *lastModifiedHeader(resp)}}) == notModified);
        }

        DOCTEST_SUBCASE("invalid date is ignored")
        {
            std::string since;

            DOCTEST_SUBCASE("empty") { since = ""; }
            DOCTEST_SUBCASE("garbage") { since = "garbage"; }
            DOCTEST_SUBCASE("ISO 8601") { since = "2017-01-26T16:00:14Z"; }
            DOCTEST_SUBCASE("missing GMT") { since = "Thu, 26 Jan 2017 16:00:14"; }
            DOCTEST_SUBCASE("invalid hour") { since = "Thu, 26 Jan 2017 25:00:14 GMT"; }

            // RFC 9110, sec. 5.3: some intermediate proxy may combine multiple field lines
            DOCTEST_SUBCASE("combined values") { since = *lastModified + ", " + *lastModified; }

            REQUIRE(get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {{"if-modified-since", since}}) == fullResponse);
        }

        DOCTEST_SUBCASE("header is ignored")
        {
            // RFC 9110, sec. 13.1.3: If-None-Match takes precedence
            REQUIRE(get(RESTCONF_ROOT_DS("running") "/example:config-nonconfig", {{"if-modified-since", *lastModified}, {"if-none-match", "\"foo\""}}) == fullResponse);

            // a date that would result in 304 if the header was evaluated
            const auto future = rousette::http::httpDate(std::chrono::system_clock::now() + 1h);

            // no timestamp for the operational datastore
            REQUIRE(get(RESTCONF_ROOT_DS("operational") "/example:config-nonconfig", {{"if-modified-since", future}}).statusCode == 200);

            // {+restconf}/data does not report the timestamp yet
            REQUIRE(get(RESTCONF_DATA_ROOT "/example:config-nonconfig", {{"if-modified-since", future}}).statusCode == 200);
        }

        DOCTEST_SUBCASE("errors take precedence")
        {
            REQUIRE(get(RESTCONF_ROOT_DS("running") "/example:top-level-leaf", {{"if-modified-since", *lastModified}}).statusCode == 404);

            const auto nacmLastModified = lastModifiedHeader(get(RESTCONF_ROOT_DS("running") "/ietf-netconf-acm:nacm", {AUTH_ROOT}));
            REQUIRE(nacmLastModified);
            REQUIRE(get(RESTCONF_ROOT_DS("running") "/ietf-netconf-acm:nacm", {{"if-modified-since", *nacmLastModified}}).statusCode == 404);
        }
    }
}

TEST_CASE("If-Modified-Since evaluation")
{
    using rousette::restconf::isNotModified;
    using namespace std::chrono;

    const auto lastModified = sys_days{1994y / November / 6} + 8h + 49min + 37s + 500ms;

    nghttp2::asio_http2::header_map headers{{"if-modified-since", {"Sun, 06 Nov 1994 08:49:37 GMT", false}}};
    REQUIRE(isNotModified(headers, lastModified));

    // RFC 9110, sec. 13.1.3: multiple values
    headers.insert({"if-modified-since", {"Sun, 06 Nov 1994 08:49:37 GMT", false}});
    REQUIRE(!isNotModified(headers, lastModified));
}

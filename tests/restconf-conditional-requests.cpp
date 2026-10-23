/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <chrono>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include <thread>
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
    return rousette::restconf::httpDate(*res);
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

            // The header has a resolution of seconds, so make sure that the write does not happen within the same second
            std::this_thread::sleep_for(1500ms);
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
            std::this_thread::sleep_for(1s);
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
}

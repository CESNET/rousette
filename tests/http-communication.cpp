/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <chrono>
#include <iostream>
#include <libyang-cpp/Enum.hpp>
#include <nghttp2/asio_http2_client.h>
#include <sstream>
#include <sysrepo-cpp/Enum.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include "UniqueResource.h"
#include "restconf/Server.h"

using namespace std::string_literals;

namespace doctest {
template <>
struct StringMaker<std::pair<int, std::string>> {
    static String convert(const std::pair<int, std::string>& o)
    {
        return ("{"s + std::to_string(o.first) + ", \"" + o.second + "\"}").c_str();
    }
};
}

static const auto SERVER_ADDRESS = "::1";
static const auto SERVER_PORT = "10080";
static const auto SERVER_ADDRESS_AND_PORT = "http://["s + SERVER_ADDRESS + "]" + ":" + SERVER_PORT;

std::pair<int, std::string> retrieveData(auto xpath, const std::optional<std::string>& user)
{
    namespace ng = nghttp2::asio_http2;
    namespace ng_client = ng::client;

    boost::asio::io_service io_service;
    ng_client::session client(io_service, SERVER_ADDRESS, SERVER_PORT);

    std::ostringstream oss;
    int statusCode;

    client.on_connect([&client, &xpath, &oss, &user, &statusCode](auto) {
        boost::system::error_code ec;
        ng::header_map headers;

        if (user) {
            headers.insert({"x-netconf-nacm-user", {user.value(), false}});
        }

        auto req = client.submit(ec, "GET", SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath, headers);
        req->on_response([&oss, &statusCode](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
            statusCode = res.status_code();
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });

    client.on_error([](const boost::system::error_code& ec) {
        std::cerr << "error: " << ec.message() << std::endl;
    });

    io_service.run();

    return {statusCode, oss.str()};
}

TEST_CASE("NACM")
{
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};

    // something we can read
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Operational);
    srSess.setItem("/nacm-test:limited/val", "limited");
    srSess.setItem("/nacm-test:unlimited/val", "unlimited");
    srSess.applyChanges();

    srSess.switchDatastore(sysrepo::Datastore::Running);
    srSess.setItem("/ietf-netconf-acm:nacm/enable-external-groups", "false");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='wheel']/user-name[.='root']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='optics']/user-name[.='dwdm']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='anonymous']/user-name[.='restconf-anonymous']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon']/group[.='anonymous']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon']/rule[name='1']/module-name", "nacm-test");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon']/rule[name='1']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon']/rule[name='1']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon']/rule[name='1']/path", "/nacm-test:unlimited/val");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon']/rule[name='2']/module-name", "*");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon']/rule[name='2']/action", "deny");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm']/group[.='dwdm']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm']/rule[name='1']/module-name", "nacm-test");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm']/rule[name='1']/action", "permit");

    srSess.applyChanges();

    const auto respLimitedLeaf = R"({
  "nacm-test:limited": {
    "val": "limited"
  }
}
)";
    const auto respUnlimitedLeaf = R"({
  "nacm-test:unlimited": {
    "val": "unlimited"
  }
}
)";
    const auto respAll = R"({
  "nacm-test:limited": {
    "val": "limited"
  },
  "nacm-test:unlimited": {
    "val": "unlimited"
  }
}
)";

    auto server = rousette::restconf::Server{srConn};
    auto guard = make_unique_resource(
        [&server]() { server.listen_and_serve(SERVER_ADDRESS, SERVER_PORT, true); },
        [&]() {
            server.stop();
            srSess.copyConfig(sysrepo::Datastore::Startup, "ietf-netconf-acm");
        });

    DOCTEST_SUBCASE("real-like NACM")
    {
        REQUIRE(retrieveData("/nacm-test:*", "dwdm") == std::pair<int, std::string>(200, respAll));
        REQUIRE(retrieveData("/nacm-test:unlimited", "dwdm") == std::pair<int, std::string>(200, respUnlimitedLeaf));
        REQUIRE(retrieveData("/nacm-test:limited", "dwdm") == std::pair<int, std::string>(200, respLimitedLeaf));
        REQUIRE(retrieveData("/nacm-test:*", "restconf-anonymous") == std::pair<int, std::string>(200, respUnlimitedLeaf));
        REQUIRE(retrieveData("/nacm-test:unlimited", "restconf-anonymous") == std::pair<int, std::string>(200, respUnlimitedLeaf));
        REQUIRE(retrieveData("/nacm-test:limited", "restconf-anonymous") == std::pair<int, std::string>(404, "go away"));
        REQUIRE(retrieveData("/nacm-test:*", std::nullopt) == std::pair<int, std::string>(200, respUnlimitedLeaf));
        REQUIRE(retrieveData("/nacm-test:unlimited", std::nullopt) == std::pair<int, std::string>(200, respUnlimitedLeaf));
        REQUIRE(retrieveData("/nacm-test:limited", std::nullopt) == std::pair<int, std::string>(404, "go away"));
    }
}

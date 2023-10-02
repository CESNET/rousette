/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <iostream>
#include <nghttp2/asio_http2.h>
#include <nghttp2/asio_http2_client.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <sysrepo-cpp/Enum.hpp>
#include <sysrepo-cpp/Session.hpp>
#include "restconf/Server.h"
#include "tests/UniqueResource.h"
#include "tests/pretty_printers.h"

using namespace std::string_literals;
namespace ng = nghttp2::asio_http2;
namespace ng_client = ng::client;

struct SrChange {
    sysrepo::ChangeOperation operation;
    std::string nodePath;
    std::optional<std::string_view> currentValue;

    bool operator==(const SrChange&) const = default;
};

namespace trompeloeil {
template <>
struct printer<SrChange> {
    static void print(std::ostream& os, const SrChange& o)
    {
        os << '{';
        os << o.operation << ", ";
        os << o.nodePath << ", ";
        printer<std::optional<std::string_view>>::print(os, o.currentValue);
        os << '}';
    }
};
}

struct ChangeMock {
    MAKE_MOCK1(change, void(const std::vector<SrChange>&));
};

struct Response {
    int statusCode;
    ng::header_map headers;
    std::string data;

    bool operator==(const Response& o) const
    {
        // Skipping 'date' header. Its value will not be reproducible in simple tests
        ng::header_map myHeaders(headers);
        ng::header_map otherHeaders(o.headers);
        myHeaders.erase("date");
        otherHeaders.erase("date");

        return statusCode == o.statusCode && data == o.data && std::equal(myHeaders.begin(), myHeaders.end(), otherHeaders.begin(), otherHeaders.end(), [](const auto& a, const auto& b) {
                   return a.first == b.first && a.second.value == b.second.value; // Skipping 'sensitive' field from ng::header_value which does not seem important for us.
               });
    }
};

namespace doctest {

template <>
struct StringMaker<ng::header_map> {
    static String convert(const ng::header_map& m)
    {
        std::ostringstream oss;
        oss << "{\n";
        for (const auto& [k, v] : m) {
            oss << "\t"
                << "{\"" << k << "\", "
                << "{\"" << v.value << "\", " << std::boolalpha << v.sensitive << "}},\n";
        }
        oss << "}";
        return oss.str().c_str();
    }
};

template <>
struct StringMaker<Response> {
    static String convert(const Response& o)
    {
        std::ostringstream oss;

        oss << "{"
            << std::to_string(o.statusCode) << ", "
            << StringMaker<decltype(o.headers)>::convert(o.headers) << ",\n"
            << "\"" << o.data << "\",\n"
            << "}";

        return oss.str().c_str();
    }
};
}

static const auto SERVER_ADDRESS = "::1";
static const auto SERVER_PORT = "10080";
static const auto SERVER_ADDRESS_AND_PORT = "http://["s + SERVER_ADDRESS + "]" + ":" + SERVER_PORT;

Response retrieveData(auto xpath, const std::map<std::string, std::string>& headers = {})
{
    boost::asio::io_service io_service;
    ng_client::session client(io_service, SERVER_ADDRESS, SERVER_PORT);

    std::ostringstream oss;
    ng::header_map resHeaders;
    int statusCode;

    client.on_connect([&](auto) {
        boost::system::error_code ec;

        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        auto req = client.submit(ec, "GET", SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath, reqHeaders);
        req->on_response([&](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
            statusCode = res.status_code();
            resHeaders = res.header();
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });
    client.on_error([](const boost::system::error_code& ec) {
        FAIL("HTTP client error: ", ec.message());
    });
    io_service.run();

    return {statusCode, resHeaders, oss.str()};
}

Response putData(auto xpath, const std::string& payload, const std::map<std::string, std::string>& headers = {})
{
    boost::asio::io_service io_service;
    ng_client::session client(io_service, SERVER_ADDRESS, SERVER_PORT);
    // this is a test, and the server is expected to reply "soon"
    client.read_timeout(boost::posix_time::seconds(3));

    std::ostringstream oss;
    ng::header_map resHeaders;
    int statusCode;
    std::optional<std::string> clientError;

    client.on_connect([&](auto) {
        boost::system::error_code ec;

        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        auto req = client.submit(ec, "PUT", SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath, payload, reqHeaders);
        req->on_response([&](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
            statusCode = res.status_code();
            resHeaders = res.header();
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });
    client.on_error([&clientError](const boost::system::error_code& ec) {
        clientError = ec.message();
    });
    io_service.run();
    if (clientError) {
        FAIL("HTTP client error: ", *clientError);
    }

    return {statusCode, resHeaders, oss.str()};
}

TEST_CASE("HTTP")
{
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.copyConfig(sysrepo::Datastore::Startup, "ietf-netconf-acm");
    srSess.copyConfig(sysrepo::Datastore::Startup, "example");
    srSess.copyConfig(sysrepo::Datastore::Startup, "ietf-system");

    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    auto guard = make_unique_resource([] {},
                                      [&]() {
                                          srSess.switchDatastore(sysrepo::Datastore::Running);

                                          /* cleanup running DS of ietf-netconf-acm module
                                             because it contains XPaths to other modules that we
                                             can't uninstall because the running DS content would be invalid
                                           */
                                          srSess.copyConfig(sysrepo::Datastore::Startup, "ietf-netconf-acm");
                                      });

    const ng::header_map jsonHeaders{
        {"access-control-allow-origin", {"*", false}},
        {"content-type", {"application/yang-data+json", false}},
    };

    const ng::header_map xmlHeaders{
        {"access-control-allow-origin", {"*", false}},
        {"content-type", {"application/yang-data+xml", false}},
    };


    // something we can read
    srSess.switchDatastore(sysrepo::Datastore::Running);
    srSess.setItem("/ietf-system:system/contact", "contact");
    srSess.setItem("/ietf-system:system/hostname", "hostname");
    srSess.setItem("/ietf-system:system/location", "location");
    srSess.setItem("/ietf-system:system/clock/timezone-utc-offset", "2");
    srSess.setItem("/ietf-system:system/radius/server[name='a']/udp/address", "1.1.1.1");
    srSess.setItem("/ietf-system:system/radius/server[name='a']/udp/shared-secret", "shared-secret");
    srSess.applyChanges();

    ChangeMock changeMock;
    trompeloeil::sequence seq1;

    auto changeIterator = [](auto session, auto& changeMock, auto path) {
        std::vector<SrChange> changes;
        for (const auto& change : session.getChanges(path)) {
            std::optional<std::string_view> val;
            if (change.node.isTerm()) {
                val = change.node.asTerm().valueStr();
            }
            changes.emplace_back(change.operation, change.node.schema().path(), val);
        }

        changeMock.change(changes);
    };

    auto sub1 = srSess.onModuleChange(
        "ietf-system", [&](auto session, auto, auto, auto, auto, auto) {
            changeIterator(session, changeMock, "/ietf-system:*//.");
            return sysrepo::ErrorCode::Ok;
        },
        std::nullopt,
        0,
        sysrepo::SubscribeOptions::DoneOnly);
    auto sub2 = srSess.onModuleChange(
        "example", [&](auto session, auto, auto, auto, auto, auto) {
            changeIterator(session, changeMock, "/example:*//.");
            return sysrepo::ErrorCode::Ok;
        },
        std::nullopt,
        0,
        sysrepo::SubscribeOptions::DoneOnly);


    // no or empty x-remote-user header gets rejected
    REQUIRE(retrieveData("/ietf-system:system", {}) == Response{401, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "access-denied",
        "error-message": "HTTP header x-remote-user not found or empty."
      }
    ]
  }
}
)"});

    REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", ""}}) == Response{401, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "access-denied",
        "error-message": "HTTP header x-remote-user not found or empty."
      }
    ]
  }
}
)"});

    // setup real-like NACM
    srSess.switchDatastore(sysrepo::Datastore::Running);
    srSess.setItem("/ietf-netconf-acm:nacm/enable-external-groups", "false");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='optics']/user-name[.='dwdm']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='yangnobody']/user-name[.='yangnobody']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='norules']/user-name[.='norules']", "");

    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/group[.='yangnobody']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/module-name", "ietf-system");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/path", "/ietf-system:system/contact");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/module-name", "ietf-system");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/path", "/ietf-system:system/hostname");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/module-name", "ietf-system");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/path", "/ietf-system:system/location");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='13']/module-name", "example");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='13']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='13']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/module-name", "*");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/action", "deny");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/group[.='optics']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/module-name", "ietf-system");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/action", "permit"); // overrides nacm:default-deny-* rules in ietf-system model
    srSess.applyChanges();

    REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
    REQUIRE(retrieveData("/ietf-interfaces:idk", {{"x-remote-user", "yangnobody"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Couldn't find schema node: /ietf-interfaces:idk"
      }
    ]
  }
}
)"});
    REQUIRE(retrieveData("/ietf-system:system/clock", {{"x-remote-user", "yangnobody"}}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-message": "No data from sysrepo."
      }
    ]
  }
}
)"});
    REQUIRE(retrieveData("/ietf-system:system/clock/timezone-utc-offset", {{"x-remote-user", "yangnobody"}}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-message": "No data from sysrepo."
      }
    ]
  }
}
)"});

    REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "dwdm"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location",
    "clock": {
      "timezone-utc-offset": 2
    },
    "radius": {
      "server": [
        {
          "name": "a",
          "udp": {
            "address": "1.1.1.1",
            "shared-secret": "shared-secret"
          }
        }
      ]
    }
  }
}
)"});

    REQUIRE(retrieveData("/ietf-interfaces:idk", {{"x-remote-user", "dwdm"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Couldn't find schema node: /ietf-interfaces:idk"
      }
    ]
  }
}
)"});
    REQUIRE(retrieveData("/ietf-system:system/clock", {{"x-remote-user", "dwdm"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "clock": {
      "timezone-utc-offset": 2
    }
  }
}
)"});

    REQUIRE(retrieveData("/ietf-system:system/radius/server", {{"x-remote-user", "norules"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "List '/ietf-system:system/radius/server' requires 1 keys"
      }
    ]
  }
}
)"});

    REQUIRE(retrieveData("/ietf-system:system/radius/server=a", {{"x-remote-user", "norules"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "radius": {
      "server": [
        {
          "name": "a",
          "udp": {
            "address": "1.1.1.1"
          }
        }
      ]
    }
  }
}
)"});

    DOCTEST_SUBCASE("Invalid NACM configurations")
    {
        DOCTEST_SUBCASE("Anonymous at first place but the wildcard-deny-all rule is missing")
        {
            srSess.deleteItem("/ietf-netconf-acm:nacm/rule-list");
            srSess.applyChanges();
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/group[.='yangnobody']", "");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/access-operations", "read");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/path", "/ietf-system:system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='2']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='2']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/group[.='optics']", "");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/action", "permit");
            srSess.applyChanges();
        }

        DOCTEST_SUBCASE("Anonymous at first place but the wildcard-deny-all rule is not last")
        {
            srSess.deleteItem("/ietf-netconf-acm:nacm/rule-list");
            srSess.applyChanges();
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/group[.='yangnobody']", "");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/access-operations", "read");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/path", "/ietf-system:system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='2']/module-name", "*");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='2']/action", "deny");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='3']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='3']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='3']/access-operations", "read");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='3']/path", "/ietf-system:system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/group[.='optics']", "");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/action", "permit");
            srSess.applyChanges();
        }

        DOCTEST_SUBCASE("Anonymous rulelist OK, but not at first place")
        {
            srSess.deleteItem("/ietf-netconf-acm:nacm/rule-list");
            srSess.applyChanges();
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/group[.='optics']", "");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/group[.='yangnobody']", "");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/module-name", "ietf-system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/access-operations", "read");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='1']/path", "/ietf-system:system");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='2']/module-name", "*");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='2']/action", "deny");
            srSess.applyChanges();
        }

        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}}) == Response{401, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "access-denied",
        "error-message": "Access denied."
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "dwdm"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location",
    "clock": {
      "timezone-utc-offset": 2
    },
    "radius": {
      "server": [
        {
          "name": "a",
          "udp": {
            "address": "1.1.1.1",
            "shared-secret": "shared-secret"
          }
        }
      ]
    }
  }
}
)"});
    }

    DOCTEST_SUBCASE("Basic querying of lists")
    {
        REQUIRE(retrieveData("/ietf-system:system/radius/server=a", {{"x-remote-user", "dwdm"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "radius": {
      "server": [
        {
          "name": "a",
          "udp": {
            "address": "1.1.1.1",
            "shared-secret": "shared-secret"
          }
        }
      ]
    }
  }
}
)"});

        REQUIRE(retrieveData("/ietf-system:system/radius/server=a/udp/address", {{"x-remote-user", "dwdm"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "radius": {
      "server": [
        {
          "name": "a",
          "udp": {
            "address": "1.1.1.1"
          }
        }
      ]
    }
  }
}
)"});

        REQUIRE(retrieveData("/ietf-system:system/radius/server=b", {{"x-remote-user", "dwdm"}}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-message": "No data from sysrepo."
      }
    ]
  }
}
)"});

        REQUIRE(retrieveData("/ietf-system:system/radius/server=a,b", {{"x-remote-user", "dwdm"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "List '/ietf-system:system/radius/server' requires 1 keys"
      }
    ]
  }
}
)"});
    }

    DOCTEST_SUBCASE("RPCs")
    {
        REQUIRE(retrieveData("/ietf-system:system-restart", {{"x-remote-user", "dwdm"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "'/ietf-system:system-restart' is not a data resource"
      }
    ]
  }
}
)"});

        REQUIRE(retrieveData("/example:l/list=eth0/example-action", {{"x-remote-user", "dwdm"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "'/example:l/list/example-action' is not a data resource"
      }
    ]
  }
}
)"});

        REQUIRE(retrieveData("/example:l/list=eth0/example-action/i", {{"x-remote-user", "dwdm"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "'/example:l/list/example-action' is not a data resource"
      }
    ]
  }
}
)"});
    }

    DOCTEST_SUBCASE("Test data formats preference")
    {
        const ng::header_map onlyCorsHeader{
            {"access-control-allow-origin", {"*", false}},
        };

        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "text/plain"}}) == Response{406, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "No requested format supported"
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "application/yang-data"}}) == Response{406, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "No requested format supported"
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"content-type", "text/plain"}}) == Response{415, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "content-type format value not supported"
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+jsonx"}}) == Response{415, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "content-type format value not supported"
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+xmlx"}}) == Response{415, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "content-type format value not supported"
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+json;charset=utf8"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "application/yang-data+xml"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "application/yang-data+xml,application/yang-data+json"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+xml"}, {"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "blabla"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "*/*"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "application/*"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "image/*"}}) == Response{406, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "No requested format supported"
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"content-type", "application/*"}}) == Response{415, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "content-type format value not supported"
      }
    ]
  }
}
)"});
        REQUIRE(retrieveData("/ietf-system:system", {{"x-remote-user", "yangnobody"}, {"accept", "application/yang-data+json;q=0.4,application/yang-data+xml"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
    }

    SECTION("PUT")
    {
        // yangnobody can't write into ietf-system
        REQUIRE(putData("/ietf-system:system", R"({"ietf-system:system":{"ietf-system:location":"prague"}}")", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+json"}}) == Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-message": "Access denied."
      }
    ]
  }
}
)"});

        // create and modify a leaf value
        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:top-level-leaf", "str"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{201, jsonHeaders, ""});
        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Modified, "/example:top-level-leaf", "other-str"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:top-level-leaf", R"({"example:top-level-leaf": "other-str"}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        // invalid path
        REQUIRE(putData("/example:nonsense", R"({"example:nonsense": "other-str"}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Couldn't find schema node: /example:nonsense"
      }
    ]
  }
}
)"});

        // invalid path in data
        REQUIRE(putData("/example:top-level-leaf", R"({"example:nonsense": "other-str"}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: Can't parse data: LY_EVALID"
      }
    ]
  }
}
)"});

        // no mock required here - no change as enabled has default value true
        REQUIRE(putData("/example:a", R"({"example:a":{"b":{"c":{"enabled":true}}}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Modified, "/example:a/b/c/enabled", "false"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/b/c", R"({"example:c":{"enabled":false}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Modified, "/example:a/b/c/enabled", "true"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/b/c/enabled", R"({"example:enabled":true}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:a/b/c/l", "val"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/b/c/l", R"({"example:l":"val"}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{201, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Deleted, "/example:a/b/c/l", "val"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/b", R"({"example:b": {}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:a/b/c/l", "ahoj"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/b", R"({"example:b": {"c": {"l": "ahoj"}}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Modified, "/example:a/b/c/enabled", "false"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/b/c/enabled", R"({"example:enabled": false}}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        // invalid data value - boolean literal in quotes
        REQUIRE(putData("/example:a", R"({"example:a":{"b":{"c":{"enabled":"false"}}}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: Can't parse data: LY_EVALID"
      }
    ]
  }
}
)"});

        // invalid data value - wrong path
        REQUIRE(putData("/example:a/b/c", R"({"example:enabled":false}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: DataNode::parseSubtree: lyd_parse_data failed: LY_EVALID"
      }
    ]
  }
}
)"});

        REQUIRE(putData("/example:a/b/c/enabled", R"({"example:l":"hey"}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Invalid data for PUT (Top-level node name mismatch)."
      }
    ]
  }
}
)"});
        REQUIRE(putData("/example:a/example-augment:b", R"({"example:b": {}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Invalid data for PUT (Top-level node name mismatch)."
      }
    ]
  }
}
)"});

        // we duplicate same structure in /example:a/b and /example:a/example-augment:b - test if we correctly manage namespaces
        // I am also providing a namespace with enabled leaf - this should work as well although not needed
        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Modified, "/example:a/example-augment:b/c/enabled", "false"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/example-augment:b", R"({"example-augment:b": { "c" : {"example-augment:enabled" : false}}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});
        REQUIRE(retrieveData("/example:a", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "example:a": {
    "b": {
      "c": {
        "enabled": false,
        "l": "ahoj"
      }
    },
    "example-augment:b": {
      "c": {
        "enabled": false
      }
    }
  }
}
)"});

        // test overwrite whole container (poor man's delete)
        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Modified, "/example:a/b/c/enabled", "true"}, {sysrepo::ChangeOperation::Deleted, "/example:a/b/c/l", "ahoj"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/example:b", R"({"example:b": {}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        // test xml data
        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:a/b/c/l", "libyang is love"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:a/b", R"(<b xmlns="http://example.tld/example"><c><l>libyang is love</l></c></b>)", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+xml"}}) == Response{204, xmlHeaders, ""});

        // test list operations
        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:top-level-list", std::nullopt}, {sysrepo::ChangeOperation::Created, "/example:top-level-list/name", "sysrepo"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:top-level-list=sysrepo", R"({"example:top-level-list":[{"name": "sysrepo"}]})", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{201, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:l/list", std::nullopt}, {sysrepo::ChangeOperation::Created, "/example:l/list/name", "libyang"}, {sysrepo::ChangeOperation::Created, "/example:l/list/choice1", "libyang"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:l/list=libyang", R"({"example:list":[{"name": "libyang", "choice1": "libyang"}]})", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{201, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:l/list", std::nullopt}, {sysrepo::ChangeOperation::Created, "/example:l/list/name", "netconf"}, {sysrepo::ChangeOperation::Created, "/example:l/list/choice1", "netconf"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:l/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "netconf"}]})", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{201, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change((std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:l/list", std::nullopt}, {sysrepo::ChangeOperation::Created, "/example:l/list/name", "sysrepo"}, {sysrepo::ChangeOperation::Created, "/example:l/list/nested", std::nullopt}, {sysrepo::ChangeOperation::Created, "/example:l/list/nested/first", "1"}, {sysrepo::ChangeOperation::Created, "/example:l/list/nested/second", "2"}, {sysrepo::ChangeOperation::Created, "/example:l/list/nested/third", "3"}, {sysrepo::ChangeOperation::Created, "/example:l/list/choice2", "sysrepo"}}))).IN_SEQUENCE(seq1).TIMES(1);
        REQUIRE(putData("/example:l/list=sysrepo", R"({"example:list":[{"name": "sysrepo", "choice2": "sysrepo", "example:nested": [{"first": "1", "second": 2, "third": "3"}]}]})", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{201, jsonHeaders, ""});

        REQUIRE_CALL(changeMock, change(std::vector<SrChange>{{sysrepo::ChangeOperation::Created, "/example:l/list/collection", "1"}, {sysrepo::ChangeOperation::Created, "/example:l/list/collection", "2"}, {sysrepo::ChangeOperation::Created, "/example:l/list/collection", "3"}, {sysrepo::ChangeOperation::Modified, "/example:l/list/choice1", "snmp"}}));
        REQUIRE(putData("/example:l/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "snmp", "collection": [1,2,3]}]})", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+json"}}) == Response{204, jsonHeaders, ""});

        REQUIRE(retrieveData("/example:l", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "example:l": {
    "list": [
      {
        "name": "libyang",
        "choice1": "libyang"
      },
      {
        "name": "netconf",
        "collection": [
          1,
          2,
          3
        ],
        "choice1": "snmp"
      },
      {
        "name": "sysrepo",
        "nested": [
          {
            "first": "1",
            "second": 2,
            "third": "3"
          }
        ],
        "choice2": "sysrepo"
      }
    ]
  }
}
)"});

        // content-type header is mandatory for PUT
        REQUIRE(putData("/example:a/example-augment:b", R"({"example-augment:b": { "c" : {"enabled" : false}}}")", {{"x-remote-user", "root"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Content-type header missing."
      }
    ]
  }
}
)"});

        // mismatch between content-type and actual data type
        REQUIRE(putData("/example:a/b", R"({"example:b": {"example:c": {"l": "ahoj"}}}")", {{"x-remote-user", "root"}, {"content-type", "application/yang-data+xml"}}) == Response{400, xmlHeaders, R"(<errors xmlns="urn:ietf:params:xml:ns:yang:ietf-restconf">
  <error>
    <error-type>application</error-type>
    <error-tag>invalid-value</error-tag>
    <error-message>Validation failure: DataNode::parseSubtree: lyd_parse_data failed: LY_EVALID</error-message>
  </error>
</errors>
)"});
        // TODO PUT leaf-list test
    }
}

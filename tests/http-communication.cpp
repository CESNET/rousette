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

#define _CHANGE(OP, KEY, VAL) \
    {                         \
        OP, KEY, VAL          \
    }
#define CREATED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Created, KEY, VAL)
#define MODIFIED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Modified, KEY, VAL)
#define DELETED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Deleted, KEY, VAL)
#define EXPECT_CHANGE(...) REQUIRE_CALL(changeMock, change((std::vector<SrChange>{__VA_ARGS__}))).IN_SEQUENCE(seq1).TIMES(1);

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

#define AUTH_DWDM {"authorization", "Basic ZHdkbTpEV0RN"}
#define AUTH_ROOT {"authorization", "Basic cm9vdDpzZWtyaXQ="}
#define AUTH_NORULES {"authorization", "Basic bm9ydWxlczplbXB0eQ=="}
#define AUTH_ROOT {"authorization", "Basic cm9vdDpzZWtyaXQ="}

#define CONTENT_TYPE_JSON {"content-type", "application/yang-data+json"}
#define CONTENT_TYPE_XML {"content-type", "application/yang-data+xml"}

Response clientRequest(auto method, auto xpath, const std::map<std::string, std::string>& headers, const std::string& data)
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

        auto req = client.submit(ec, method, SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath, data, reqHeaders);

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

Response get(auto xpath, const std::map<std::string, std::string>& headers)
{
    return clientRequest("GET", xpath, headers, "");
}

Response put(auto xpath, const std::string& data, const std::map<std::string, std::string>& headers)
{
    return clientRequest("PUT", xpath, headers, data);
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
            changes.emplace_back(change.operation, change.node.path(), val);
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


    // anonymous access doesn't work without magic NACM rules
    REQUIRE(get("/ietf-system:system", {}) == Response{401, jsonHeaders, R"({
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

    // we do not support these http methods yet
    for (const auto& httpMethod : {"OPTIONS"s, "POST"s, "PATCH"s, "DELETE"s}) {
        CAPTURE(httpMethod);
        REQUIRE(clientRequest(httpMethod, "/ietf-system:system", {AUTH_ROOT}, "") == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "Method not allowed."
      }
    ]
  }
}
)"});
    }

    REQUIRE(get("", {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});

    REQUIRE(get("/ietf-system:system", {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
    REQUIRE(get("/ietf-interfaces:idk", {}) == Response{400, jsonHeaders, R"({
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
    REQUIRE(get("/ietf-system:system/clock", {}) == Response{404, jsonHeaders, R"({
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
    REQUIRE(get("/ietf-system:system/clock/timezone-utc-offset", {}) == Response{404, jsonHeaders, R"({
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

    REQUIRE(get("/ietf-system:system", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
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

    // wrong password
    REQUIRE(get("/ietf-system:system", {{"authorization", "Basic ZHdkbTpGQUlM"}}) == Response{401, jsonHeaders, R"({
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

    REQUIRE(get("/ietf-interfaces:idk", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
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
    REQUIRE(get("/ietf-system:system/clock", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "clock": {
      "timezone-utc-offset": 2
    }
  }
}
)"});

    REQUIRE(get("/ietf-system:system/radius/server", {AUTH_NORULES}) == Response{400, jsonHeaders, R"({
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

    REQUIRE(get("/ietf-system:system/radius/server=a", {AUTH_NORULES}) == Response{200, jsonHeaders, R"({
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

        REQUIRE(get("/ietf-system:system", {}) == Response{401, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system/radius/server=a", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
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

        REQUIRE(get("/ietf-system:system/radius/server=a/udp/address", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
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

        REQUIRE(get("/ietf-system:system/radius/server=b", {AUTH_DWDM}) == Response{404, jsonHeaders, R"({
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

        REQUIRE(get("/ietf-system:system/radius/server=a,b", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system-restart", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
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

        REQUIRE(get("/example:l/list=eth0/example-action", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
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

        REQUIRE(get("/example:l/list=eth0/example-action/i", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
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

        REQUIRE(get("/ietf-system:system", {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {{"accept", "text/plain"}}) == Response{406, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"accept", "application/yang-data"}}) == Response{406, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"content-type", "text/plain"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {CONTENT_TYPE_JSON}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {{"content-type", "application/yang-data+jsonx"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"content-type", "application/yang-data+xmlx"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"content-type", "application/yang-data+json;charset=utf8"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {{"accept", "application/yang-data+xml"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
        REQUIRE(get("/ietf-system:system", {{"accept", "application/yang-data+xml,application/yang-data+json"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
        REQUIRE(get("/ietf-system:system", {CONTENT_TYPE_XML, {"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {{"accept", "blabla"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {{"accept", "*/*"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {{"accept", "application/*"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get("/ietf-system:system", {{"accept", "image/*"}}) == Response{406, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"content-type", "application/*"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"accept", "application/yang-data+json;q=0.4,application/yang-data+xml"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
    }

    SECTION("PUT")
    {
        // PUT on datastore resource (/restconf/data) is not a valid operation
        REQUIRE(put("", "", {CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-not-supported",
        "error-message": "Invalid URI for PUT request"
      }
    ]
  }
}
)"});

        // anonymous can't write into ietf-system
        REQUIRE(put("/ietf-system:system", R"({"ietf-system:system":{"ietf-system:location":"prague"}}")", {CONTENT_TYPE_JSON}) == Response{403, jsonHeaders, R"({
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
        EXPECT_CHANGE(CREATED("/example:top-level-leaf", "str"));
        REQUIRE(put("/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
        EXPECT_CHANGE(MODIFIED("/example:top-level-leaf", "other-str"));
        REQUIRE(put("/example:top-level-leaf", R"({"example:top-level-leaf": "other-str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // invalid path
        // FIXME: add error-path reporting for wrong URIs according to https://datatracker.ietf.org/doc/html/rfc8040#page-78
        REQUIRE(put("/example:nonsense", R"({"example:nonsense": "other-str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
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
        REQUIRE(put("/example:top-level-leaf", R"({"example:nonsense": "other-str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
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
        REQUIRE(put("/example:a", R"({"example:a":{"b":{"c":{"enabled":true}}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        EXPECT_CHANGE(MODIFIED("/example:a/b/c/enabled", "false"));
        REQUIRE(put("/example:a/b/c", R"({"example:c":{"enabled":false}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        EXPECT_CHANGE(MODIFIED("/example:a/b/c/enabled", "true"));
        REQUIRE(put("/example:a/b/c/enabled", R"({"example:enabled":true}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        EXPECT_CHANGE(CREATED("/example:a/b/c/l", "val"));
        REQUIRE(put("/example:a/b/c/l", R"({"example:l":"val"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        EXPECT_CHANGE(DELETED("/example:a/b/c/l", "val"));
        REQUIRE(put("/example:a/b", R"({"example:b": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        EXPECT_CHANGE(CREATED("/example:a/b/c/l", "ahoj"));
        REQUIRE(put("/example:a/b", R"({"example:b": {"c": {"l": "ahoj"}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        EXPECT_CHANGE(MODIFIED("/example:a/b/c/enabled", "false"));
        REQUIRE(put("/example:a/b/c/enabled", R"({"example:enabled": false}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // invalid data value - boolean literal in quotes
        REQUIRE(put("/example:a", R"({"example:a":{"b":{"c":{"enabled":"false"}}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
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

        // invalid data value - wrong path: enabled leaf is not located under node b and libyang-cpp throws
        REQUIRE(put("/example:a/b/c", R"({"example:enabled":false}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
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

        // invalid data value - wrong path: leaf l is located under node c but we check that URI path corresponds to the leaf we parse
        REQUIRE(put("/example:a/b/c/enabled", R"({"example:l":"hey"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:a/b/c/l",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});

        // put correct element but also its sibling
        REQUIRE(put("/example:a/b/c/enabled", R"({"example:enabled":false, "example:l": "nope"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:a/b/c/l",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});

        // different node specified in URL than in the data (same name but namespaces differ)
        REQUIRE(put("/example:a/example-augment:b", R"({"example:b": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:a/b",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});
        // different top-level node in the data than the URL indicates
        REQUIRE(put("/example:a", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:top-level-leaf",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});
        REQUIRE(put("/example:top-level-list=aaa", R"({"example:top-level-leaf": "a"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:top-level-leaf",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});

        // there are two childs named 'b' under /example:a but both inside different namespaces (/example:a/b and /example:a/example-augment:b)
        // I am also providing a namespace with enabled leaf - this should work as well although not needed
        EXPECT_CHANGE(MODIFIED("/example:a/example-augment:b/c/enabled", "false"));
        REQUIRE(put("/example:a/example-augment:b", R"({"example-augment:b": {"c":{"example-augment:enabled":false}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
        REQUIRE(get("/example:a", {{"x-remote-user", "yangnobody"}, CONTENT_TYPE_JSON}) == Response{200, jsonHeaders, R"({
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
        EXPECT_CHANGE(
            MODIFIED("/example:a/b/c/enabled", "true"),
            DELETED("/example:a/b/c/l", "ahoj"));
        REQUIRE(put("/example:a/example:b", R"({"example:b": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // test xml data
        EXPECT_CHANGE(CREATED("/example:a/b/c/l", "libyang is love"));
        REQUIRE(put("/example:a/b", R"(<b xmlns="http://example.tld/example"><c><l>libyang is love</l></c></b>)", {AUTH_ROOT, CONTENT_TYPE_XML}) == Response{204, xmlHeaders, ""});

        // test list operations
        // basic insert into a top-level list
        EXPECT_CHANGE(
            CREATED("/example:top-level-list[name='sysrepo']", std::nullopt),
            CREATED("/example:top-level-list[name='sysrepo']/name", "sysrepo"));
        REQUIRE(put("/example:top-level-list=sysrepo", R"({"example:top-level-list":[{"name": "sysrepo"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // basic insert into not-a-top-level list twice (just to check that both list entries are preserved)
        EXPECT_CHANGE(
            CREATED("/example:l/list[name='libyang']", std::nullopt),
            CREATED("/example:l/list[name='libyang']/name", "libyang"),
            CREATED("/example:l/list[name='libyang']/choice1", "libyang"));
        REQUIRE(put("/example:l/list=libyang", R"({"example:list":[{"name": "libyang", "choice1": "libyang"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        EXPECT_CHANGE(
            CREATED("/example:l/list[name='netconf']", std::nullopt),
            CREATED("/example:l/list[name='netconf']/name", "netconf"),
            CREATED("/example:l/list[name='netconf']/choice1", "netconf"));
        REQUIRE(put("/example:l/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "netconf"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // insert more complicated list entry into a list
        EXPECT_CHANGE(
            CREATED("/example:l/list[name='sysrepo']", std::nullopt),
            CREATED("/example:l/list[name='sysrepo']/name", "sysrepo"),
            CREATED("/example:l/list[name='sysrepo']/nested[first='1'][second='2'][third='3']", std::nullopt),
            CREATED("/example:l/list[name='sysrepo']/nested[first='1'][second='2'][third='3']/first", "1"),
            CREATED("/example:l/list[name='sysrepo']/nested[first='1'][second='2'][third='3']/second", "2"),
            CREATED("/example:l/list[name='sysrepo']/nested[first='1'][second='2'][third='3']/third", "3"),
            CREATED("/example:l/list[name='sysrepo']/choice2", "sysrepo"));
        REQUIRE(put("/example:l/list=sysrepo", R"({"example:list":[{"name": "sysrepo", "choice2": "sysrepo", "example:nested": [{"first": "1", "second": 2, "third": "3"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // previous test created a nested list in a list. Add new entry there
        EXPECT_CHANGE(
            CREATED("/example:l/list[name='sysrepo']/nested[first='11'][second='12'][third='13']", std::nullopt),
            CREATED("/example:l/list[name='sysrepo']/nested[first='11'][second='12'][third='13']/first", "11"),
            CREATED("/example:l/list[name='sysrepo']/nested[first='11'][second='12'][third='13']/second", "12"),
            CREATED("/example:l/list[name='sysrepo']/nested[first='11'][second='12'][third='13']/third", "13"));
        REQUIRE(put("/example:l/list=sysrepo/nested=11,12,13", R"({"example:nested": [{"first": "11", "second": 12, "third": "13"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // modify a leaf in a list
        EXPECT_CHANGE(MODIFIED("/example:l/list[name='netconf']/choice1", "restconf"));
        REQUIRE(put("/example:l/list=netconf/choice1", R"({"example:choice1": "restconf"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // add values to leaf-lists
        EXPECT_CHANGE(CREATED("/example:top-level-leaf-list[.='4']", "4"));
        REQUIRE(put("/example:top-level-leaf-list=4", R"({"example:top-level-leaf-list":[4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
        EXPECT_CHANGE(CREATED("/example:top-level-leaf-list[.='1']", "1"));
        REQUIRE(put("/example:top-level-leaf-list=1", R"({"example:top-level-leaf-list":[1]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
        EXPECT_CHANGE(CREATED("/example:l/list[name='netconf']/collection[.='4']", "4"));
        REQUIRE(put("/example:l/list=netconf/collection=4", R"({"example:collection": [4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // overwrite list entry
        EXPECT_CHANGE(
            DELETED("/example:l/list[name='netconf']/collection[.='4']", "4"),
            CREATED("/example:l/list[name='netconf']/collection[.='1']", "1"),
            CREATED("/example:l/list[name='netconf']/collection[.='2']", "2"),
            CREATED("/example:l/list[name='netconf']/collection[.='3']", "3"),
            MODIFIED("/example:l/list[name='netconf']/choice1", "snmp"));
        REQUIRE(put("/example:l/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "snmp", "collection": [1,2,3]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // send wrong keys
        REQUIRE(put("/example:l/list=netconf", R"({"example:list":[{"name": "ahoj", "choice1": "nope"}]})", {AUTH_ROOT, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:l/list[name='ahoj']/name",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});
        REQUIRE(put("/example:top-level-list=netconf", R"({"example:top-level-list":[{"name": "ahoj"}]})", {AUTH_ROOT, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:top-level-list[name='ahoj']/name",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});
        REQUIRE(put("/example:l/list=netconf/collection=667", R"({"example:collection":[666]})", {AUTH_ROOT, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:l/list[name='netconf']/collection[.='666']",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});
        REQUIRE(put("/example:top-level-leaf-list=667", R"({"example:top-level-leaf-list":[666]})", {AUTH_ROOT, {"content-type", "application/yang-data+json"}}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:top-level-leaf-list[.='666']",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});

        REQUIRE(get("/example:l", {{"x-remote-user", "yangnobody"}, {"content-type", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
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
          },
          {
            "first": "11",
            "second": 12,
            "third": "13"
          }
        ],
        "choice2": "sysrepo"
      }
    ]
  }
}
)"});

        // content-type header is mandatory for PUT
        REQUIRE(put("/example:a/example-augment:b", R"({"example-augment:b": { "c" : {"enabled" : false}}}")", {AUTH_ROOT}) == Response{400, jsonHeaders, R"({
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
        REQUIRE(put("/example:a/b", R"({"example:b": {"example:c": {"l": "ahoj"}}}")", {AUTH_ROOT, CONTENT_TYPE_XML}) == Response{400, xmlHeaders, R"(<errors xmlns="urn:ietf:params:xml:ns:yang:ietf-restconf">
  <error>
    <error-type>application</error-type>
    <error-tag>invalid-value</error-tag>
    <error-message>Validation failure: DataNode::parseSubtree: lyd_parse_data failed: LY_EVALID</error-message>
  </error>
</errors>
)"});
    }
}

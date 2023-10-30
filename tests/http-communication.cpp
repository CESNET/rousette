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
#include <sysrepo-cpp/Session.hpp>
#include "restconf/Server.h"
#include "tests/UniqueResource.h"
#include "configure.cmake.h"

using namespace std::string_literals;
namespace ng = nghttp2::asio_http2;
namespace ng_client = ng::client;

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
#define AUTH_NORULES {"authorization", "Basic bm9ydWxlczplbXB0eQ=="}

Response clientRequest(auto method, auto xpath, const std::map<std::string, std::string>& headers)
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

        auto req = client.submit(ec, method, SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath, reqHeaders);
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
    return clientRequest("GET", xpath, headers);
}

TEST_CASE("HTTP")
{
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.copyConfig(sysrepo::Datastore::Startup, "ietf-netconf-acm");

    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT, std::filesystem::path(CMAKE_CURRENT_BINARY_DIR) / "tests" / "pam"};
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
    srSess.switchDatastore(sysrepo::Datastore::Operational);
    srSess.setItem("/ietf-system:system/contact", "contact");
    srSess.setItem("/ietf-system:system/hostname", "hostname");
    srSess.setItem("/ietf-system:system/location", "location");
    srSess.setItem("/ietf-system:system/clock/timezone-utc-offset", "2");
    srSess.setItem("/ietf-system:system/radius/server[name='a']/udp/address", "1.1.1.1");
    srSess.setItem("/ietf-system:system/radius/server[name='a']/udp/shared-secret", "shared-secret");
    srSess.applyChanges();

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
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/module-name", "*");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/action", "deny");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/group[.='optics']", "");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/module-name", "ietf-system");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/action", "permit"); // overrides nacm:default-deny-* rules in ietf-system model
    srSess.applyChanges();

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
        REQUIRE(get("/ietf-system:system", {{"content-type", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
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
        REQUIRE(get("/ietf-system:system", {{"content-type", "application/yang-data+xml"}, {"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
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
}

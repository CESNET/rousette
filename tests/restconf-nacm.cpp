/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10082";
#include "tests/aux-utils.h"
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"

TEST_CASE("NACM")
{
    spdlog::set_level(spdlog::level::trace);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};

    // something we can read
    srSess.switchDatastore(sysrepo::Datastore::Operational);
    srSess.setItem("/ietf-system:system/contact", "contact");
    srSess.setItem("/ietf-system:system/hostname", "hostname");
    srSess.setItem("/ietf-system:system/location", "location");
    srSess.setItem("/ietf-system:system/clock/timezone-utc-offset", "2");
    srSess.setItem("/ietf-system:system/radius/server[name='a']/udp/address", "1.1.1.1");
    srSess.setItem("/ietf-system:system/radius/server[name='a']/udp/shared-secret", "shared-secret");
    srSess.applyChanges();
    srSess.switchDatastore(sysrepo::Datastore::Running);

    DOCTEST_SUBCASE("no rules")
    {
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
    }

    DOCTEST_SUBCASE("Invalid NACM configurations")
    {
        srSess.setItem("/ietf-netconf-acm:nacm/enable-external-groups", "false");
        srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='optics']/user-name[.='dwdm']", "");
        srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='yangnobody']/user-name[.='yangnobody']", "");
        srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='norules']/user-name[.='norules']", "");

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

    DOCTEST_SUBCASE("standard NACM rules")
    {
        // setup real-like NACM
        setupRealNacm(srSess);

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

        {
            // wrong password: the server should delay its response, so let the client wait "long enough"
            const auto start = std::chrono::steady_clock::now();
            REQUIRE(clientRequest("GET",
                        "/ietf-system:system",
                        {{"authorization", "Basic ZHdkbTpGQUlM"}},
                        boost::posix_time::seconds(5))
                    == Response{401, jsonHeaders, R"({
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
            auto processingMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            REQUIRE(processingMS <= 5'000);
            REQUIRE(processingMS >= 1'500);
        }

        {
            // wrong password: the server should delay its response, in this case let the client terminate its
            // request and check that the server doesn't crash
            const auto start = std::chrono::steady_clock::now();
            REQUIRE_THROWS_WITH(clientRequest("GET",
                        "/ietf-system:system",
                        {{"authorization", "Basic ZHdkbTpGQUlM"}},
                        boost::posix_time::milliseconds(100)),
                    "HTTP client error: Connection timed out");
            auto processingMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            REQUIRE(processingMS <= 500);
        }

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
    }
}

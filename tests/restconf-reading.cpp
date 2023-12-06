/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10081";
#include "tests/aux-utils.h"
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"

TEST_CASE("reading data")
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

    // setup real-like NACM
    setupRealNacm(srSess);

    DOCTEST_SUBCASE("unsupported methods")
    {
        // we do not support these http methods yet
        for (const auto& httpMethod : {"OPTIONS"s, "POST"s, "PATCH"s, "DELETE"s}) {
            CAPTURE(httpMethod);
            REQUIRE(clientRequest(httpMethod, RESTCONF_DATA_ROOT "/ietf-system:system", "", {AUTH_ROOT}) == Response{405, jsonHeaders, R"({
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
    }

    DOCTEST_SUBCASE("entire datastore")
    {
        // this relies on a NACM rule for anonymous access that filters out "a lot of stuff"
        REQUIRE(get(RESTCONF_DATA_ROOT, {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});

        REQUIRE(get(RESTCONF_ROOT_DS("operational"), {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});

        REQUIRE(get(RESTCONF_ROOT_DS("running"), {}) == Response{200, jsonHeaders, R"({

}
)"});
    }

    DOCTEST_SUBCASE("subtree")
    {
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system/clock", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "clock": {
      "timezone-utc-offset": 2
    }
  }
}
)"});
    }

    DOCTEST_SUBCASE("Basic querying of lists")
    {
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system/radius/server=a", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
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

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system/radius/server=a/udp/address", {AUTH_DWDM}) == Response{200, jsonHeaders, R"({
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

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system/radius/server=b", {AUTH_DWDM}) == Response{404, jsonHeaders, R"({
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

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system/radius/server=a,b", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system-restart", {AUTH_DWDM}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-not-supported",
        "error-message": "'/ietf-system:system-restart' is not a data resource"
      }
    ]
  }
}
)"});

        REQUIRE(get(RESTCONF_DATA_ROOT "/example:tlc/list=eth0/example-action", {AUTH_DWDM}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-not-supported",
        "error-message": "'/example:tlc/list/example-action' is not a data resource"
      }
    ]
  }
}
)"});

        REQUIRE(get(RESTCONF_DATA_ROOT "/example:tlc/list=eth0/example-action/i", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "'/example:tlc/list/example-action/i' is not a data resource"
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

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "text/plain"}}) == Response{406, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "application/yang-data"}}) == Response{406, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"content-type", "text/plain"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"content-type", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"content-type", "application/yang-data+jsonx"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"content-type", "application/yang-data+xmlx"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"content-type", "application/yang-data+json;charset=utf8"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "application/yang-data+xml"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "application/yang-data+xml,application/yang-data+json"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"content-type", "application/yang-data+xml"}, {"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "blabla"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "*/*"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "application/*"}}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "image/*"}}) == Response{406, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"content-type", "application/*"}}) == Response{415, jsonHeaders, R"({
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
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {{"accept", "application/yang-data+json;q=0.4,application/yang-data+xml"}}) == Response{200, xmlHeaders, R"(<system xmlns="urn:ietf:params:xml:ns:yang:ietf-system">
  <contact>contact</contact>
  <hostname>hostname</hostname>
  <location>location</location>
</system>
)"});
    }

    SECTION("NMDA (RFC 8527)")
    {
        srSess.switchDatastore(sysrepo::Datastore::Startup);
        srSess.setItem("/ietf-system:system/contact", "startup-contact");
        srSess.applyChanges();

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-system:system", {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "contact",
    "hostname": "hostname",
    "location": "location"
  }
}
)"});

        REQUIRE(get(RESTCONF_ROOT_DS("startup") "/ietf-system:system", {}) == Response{200, jsonHeaders, R"({
  "ietf-system:system": {
    "contact": "startup-contact"
  }
}
)"});
    }
}

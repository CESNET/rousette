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

#define FORWARDED {"forward", "proto=http;host=example.net"}

TEST_CASE("obtaining YANG schemas")
{
    spdlog::set_level(spdlog::level::trace);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};

    SECTION("Locations are overwritten")
    {
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:yang-library/module-set=complete/module=ietf-yang-library", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:yang-library": {
    "module-set": [
      {
        "name": "complete",
        "module": [
          {
            "name": "ietf-yang-library",
            "revision": "2019-01-04",
            "namespace": "urn:ietf:params:xml:ns:yang:ietf-yang-library",
            "location": [
              "http://example.net/yang/ietf-yang-library@2019-01-04"
            ]
          }
        ]
      }
    ]
  }
}
)"});

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:yang-library/module-set=complete/import-only-module=ietf-inet-types,2013-07-15", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:yang-library": {
    "module-set": [
      {
        "name": "complete",
        "import-only-module": [
          {
            "name": "ietf-inet-types",
            "revision": "2013-07-15",
            "namespace": "urn:ietf:params:xml:ns:yang:ietf-inet-types",
            "location": [
              "http://example.net/yang/ietf-inet-types@2013-07-15"
            ]
          }
        ]
      }
    ]
  }
}
)"});

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:modules-state/module=ietf-yang-library,2019-01-04", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:modules-state": {
    "module": [
      {
        "name": "ietf-yang-library",
        "revision": "2019-01-04",
        "schema": "http://example.net/yang/ietf-yang-library@2019-01-04",
        "namespace": "urn:ietf:params:xml:ns:yang:ietf-yang-library",
        "conformance-type": "implement"
      }
    ]
  }
}
)"});

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:modules-state/module=example,", {AUTH_ROOT, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:modules-state": {
    "module": [
      {
        "name": "example",
        "revision": "",
        "schema": "http://example.net/yang/example",
        "namespace": "http://example.tld/example",
        "feature": [
          "f1"
        ],
        "conformance-type": "implement"
      }
    ]
  }
}
)"});
    }

    SECTION("get YANG schema")
    {
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

    SECTION("NACM filters ietf-yang-library nodes")
    {
        srSess.switchDatastore(sysrepo::Datastore::Running);
        srSess.setItem("/ietf-netconf-acm:nacm/enable-external-groups", "false");
        srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='norules']/user-name[.='norules']", "");

        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/group[.='norules']", "");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/module-name", "ietf-yang-library");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/action", "permit");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/access-operations", "read");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/path", "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/module-name", "ietf-yang-library");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/action", "deny");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/path", "/ietf-yang-library:yang-library/module-set[name='complete']");
        srSess.applyChanges();

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:yang-library/module-set=complete", {AUTH_NORULES, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:yang-library": {
    "module-set": [
      {
        "name": "complete",
        "module": [
          {
            "name": "ietf-yang-library",
            "revision": "2019-01-04",
            "namespace": "urn:ietf:params:xml:ns:yang:ietf-yang-library",
            "location": [
              "http://example.net/yang/ietf-yang-library@2019-01-04"
            ]
          }
        ]
      }
    ]
  }
}
)"});
    }
}

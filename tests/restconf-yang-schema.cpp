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
                REQUIRE(clientRequest(httpMethod, YANG_ROOT "/ietf-yang-library@2019-01-04", "", {AUTH_ROOT}) == Response{405, noContentTypeHeaders, ""});
            }
        }

        SECTION("loaded modules")
        {
            SECTION("module with revision")
            {
                SECTION("no revision in uri")
                {
                    REQUIRE(get(YANG_ROOT "/ietf-system", {AUTH_ROOT}) == Response{404, plaintextHeaders, "YANG schema not found"});
                }
                SECTION("correct revision in uri")
                {
                    auto resp = get(YANG_ROOT "/ietf-system@2014-08-06", {AUTH_ROOT});
                    auto expectedShortenedResp = Response{200, yangHeaders, "module ietf-system {\n  namespa"};

                    REQUIRE(resp.equalStatusCodeAndHeaders(expectedShortenedResp));
                    REQUIRE(resp.data.substr(0, 30) == expectedShortenedResp.data);
                }
                SECTION("wrong revision in uri")
                {
                    REQUIRE(get(YANG_ROOT "/ietf-system@1999-12-13", {AUTH_ROOT}) == Response{404, plaintextHeaders, "YANG schema not found"});
                    REQUIRE(get(YANG_ROOT "/ietf-system@abcd-ef-gh", {AUTH_ROOT}) == Response{404, plaintextHeaders, "YANG schema not found"});
                }
                SECTION("wrong password")
                {
                    REQUIRE(clientRequest("GET", YANG_ROOT "/ietf-system@2014-08-06", "",
                                {{"authorization", "Basic ZHdkbTpGQUlM"}}, boost::posix_time::seconds{5})
                            == Response{401, plaintextHeaders, "Access denied."});
                }
            }

            SECTION("module without revision")
            {
                SECTION("revision in uri")
                {
                    REQUIRE(get(YANG_ROOT "/example@2020-02-02", {AUTH_ROOT}) == Response{404, plaintextHeaders, "YANG schema not found"});
                }
                SECTION("no revision in uri")
                {
                    std::string moduleName;
                    std::string expectedResponseStart;

                    SECTION("loaded module")
                    {
                        moduleName = "example";
                        expectedResponseStart = "module example {";
                    }
                    SECTION("loaded submodule")
                    {
                        moduleName = "root-submod";
                        expectedResponseStart = "submodule root-submod {";
                    }
                    SECTION("imported module")
                    {
                        moduleName = "imp-mod";
                        expectedResponseStart = "module imp-mod {";
                    }
                    SECTION("imported submodule")
                    {
                        moduleName = "imp-submod";
                        expectedResponseStart = "submodule imp-submod {";
                    }

                    auto resp = get(YANG_ROOT "/" + moduleName, {AUTH_ROOT});
                    auto expectedShortenedResp = Response{200, yangHeaders, expectedResponseStart};

                    REQUIRE(resp.equalStatusCodeAndHeaders(expectedShortenedResp));
                    REQUIRE(resp.data.substr(0, expectedResponseStart.size()) == expectedShortenedResp.data);
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

        SECTION("Only ietf-yang-library accessible")
        {
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

            REQUIRE(get(YANG_ROOT "/ietf-system@2014-08-06", {AUTH_NORULES}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/ietf-system@2014-08-06", {AUTH_NORULES, FORWARDED}) == Response{404, plaintextHeaders, "YANG schema not found"});

            {
                auto resp = get(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_NORULES, FORWARDED});
                REQUIRE(resp.equalStatusCodeAndHeaders(Response{200, yangHeaders, ""}));
                REQUIRE(resp.data.substr(0, 26) == "module ietf-yang-library {");
            }

            REQUIRE(get(YANG_ROOT "/inp-mod", {AUTH_NORULES}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/inp-submod", {AUTH_NORULES}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/root-mod", {AUTH_NORULES}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/root-submod", {AUTH_NORULES}) == Response{404, plaintextHeaders, "YANG schema not found"});
        }

        SECTION("root-mod is accessible, therefore also root-submod is accessible")
        {
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/module-name", "ietf-yang-library");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/access-operations", "read");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/path", "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='root-mod']");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/module-name", "ietf-yang-library");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/action", "deny");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/path", "/ietf-yang-library:yang-library/module-set[name='complete']");
            srSess.applyChanges();

            REQUIRE(get(YANG_ROOT "/inp-mod", {AUTH_NORULES}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/inp-submod", {AUTH_NORULES}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/root-mod", {AUTH_NORULES}).statusCode == 200);
            REQUIRE(get(YANG_ROOT "/root-submod", {AUTH_NORULES}).statusCode == 200);
        }

        SECTION("root-submod is accessible, this enables the parent module as well")
        {
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/module-name", "ietf-yang-library");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/access-operations", "read");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/path", "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='root-mod']/submodule[name='root-submod']");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/module-name", "ietf-yang-library");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/action", "deny");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/path", "/ietf-yang-library:yang-library/module-set[name='complete']");
            srSess.applyChanges();

            REQUIRE(get(YANG_ROOT "/inp-mod", {AUTH_NORULES}).statusCode == 404);
            REQUIRE(get(YANG_ROOT "/inp-submod", {AUTH_NORULES}).statusCode == 404);
            REQUIRE(get(YANG_ROOT "/root-mod", {AUTH_NORULES}).statusCode == 200);
            REQUIRE(get(YANG_ROOT "/root-submod", {AUTH_NORULES}).statusCode == 200);
        }
    }
}

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

        SECTION("Every node of the leaf-list is deleted")
        {
            // overwrite location leaf-list with our three values
            auto sub = srSess.onOperGet(
                "ietf-yang-library", [](auto, auto, auto, auto, auto, auto, auto& parent) {
                    REQUIRE(!!parent);
                    parent->newPath("location", "hello1");
                    parent->newPath("location", "hello2");
                    parent->newPath("location", "hello3");
                    return sysrepo::ErrorCode::Ok;
                },
                "/ietf-yang-library:yang-library/module-set/module/location");

            // check that direct fetch of data via sysrepo really returns these three location nodes
            static const auto locationLeafsXPath = "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']/location";
            std::map<std::string, std::string> dataFromSysrepo;
            auto client = sysrepo::Connection{}.sessionStart(sysrepo::Datastore::Operational);
            auto data = client.getData(locationLeafsXPath);
            REQUIRE(!!data);
            for (const auto& node : data->findXPath(locationLeafsXPath)) {
                dataFromSysrepo.emplace(node.path(), node.asTerm().valueStr());
            }

            REQUIRE(dataFromSysrepo == std::map<std::string, std::string>({
                        {"/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']/location[1]", "hello1"},
                        {"/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']/location[2]", "hello2"},
                        {"/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']/location[3]", "hello3"},
                    }));

            // but all of this does not affect the restconf data
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
        }
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
                SECTION("revision in uri")
                {
                    REQUIRE(get(YANG_ROOT "/example@2020-02-02", {}) == Response{404, noContentTypeHeaders, ""});
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

                    auto resp = get(YANG_ROOT "/" + moduleName, {});
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
        srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='dwdm']/user-name[.='dwdm']", "");

        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/group[.='dwdm']", "");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/module-name", "ietf-yang-library");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/action", "permit");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/access-operations", "read");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/path", "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/module-name", "ietf-yang-library");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/action", "deny");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/path", "/ietf-yang-library:yang-library/module-set[name='complete']");
        srSess.applyChanges();

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:yang-library/module-set=complete", {AUTH_DWDM, FORWARDED}) == Response{200, jsonHeaders, R"({
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

    SECTION("Location leaf is not added if sysrepo does not report it")
    {
        srSess.switchDatastore(sysrepo::Datastore::Running);
        srSess.setItem("/ietf-netconf-acm:nacm/enable-external-groups", "false");
        srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='dwdm']/user-name[.='dwdm']", "");

        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/group[.='dwdm']", "");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/module-name", "ietf-yang-library");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/action", "deny");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/access-operations", "*");
        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='10']/path", "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']/location");
        srSess.applyChanges();

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:yang-library/module-set=complete/module=ietf-yang-library", {AUTH_DWDM, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:yang-library": {
    "module-set": [
      {
        "name": "complete",
        "module": [
          {
            "name": "ietf-yang-library",
            "revision": "2019-01-04",
            "namespace": "urn:ietf:params:xml:ns:yang:ietf-yang-library"
          }
        ]
      }
    ]
  }
}
)"});
    }

    SECTION("Submodules are reported")
    {
        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:yang-library/module-set=complete/module=root-mod", {AUTH_DWDM, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:yang-library": {
    "module-set": [
      {
        "name": "complete",
        "module": [
          {
            "name": "root-mod",
            "namespace": "rm",
            "location": [
              "http://example.net/yang/root-mod"
            ],
            "submodule": [
              {
                "name": "root-submod",
                "location": [
                  "http://example.net/yang/root-submod"
                ]
              }
            ]
          }
        ]
      }
    ]
  }
}
)"});

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:modules-state/module=root-mod,", {AUTH_DWDM, FORWARDED}) == Response{200, jsonHeaders, R"({
  "ietf-yang-library:modules-state": {
    "module": [
      {
        "name": "root-mod",
        "revision": "",
        "schema": "http://example.net/yang/root-mod",
        "namespace": "rm",
        "conformance-type": "implement",
        "submodule": [
          {
            "name": "root-submod",
            "revision": "",
            "schema": "http://example.net/yang/root-submod"
          }
        ]
      }
    ]
  }
}
)"});
    }
}

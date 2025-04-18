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
            REQUIRE(post(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_ROOT}, "")
                    == Response{405, Response::Headers{ACCESS_CONTROL_ALLOW_ORIGIN, {"allow", "GET, HEAD, OPTIONS"}}, ""});
            REQUIRE(put(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_ROOT}, "")
                    == Response{405, Response::Headers{ACCESS_CONTROL_ALLOW_ORIGIN, {"allow", "GET, HEAD, OPTIONS"}}, ""});
            REQUIRE(patch(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_ROOT}, "")
                    == Response{405, Response::Headers{ACCESS_CONTROL_ALLOW_ORIGIN, {"allow", "GET, HEAD, OPTIONS"}}, ""});
            REQUIRE(httpDelete(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_ROOT})
                    == Response{405, Response::Headers{ACCESS_CONTROL_ALLOW_ORIGIN, {"allow", "GET, HEAD, OPTIONS"}}, ""});
        }

        REQUIRE(options(YANG_ROOT "/ietf-yang-library@2019-01-04", {}) == Response{200, Response::Headers{ACCESS_CONTROL_ALLOW_ORIGIN, {"allow", "GET, HEAD, OPTIONS"}}, ""});

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
                    REQUIRE(head(YANG_ROOT "/ietf-system@abcd-ef-gh", {AUTH_ROOT}) == Response{404, plaintextHeaders, ""});
                }
                SECTION("auth failure")
                {
                    // wrong password
                    REQUIRE(get(YANG_ROOT "/ietf-system@2014-08-06", {AUTH_WRONG_PASSWORD}, boost::posix_time::seconds{5})
                            == Response{401, plaintextHeaders, "Access denied."});
                    REQUIRE(head(YANG_ROOT "/ietf-system@2014-08-06", {AUTH_WRONG_PASSWORD}, boost::posix_time::seconds{5})
                            == Response{401, plaintextHeaders, ""});
                    // anonymous request
                    REQUIRE(head(YANG_ROOT "/ietf-system@2014-08-06", {FORWARDED}, boost::posix_time::seconds{5})
                            == Response{401, plaintextHeaders, ""});
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

                    REQUIRE(head(YANG_ROOT "/" + moduleName, {AUTH_ROOT}) == Response{200, yangHeaders, ""});

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
        srSess.setItem("/ietf-netconf-acm:nacm/groups/group[name='dwdm']/user-name[.='dwdm']", "");

        srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/group[.='dwdm']", "");
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

            SECTION("no other modifications")
            {
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
                auto resp = get(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_DWDM, FORWARDED});
                REQUIRE(resp.equalStatusCodeAndHeaders(Response{200, yangHeaders, ""}));
                REQUIRE(resp.data.substr(0, 26) == "module ietf-yang-library {");
            }

            SECTION("blocked location leaf-list")
            {
                srSess.moveItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='09']", sysrepo::MovePosition::Before, "[name='10']");
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='09']/module-name", "ietf-yang-library");
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='09']/action", "deny");
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='09']/access-operations", "read");
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='09']/path", "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='ietf-yang-library']/location");
                srSess.applyChanges();

                auto resp = get(RESTCONF_DATA_ROOT "/ietf-netconf-acm:nacm", {AUTH_ROOT, FORWARDED});
                CAPTURE(resp.data);

                REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-yang-library:yang-library/module-set=complete", {AUTH_DWDM, FORWARDED}) == Response{200, jsonHeaders, R"({
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
                REQUIRE(head(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_DWDM, FORWARDED}) == Response{404, plaintextHeaders, ""});
                REQUIRE(get(YANG_ROOT "/ietf-yang-library@2019-01-04", {AUTH_DWDM, FORWARDED}) == Response{404, plaintextHeaders, "YANG schema not found"});
            }

            REQUIRE(get(YANG_ROOT "/ietf-system@2014-08-06", {AUTH_DWDM}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/ietf-system@2014-08-06", {AUTH_DWDM, FORWARDED}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/imp-mod", {AUTH_DWDM}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/imp-submod", {AUTH_DWDM}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/root-mod", {AUTH_DWDM}) == Response{404, plaintextHeaders, "YANG schema not found"});
            REQUIRE(get(YANG_ROOT "/root-submod", {AUTH_DWDM}) == Response{404, plaintextHeaders, "YANG schema not found"});
        }

        SECTION("modules, submodules, and imported modules")
        {
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/module-name", "ietf-yang-library");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/action", "permit");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/access-operations", "read");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/module-name", "ietf-yang-library");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/action", "deny");
            srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='99']/path", "/ietf-yang-library:yang-library/module-set[name='complete']");

            int rootMod = 404;
            int rootSubmod = 404;
            int impMod = 404;
            int impSubmod = 404;

            SECTION("root-mod accessible")
            {
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/path",
                               "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='root-mod']");

                // the entire list instance for "root-mod" is available, this means that all of its submodules are accessible as well
                rootMod = rootSubmod = 200;
            }

            SECTION("root-submod accessible only")
            {
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/path",
                               "/ietf-yang-library:yang-library/module-set[name='complete']/module[name='root-mod']/submodule[name='root-submod']");

                rootSubmod = 200;
            }

            SECTION("imported imp-mod accessible")
            {
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/path",
                               "/ietf-yang-library:yang-library/module-set[name='complete']/import-only-module[name='imp-mod'][revision='']");

                // the entire list instance for "imp-mod" is available, this means that all of its submodules are accessible as well
                impMod = impSubmod = 200;
            }

            SECTION("imported imp-submod accessible only")
            {
                srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='rule']/rule[name='11']/path",
                               "/ietf-yang-library:yang-library/module-set[name='complete']/import-only-module[name='imp-mod'][revision='']/submodule[name='imp-submod']");

                impSubmod = 200;
            }

            srSess.applyChanges();

            REQUIRE(get(YANG_ROOT "/root-mod", {AUTH_DWDM}).statusCode == rootMod);
            REQUIRE(get(YANG_ROOT "/root-submod", {AUTH_DWDM}).statusCode == rootSubmod);
            REQUIRE(get(YANG_ROOT "/imp-mod", {AUTH_DWDM}).statusCode == impMod);
            REQUIRE(get(YANG_ROOT "/imp-submod", {AUTH_DWDM}).statusCode == impSubmod);
        }
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

/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10083";
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/pretty_printers.h"

#define _CHANGE(OP, KEY, VAL) \
    {                         \
        OP, KEY, VAL          \
    }
#define CREATED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Created, KEY, VAL)
#define MODIFIED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Modified, KEY, VAL)
#define DELETED(KEY, VAL) _CHANGE(sysrepo::ChangeOperation::Deleted, KEY, VAL)
#define EXPECT_CHANGE(...) REQUIRE_CALL(dsChangesMock, change((std::vector<SrChange>{__VA_ARGS__}))).IN_SEQUENCE(seq1).TIMES(1);

#define CONTENT_TYPE_JSON                            \
    {                                                \
        "content-type", "application/yang-data+json" \
    }
#define CONTENT_TYPE_XML                            \
    {                                               \
        "content-type", "application/yang-data+xml" \
    }

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

struct DatastoreChangesMock {
    MAKE_MOCK1(change, void(const std::vector<SrChange>&));
};

void datastoreChanges(auto session, auto& dsChangesMock, auto path)
{
    std::vector<SrChange> changes;

    for (const auto& change : session.getChanges(path)) {
        std::optional<std::string_view> val;

        if (change.node.isTerm()) {
            val = change.node.asTerm().valueStr();
        }

        changes.emplace_back(change.operation, change.node.path(), val);
    }

    dsChangesMock.change(changes);
}

sysrepo::Subscription datastoreChangesSubscription(auto session, auto& dsChangesMock, const std::string& moduleName)
{
    return session.onModuleChange(
        moduleName,
        [=, &dsChangesMock](auto session, auto, auto, auto, auto, auto) {
            datastoreChanges(session, dsChangesMock, "/" + moduleName + ":*//.");
            return sysrepo::ErrorCode::Ok;
        },
        std::nullopt,
        0,
        sysrepo::SubscribeOptions::DoneOnly);
}

TEST_CASE("writing data")
{
    spdlog::set_level(spdlog::level::trace);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};

    setupRealNacm(srSess);

    trompeloeil::sequence seq1;

    DatastoreChangesMock dsChangesMock;
    auto sub1 = datastoreChangesSubscription(srSess, dsChangesMock, "ietf-system");
    auto sub2 = datastoreChangesSubscription(srSess, dsChangesMock, "example");

    SECTION("PUT")
    {
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

        // PUT on datastore resource (/restconf/data) is not a valid operation
        REQUIRE(put("", "", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{400, jsonHeaders, R"({
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

        // test that overwriting example:c with new content of enabled leaf erases leaf l
        EXPECT_CHANGE(
            MODIFIED("/example:a/b/c/enabled", "true"),
            DELETED("/example:a/b/c/l", "ahoj"));
        REQUIRE(put("/example:a/b/c", R"({"example:c": {"example:enabled": true}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // test overwrite whole container (poor man's delete)
        EXPECT_CHANGE(CREATED("/example:a/b/c/l", "ahoj"));
        REQUIRE(put("/example:a/b/c/l", R"({"example:l": "ahoj"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
        EXPECT_CHANGE(MODIFIED("/example:a/b/c/enabled", "false"));
        REQUIRE(put("/example:a/b/c/enabled", R"({"example:enabled": false}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
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
            CREATED("/example:tlc/list[name='libyang']", std::nullopt),
            CREATED("/example:tlc/list[name='libyang']/name", "libyang"),
            CREATED("/example:tlc/list[name='libyang']/choice1", "libyang"));
        REQUIRE(put("/example:tlc/list=libyang", R"({"example:list":[{"name": "libyang", "choice1": "libyang"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        EXPECT_CHANGE(
            CREATED("/example:tlc/list[name='netconf']", std::nullopt),
            CREATED("/example:tlc/list[name='netconf']/name", "netconf"),
            CREATED("/example:tlc/list[name='netconf']/choice1", "netconf"));
        REQUIRE(put("/example:tlc/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "netconf"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // insert more complicated list entry into a list
        EXPECT_CHANGE(
            CREATED("/example:tlc/list[name='sysrepo']", std::nullopt),
            CREATED("/example:tlc/list[name='sysrepo']/name", "sysrepo"),
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='1'][second='2'][third='3']", std::nullopt),
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='1'][second='2'][third='3']/first", "1"),
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='1'][second='2'][third='3']/second", "2"),
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='1'][second='2'][third='3']/third", "3"),
            CREATED("/example:tlc/list[name='sysrepo']/choice2", "sysrepo"));
        REQUIRE(put("/example:tlc/list=sysrepo", R"({"example:list":[{"name": "sysrepo", "choice2": "sysrepo", "example:nested": [{"first": "1", "second": 2, "third": "3"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // previous test created a nested list in a list. Add new entry there
        EXPECT_CHANGE(
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='11'][second='12'][third='13']", std::nullopt),
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='11'][second='12'][third='13']/first", "11"),
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='11'][second='12'][third='13']/second", "12"),
            CREATED("/example:tlc/list[name='sysrepo']/nested[first='11'][second='12'][third='13']/third", "13"));
        REQUIRE(put("/example:tlc/list=sysrepo/nested=11,12,13", R"({"example:nested": [{"first": "11", "second": 12, "third": "13"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // modify a leaf in a list
        EXPECT_CHANGE(MODIFIED("/example:tlc/list[name='netconf']/choice1", "restconf"));
        REQUIRE(put("/example:tlc/list=netconf/choice1", R"({"example:choice1": "restconf"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // add values to leaf-lists
        EXPECT_CHANGE(CREATED("/example:top-level-leaf-list[.='4']", "4"));
        REQUIRE(put("/example:top-level-leaf-list=4", R"({"example:top-level-leaf-list":[4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
        EXPECT_CHANGE(CREATED("/example:top-level-leaf-list[.='1']", "1"));
        REQUIRE(put("/example:top-level-leaf-list=1", R"({"example:top-level-leaf-list":[1]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
        EXPECT_CHANGE(CREATED("/example:tlc/list[name='netconf']/collection[.='4']", "4"));
        REQUIRE(put("/example:tlc/list=netconf/collection=4", R"({"example:collection": [4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

        // overwrite list entry
        EXPECT_CHANGE(
            DELETED("/example:tlc/list[name='netconf']/collection[.='4']", "4"),
            CREATED("/example:tlc/list[name='netconf']/collection[.='1']", "1"),
            CREATED("/example:tlc/list[name='netconf']/collection[.='2']", "2"),
            CREATED("/example:tlc/list[name='netconf']/collection[.='3']", "3"),
            MODIFIED("/example:tlc/list[name='netconf']/choice1", "snmp"));
        REQUIRE(put("/example:tlc/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "snmp", "collection": [1,2,3]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

        // send wrong keys
        REQUIRE(put("/example:tlc/list=netconf", R"({"example:list":[{"name": "ahoj", "choice1": "nope"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:tlc/list[name='ahoj']/name",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});
        REQUIRE(put("/example:top-level-list=netconf", R"({"example:top-level-list":[{"name": "ahoj"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
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
        REQUIRE(put("/example:tlc/list=netconf/collection=667", R"({"example:collection":[666]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-path": "/example:tlc/list[name='netconf']/collection[.='666']",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});
        REQUIRE(put("/example:top-level-leaf-list=667", R"({"example:top-level-leaf-list":[666]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
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

        REQUIRE(get("/example:tlc", {{"x-remote-user", "yangnobody"}, CONTENT_TYPE_JSON}) == Response{200, jsonHeaders, R"({
  "example:tlc": {
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

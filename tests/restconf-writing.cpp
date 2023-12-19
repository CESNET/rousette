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
    std::optional<std::string> currentValue;
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
        printer<std::optional<std::string>>::print(os, o.currentValue);
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
        std::optional<std::string> val;

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
        [moduleName, &dsChangesMock](auto session, auto, auto, auto, auto, auto) {
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

    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    DatastoreChangesMock dsChangesMock;

    SECTION("PUT")
    {
        auto changesIetfSystem = datastoreChangesSubscription(srSess, dsChangesMock, "ietf-system");
        auto changesExample = datastoreChangesSubscription(srSess, dsChangesMock, "example");

        SECTION("anonymous writes disabled by NACM")
        {
            REQUIRE(put(RESTCONF_DATA_ROOT "/ietf-system:system", R"({"ietf-system:system":{"ietf-system:location":"prague"}}")", {CONTENT_TYPE_JSON}) == Response{403, jsonHeaders, R"({
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
        }

        SECTION("PUT request with valid URI but invalid path in data")
        {
            // nonsense node is not in the YANG module so libyang fails here
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:nonsense": "other-str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: Can't parse data: LY_EVALID"
      }
    ]
  }
}
)"});

            // libyang parses correctly, example:a is valid but we reject because of the node mismatch
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:a": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:a",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});
        }

        SECTION("Create and modify a leaf")
        {
            SECTION("Top-level leaf")
            {
                EXPECT_CHANGE(CREATED("/example:top-level-leaf", "str"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(MODIFIED("/example:top-level-leaf", "other-str"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "other-str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Leaf in a container")
            {
                EXPECT_CHANGE(CREATED("/example:two-leafs/a", "a-value"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs/a", R"({"example:a": "a-value"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(MODIFIED("/example:two-leafs/a", "another-a-value"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs/a", R"({"example:a": "another-a-value"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Repeated insertion")
            {
                EXPECT_CHANGE(CREATED("/example:top-level-leaf", "str"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }
        }

        SECTION("Container operations")
        {
            // create a container entry with two leafs
            EXPECT_CHANGE(
                CREATED("/example:two-leafs/a", "a-val"),
                CREATED("/example:two-leafs/b", "b-val"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs", R"({"example:two-leafs": {"a": "a-val", "b": "b-val"}})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

            SECTION("Overwrite container with only one child, the second gets deleted")
            {
                EXPECT_CHANGE(
                    DELETED("/example:two-leafs/a", "a-val"),
                    MODIFIED("/example:two-leafs/b", "new-b-val"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs", R"({"example:two-leafs": {"b": "new-b-val"}})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Modify one leaf")
            {
                EXPECT_CHANGE(MODIFIED("/example:two-leafs/b", "new-b-val"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs/b", R"({"example:b": "new-b-val"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Set container to empty container (delete)")
            {
                EXPECT_CHANGE(
                    DELETED("/example:two-leafs/a", "a-val"),
                    DELETED("/example:two-leafs/b", "b-val"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs", R"({"example:two-leafs": {}})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }
        }

        SECTION("content-type")
        {
            EXPECT_CHANGE(CREATED("/example:a/b/c/blower", "libyang is love"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b", R"(<b xmlns="http://example.tld/example"><c><blower>libyang is love</blower></c></b>)", {AUTH_ROOT, CONTENT_TYPE_XML}) == Response{204, xmlHeaders, ""});

            // content-type header is mandatory for PUT
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/example-augment:b", R"({"example-augment:b": { "c" : {"enabled" : false}}}")", {AUTH_ROOT}) == Response{400, jsonHeaders, R"({
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

            // mismatch between content-type and actual data format
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b", R"({"example:b": {"example:c": {"l": "ahoj"}}}")", {AUTH_ROOT, CONTENT_TYPE_XML}) == Response{400, xmlHeaders, R"(<errors xmlns="urn:ietf:params:xml:ns:yang:ietf-restconf">
  <error>
    <error-type>protocol</error-type>
    <error-tag>invalid-value</error-tag>
    <error-message>Validation failure: DataNode::parseSubtree: lyd_parse_data failed: LY_EVALID</error-message>
  </error>
</errors>
)"});
        }

        SECTION("Invalid requests")
        {
            // Invalid path, this throws in the uri parser
            // FIXME: add error-path reporting for wrong URIs according to https://datatracker.ietf.org/doc/html/rfc8040#page-78
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:nonsense", R"({"example:nonsense": "other-str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
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

            // boolean literal in quotes
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a", R"({"example:a":{"b":{"c":{"enabled":"false"}}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: Can't parse data: LY_EVALID"
      }
    ]
  }
}
)"});

            // wrong path: enabled leaf is not located under node b and libyang-cpp throws
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b/c", R"({"example:enabled":false}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: DataNode::parseSubtree: lyd_parse_data failed: LY_EVALID"
      }
    ]
  }
}
)"});

            // wrong path: leaf l is located under node c (it is sibling of enabled leaf) but we check that URI path corresponds to the leaf we parse
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b/c/enabled", R"({"example:blower":"hey"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:a/b/c/blower",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});

            // put the correct root element but also its sibling
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b/c/enabled", R"({"example:enabled":false, "example:blower": "nope"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:a/b/c/blower",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});

            // the root node in data is different from the one in URI
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:top-level-leaf",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});

            // the root node in data is different from the one in URI
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-list=aaa", R"({"example:top-level-leaf": "a"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:top-level-leaf",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});
        }

        SECTION("Default values handling")
        {
            // no change here: enabled leaf has default value true
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a", R"({"example:a":{"b":{"c":{"enabled":true}}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

            EXPECT_CHANGE(MODIFIED("/example:a/b/c/enabled", "false"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b/c", R"({"example:c":{"enabled":false}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

            EXPECT_CHANGE(MODIFIED("/example:a/b/c/enabled", "true"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b", R"({"example:b": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
        }

        SECTION("Children with same name but different namespaces")
        {
            // there are two childs named 'b' under /example:a but both inside different namespaces (/example:a/b and /example:a/example-augment:b)
            // I am also providing a namespace with enabled leaf - this should work as well although not needed
            EXPECT_CHANGE(MODIFIED("/example:a/example-augment:b/c/enabled", "false"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/example-augment:b", R"({"example-augment:b": {"c":{"example-augment:enabled":false}}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

            // the namespaces differ between URI and data
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/example-augment:b", R"({"example:b": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:a/b",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});
        }

        SECTION("List operations")
        {
            // two inserts so we have something to operate on
            EXPECT_CHANGE(
                CREATED("/example:top-level-list[name='sysrepo']", std::nullopt),
                CREATED("/example:top-level-list[name='sysrepo']/name", "sysrepo"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-list=sysrepo", R"({"example:top-level-list":[{"name": "sysrepo"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

            EXPECT_CHANGE(
                CREATED("/example:tlc/list[name='libyang']", std::nullopt),
                CREATED("/example:tlc/list[name='libyang']/name", "libyang"),
                CREATED("/example:tlc/list[name='libyang']/choice1", "libyang"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang", R"({"example:list":[{"name": "libyang", "choice1": "libyang"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

            SECTION("New insert does not modify other list entries")
            {
                EXPECT_CHANGE(
                    CREATED("/example:tlc/list[name='netconf']", std::nullopt),
                    CREATED("/example:tlc/list[name='netconf']/name", "netconf"),
                    CREATED("/example:tlc/list[name='netconf']/choice1", "netconf"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "netconf"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Insert a larger portion of data")
            {
                EXPECT_CHANGE(
                    CREATED("/example:tlc/list[name='large']", std::nullopt),
                    CREATED("/example:tlc/list[name='large']/name", "large"),
                    CREATED("/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']", std::nullopt),
                    CREATED("/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']/first", "1"),
                    CREATED("/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']/second", "2"),
                    CREATED("/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']/third", "3"),
                    CREATED("/example:tlc/list[name='large']/choice2", "large"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=large", R"({"example:list":[{"name": "large", "choice2": "large", "example:nested": [{"first": "1", "second": 2, "third": "3"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Insert into the list having multiple keys")
            {
                EXPECT_CHANGE(
                    CREATED("/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']", std::nullopt),
                    CREATED("/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']/first", "11"),
                    CREATED("/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']/second", "12"),
                    CREATED("/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']/third", "13"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/nested=11,12,13", R"({"example:nested": [{"first": "11", "second": 12, "third": "13"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Modify a leaf in a list entry")
            {
                EXPECT_CHANGE(MODIFIED("/example:tlc/list[name='libyang']/choice1", "restconf"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/choice1", R"({"example:choice1": "restconf"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Overwrite a list entry")
            {
                // insert something in the leaf-list first so we can test that the leaf-list collection was overwritten later
                EXPECT_CHANGE(CREATED("/example:tlc/list[name='libyang']/collection[.='4']", "4"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/collection=4", R"({"example:collection": [4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(
                    DELETED("/example:tlc/list[name='libyang']/collection[.='4']", "4"),
                    CREATED("/example:tlc/list[name='libyang']/collection[.='1']", "1"),
                    CREATED("/example:tlc/list[name='libyang']/collection[.='2']", "2"),
                    CREATED("/example:tlc/list[name='libyang']/collection[.='3']", "3"),
                    MODIFIED("/example:tlc/list[name='libyang']/choice1", "idk"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang", R"({"example:list":[{"name": "libyang", "choice1": "idk", "collection": [1,2,3]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Insert into leaf-lists")
            {
                EXPECT_CHANGE(CREATED("/example:top-level-leaf-list[.='4']", "4"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=4", R"({"example:top-level-leaf-list":[4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(CREATED("/example:top-level-leaf-list[.='1']", "1"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=1", R"({"example:top-level-leaf-list":[1]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(CREATED("/example:tlc/list[name='libyang']/collection[.='4']", "4"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/collection=4", R"({"example:collection": [4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Send wrong keys")
            {
                // wrong key value
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf", R"({"example:list":[{"name": "ahoj", "choice1": "nope"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:tlc/list[name='ahoj']/name",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});

                // wrong key value for top level list; this request goes through another branch in the PUT code so let's test this as well
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-list=netconf", R"({"example:top-level-list":[{"name": "ahoj"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:top-level-list[name='ahoj']/name",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});


                // key leaf missing
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf", R"({"example:list":[{"choice1": "nope"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: DataNode::parseSubtree: lyd_parse_data failed: LY_EVALID"
      }
    ]
  }
}
)"});

                // list node missing
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf", R"({"example:list":[]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Invalid data for PUT (node indicated by URI is missing)."
      }
    ]
  }
}
)"});

                // list node is missing; this request goes through another branch in the PUT code so let's test this as well
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-list=ahoj", R"({"example:top-level-list":[]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Invalid data for PUT (node indicated by URI is missing)."
      }
    ]
  }
}
)"});

                // wrong key value for a leaf-list
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf/collection=667", R"({"example:collection":[666]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:tlc/list[name='netconf']/collection[.='666']",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});

                // wrong key value for a leaf-list
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=667", R"({"example:top-level-leaf-list":[666]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:top-level-leaf-list[.='666']",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});

                // multiple list entries in one request; the key specified in the URI is in the first list entry
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "nope"}, {"name": "sysrepo", "choice1": "bla"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:tlc/list[name='sysrepo']/name",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});

                // multiple list entries in one request; the key specified in the URI is in the second list entry
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf", R"({"example:list":[{"name": "sysrepo", "choice1": "bla"}, {"name": "netconf", "choice1": "nope"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:tlc/list[name='sysrepo']/name",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});

                // multiple values for a leaf-list insertion
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/collection=5", R"({"example:collection": [5, 42]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:tlc/list[name='libyang']/collection[.='42']",
        "error-message": "Invalid data for PUT (list key mismatch between URI path and data)."
      }
    ]
  }
}
)"});
            }
        }

        SECTION("Complete-datastore")
        {
            EXPECT_CHANGE(
                CREATED("/example:top-level-leaf", "str"),
                CREATED("/example:tlc/list[name='libyang']", std::nullopt),
                CREATED("/example:tlc/list[name='libyang']/name", "libyang"),
                CREATED("/example:tlc/list[name='libyang']/choice1", "libyang"));
            REQUIRE(put(RESTCONF_DATA_ROOT, R"({"example:top-level-leaf": "str", "example:tlc": {"list": [{"name": "libyang", "choice1": "libyang"}]}})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{201, jsonHeaders, ""});

            EXPECT_CHANGE(
                MODIFIED("/example:top-level-leaf", "other-str"),
                DELETED("/example:tlc/list[name='libyang']", std::nullopt),
                DELETED("/example:tlc/list[name='libyang']/name", "libyang"),
                DELETED("/example:tlc/list[name='libyang']/choice1", "libyang"),
                CREATED("/example:tlc/list[name='sysrepo']", std::nullopt),
                CREATED("/example:tlc/list[name='sysrepo']/name", "sysrepo"),
                CREATED("/example:tlc/list[name='sysrepo']/choice1", "sysrepo"));
            REQUIRE(put(RESTCONF_DATA_ROOT, R"({"example:top-level-leaf": "other-str", "example:tlc": {"list": [{"name": "sysrepo", "choice1": "sysrepo"}]}})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{201, jsonHeaders, ""});
        }

        DOCTEST_SUBCASE("RPCs")
        {
            REQUIRE(put(RESTCONF_DATA_ROOT "/ietf-system:system-restart", "", {AUTH_DWDM}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-not-supported",
        "error-message": "'/ietf-system:system-restart' is an RPC/Action node"
      }
    ]
  }
}
)"});

            REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=eth0/example-action", "", {AUTH_DWDM}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-not-supported",
        "error-message": "'/example:tlc/list/example-action' is an RPC/Action node"
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
        "error-message": "'/example:tlc/list/example-action' is an RPC/Action node, any child of it can't be requested"
      }
    ]
  }
}
)"});

            REQUIRE(get(RESTCONF_DATA_ROOT "/example:tlc/list=eth0/example-action/o", {AUTH_DWDM}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "'/example:tlc/list/example-action' is an RPC/Action node, any child of it can't be requested"
      }
    ]
  }
}
)"});
        }
    }

    SECTION("PUT with NMDA")
    {
        SECTION("Writable datastores")
        {
            sysrepo::Datastore ds = sysrepo::Datastore::Running;
            std::string uri;

            SECTION("Complete datastore")
            {
                SECTION("startup")
                {
                    ds = sysrepo::Datastore::Startup;
                    uri = RESTCONF_ROOT_DS("startup");
                }

                SECTION("candidate")
                {
                    ds = sysrepo::Datastore::Candidate;
                    uri = RESTCONF_ROOT_DS("candidate");
                }

                SECTION("running")
                {
                    ds = sysrepo::Datastore::Running;
                    uri = RESTCONF_ROOT_DS("running");
                }

                auto sess = srConn.sessionStart(ds);

                auto sub = datastoreChangesSubscription(sess, dsChangesMock, "example");

                EXPECT_CHANGE(
                    CREATED("/example:top-level-leaf", "str"),
                    CREATED("/example:tlc/list[name='libyang']", std::nullopt),
                    CREATED("/example:tlc/list[name='libyang']/name", "libyang"),
                    CREATED("/example:tlc/list[name='libyang']/choice1", "libyang"));
                REQUIRE(put(uri, R"({"example:top-level-leaf": "str", "example:tlc": {"list": [{"name": "libyang", "choice1": "libyang"}]}})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(
                    MODIFIED("/example:top-level-leaf", "other-str"),
                    DELETED("/example:tlc/list[name='libyang']", std::nullopt),
                    DELETED("/example:tlc/list[name='libyang']/name", "libyang"),
                    DELETED("/example:tlc/list[name='libyang']/choice1", "libyang"),
                    CREATED("/example:tlc/list[name='sysrepo']", std::nullopt),
                    CREATED("/example:tlc/list[name='sysrepo']/name", "sysrepo"),
                    CREATED("/example:tlc/list[name='sysrepo']/choice1", "sysrepo"));
                REQUIRE(put(uri, R"({"example:top-level-leaf": "other-str", "example:tlc": {"list": [{"name": "sysrepo", "choice1": "sysrepo"}]}})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Inner resources")
            {
                SECTION("startup")
                {
                    ds = sysrepo::Datastore::Startup;
                    uri = RESTCONF_ROOT_DS("startup");
                }

                SECTION("candidate")
                {
                    ds = sysrepo::Datastore::Candidate;
                    uri = RESTCONF_ROOT_DS("candidate");
                }

                SECTION("running")
                {
                    ds = sysrepo::Datastore::Running;
                    uri = RESTCONF_ROOT_DS("running");
                }

                auto sess = srConn.sessionStart(ds);
                auto sub = datastoreChangesSubscription(sess, dsChangesMock, "example");

                EXPECT_CHANGE(CREATED("/example:two-leafs/a", "hello"));
                REQUIRE(put(uri + "/example:two-leafs/a", R"({"example:a":"hello"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(MODIFIED("/example:two-leafs/a", "hello world"));
                REQUIRE(put(uri + "/example:two-leafs/a", R"({"example:a":"hello world"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }
        }

        SECTION("Read-only datastores")
        {
            std::string uri;
            SECTION("operational")
            {
                uri = RESTCONF_ROOT_DS("operational");
            }

            SECTION("factory-default")
            {
                uri = RESTCONF_ROOT_DS("factory-default");
            }

            REQUIRE(put(uri + "/example:top-level-leaf", R"({"example:top-level-leaf": "str"})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "Read-only datastore."
      }
    ]
  }
}
)"});
        }
    }

    SECTION("POST")
    {
            REQUIRE(post(RESTCONF_DATA_ROOT, R"({"example:top-level-leaf": "str"})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "POST method for a complete-datastore resource is not yet implemented"
      }
    ]
  }
}
)"});

            REQUIRE(post(RESTCONF_ROOT_DS("running"), R"({"example:top-level-leaf": "str"})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "POST method for a complete-datastore resource is not yet implemented"
      }
    ]
  }
}
)"});

            REQUIRE(post(RESTCONF_DATA_ROOT "/example:two-leafs", R"({"example:a": "a-value"}")", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "POST method for a data resource is not yet implemented"
      }
    ]
  }
}
)"});
    }
}

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

#define _CHANGE(DS, OP, KEY, VAL) \
    {                             \
        DS, OP, KEY, VAL          \
    }
#define CREATED(DS, KEY, VAL) _CHANGE(DS, sysrepo::ChangeOperation::Created, KEY, VAL)
#define MODIFIED(DS, KEY, VAL) _CHANGE(DS, sysrepo::ChangeOperation::Modified, KEY, VAL)
#define DELETED(DS, KEY, VAL) _CHANGE(DS, sysrepo::ChangeOperation::Deleted, KEY, VAL)
#define EXPECT_CHANGE(...) REQUIRE_CALL(dsChangesMock, change((std::vector<SrChange>{__VA_ARGS__}))).IN_SEQUENCE(seq1).TIMES(1);

#define DS_STARTUP sysrepo::Datastore::Startup
#define DS_RUNNING sysrepo::Datastore::Running

#define CONTENT_TYPE_JSON                            \
    {                                                \
        "content-type", "application/yang-data+json" \
    }
#define CONTENT_TYPE_XML                            \
    {                                               \
        "content-type", "application/yang-data+xml" \
    }

struct SrChange {
    sysrepo::Datastore datastore;
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

        changes.emplace_back(session.activeDatastore(), change.operation, change.node.path(), val);
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

    srSess.copyConfig(sysrepo::Datastore::Startup, "ietf-system");
    srSess.copyConfig(sysrepo::Datastore::Startup, "example");

    DatastoreChangesMock dsChangesMock;
    std::vector<sysrepo::Subscription> subs;
    subs.emplace_back(datastoreChangesSubscription(srSess, dsChangesMock, "ietf-system"));
    subs.emplace_back(datastoreChangesSubscription(srSess, dsChangesMock, "example"));

    srSess.switchDatastore(sysrepo::Datastore::Startup);
    subs.emplace_back(datastoreChangesSubscription(srSess, dsChangesMock, "ietf-system"));
    subs.emplace_back(datastoreChangesSubscription(srSess, dsChangesMock, "example"));

    SECTION("PUT")
    {
        SECTION("Test anonymous writes disabled by NACM")
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
                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:top-level-leaf", "str"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(MODIFIED(DS_RUNNING, "/example:top-level-leaf", "other-str"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "other-str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Leaf in a container")
            {
                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:two-leafs/a", "a-value"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs/a", R"({"example:a": "a-value"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(MODIFIED(DS_RUNNING, "/example:two-leafs/a", "another-a-value"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs/a", R"({"example:a": "another-a-value"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Repeated insertion")
            {
                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:top-level-leaf", "str"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Datastores")
            {
                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:top-level-leaf", "str"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(CREATED(DS_STARTUP, "/example:top-level-leaf", "str"));
                REQUIRE(put(RESTCONF_ROOT_DS("startup") "/example:top-level-leaf", R"({"example:top-level-leaf": "str"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }
        }

        SECTION("Container operations")
        {
            // create a container entry with two leafs
            EXPECT_CHANGE(
                CREATED(DS_RUNNING, "/example:two-leafs/a", "a-val"),
                CREATED(DS_RUNNING, "/example:two-leafs/b", "b-val"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs", R"({"example:two-leafs": {"a": "a-val", "b": "b-val"}})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

            SECTION("Overwrite container with only one child, the second gets deleted")
            {
                EXPECT_CHANGE(
                    DELETED(DS_RUNNING, "/example:two-leafs/a", "a-val"),
                    MODIFIED(DS_RUNNING, "/example:two-leafs/b", "new-b-val"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs", R"({"example:two-leafs": {"b": "new-b-val"}})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Modify one leaf")
            {
                EXPECT_CHANGE(MODIFIED(DS_RUNNING, "/example:two-leafs/b", "new-b-val"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs/b", R"({"example:b": "new-b-val"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Set container to empty container (delete)")
            {
                EXPECT_CHANGE(
                    DELETED(DS_RUNNING, "/example:two-leafs/a", "a-val"),
                    DELETED(DS_RUNNING, "/example:two-leafs/b", "b-val"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:two-leafs", R"({"example:two-leafs": {}})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }
        }

        SECTION("Test content-type")
        {
            EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:a/b/c/l", "libyang is love"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b", R"(<b xmlns="http://example.tld/example"><c><l>libyang is love</l></c></b>)", {AUTH_ROOT, CONTENT_TYPE_XML}) == Response{204, xmlHeaders, ""});

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
            // PUT on datastore resource (/restconf/data, /restconf/ds/ietf-datastores:*) is not a valid operation
            REQUIRE(put(RESTCONF_DATA_ROOT, "", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{400, jsonHeaders, R"({
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

            REQUIRE(put(RESTCONF_ROOT_DS("operational"), "", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{400, jsonHeaders, R"({
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

            // factory-default is read-only DS
            REQUIRE(put(RESTCONF_ROOT_DS("factory-default") "/example:top-level-leaf", R"({"example:top-level-leaf": "str"})", {CONTENT_TYPE_JSON, AUTH_ROOT}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-not-supported",
        "error-message": "Read only datastore"
      }
    ]
  }
}
)"});

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
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b/c/enabled", R"({"example:l":"hey"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:a/b/c/l",
        "error-message": "Invalid data for PUT (data contains invalid node)."
      }
    ]
  }
}
)"});

            // put the correct root element but also its sibling
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b/c/enabled", R"({"example:enabled":false, "example:l": "nope"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:a/b/c/l",
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

            EXPECT_CHANGE(MODIFIED(DS_RUNNING, "/example:a/b/c/enabled", "false"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b/c", R"({"example:c":{"enabled":false}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});

            EXPECT_CHANGE(MODIFIED(DS_RUNNING, "/example:a/b/c/enabled", "true"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:a/b", R"({"example:b": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
        }

        SECTION("Children with same name but different namespaces")
        {
            // there are two childs named 'b' under /example:a but both inside different namespaces (/example:a/b and /example:a/example-augment:b)
            // I am also providing a namespace with enabled leaf - this should work as well although not needed
            EXPECT_CHANGE(MODIFIED(DS_RUNNING, "/example:a/example-augment:b/c/enabled", "false"));
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
                CREATED(DS_RUNNING, "/example:top-level-list[name='sysrepo']", std::nullopt),
                CREATED(DS_RUNNING, "/example:top-level-list[name='sysrepo']/name", "sysrepo"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-list=sysrepo", R"({"example:top-level-list":[{"name": "sysrepo"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

            EXPECT_CHANGE(
                CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']", std::nullopt),
                CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/name", "libyang"),
                CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/choice1", "libyang"));
            REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang", R"({"example:list":[{"name": "libyang", "choice1": "libyang"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

            SECTION("New insert does not modify other list entries")
            {
                EXPECT_CHANGE(
                    CREATED(DS_RUNNING, "/example:tlc/list[name='netconf']", std::nullopt),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='netconf']/name", "netconf"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='netconf']/choice1", "netconf"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=netconf", R"({"example:list":[{"name": "netconf", "choice1": "netconf"}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Insert a larger portion of data")
            {
                EXPECT_CHANGE(
                    CREATED(DS_RUNNING, "/example:tlc/list[name='large']", std::nullopt),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='large']/name", "large"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']", std::nullopt),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']/first", "1"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']/second", "2"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='large']/nested[first='1'][second='2'][third='3']/third", "3"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='large']/choice2", "large"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=large", R"({"example:list":[{"name": "large", "choice2": "large", "example:nested": [{"first": "1", "second": 2, "third": "3"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Insert into the list having multiple keys")
            {
                EXPECT_CHANGE(
                    CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']", std::nullopt),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']/first", "11"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']/second", "12"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/nested[first='11'][second='12'][third='13']/third", "13"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/nested=11,12,13", R"({"example:nested": [{"first": "11", "second": 12, "third": "13"}]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});
            }

            SECTION("Modify a leaf in a list entry")
            {
                EXPECT_CHANGE(MODIFIED(DS_RUNNING, "/example:tlc/list[name='libyang']/choice1", "restconf"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/choice1", R"({"example:choice1": "restconf"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Overwrite a list entry")
            {
                // insert something in the leaf-list first so we can test that the leaf-list collection was overwritten later
                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/collection[.='4']", "4"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/collection=4", R"({"example:collection": [4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(
                    DELETED(DS_RUNNING, "/example:tlc/list[name='libyang']/collection[.='4']", "4"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/collection[.='1']", "1"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/collection[.='2']", "2"),
                    CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/collection[.='3']", "3"),
                    MODIFIED(DS_RUNNING, "/example:tlc/list[name='libyang']/choice1", "idk"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:tlc/list=libyang", R"({"example:list":[{"name": "libyang", "choice1": "idk", "collection": [1,2,3]}]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, jsonHeaders, ""});
            }

            SECTION("Insert into leaf-lists")
            {
                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:top-level-leaf-list[.='4']", "4"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=4", R"({"example:top-level-leaf-list":[4]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:top-level-leaf-list[.='1']", "1"));
                REQUIRE(put(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=1", R"({"example:top-level-leaf-list":[1]})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

                EXPECT_CHANGE(CREATED(DS_RUNNING, "/example:tlc/list[name='libyang']/collection[.='4']", "4"));
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

                // multiple values for a list insertion
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
    }
}

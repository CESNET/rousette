/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10086";
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
#define EXPECT_CHANGE(...) REQUIRE_CALL(dsChangesMock, change((std::vector<SrChange>{__VA_ARGS__}))).IN_SEQUENCE(seq1).TIMES(1)

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
    MAKE_MOCK1(contentAfterChange, void(const std::optional<std::string>&));
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

void datastoreNewState(auto session, auto& dsChangesMock, auto path)
{
    dsChangesMock.contentAfterChange(session.getData(path)->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings));
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

sysrepo::Subscription datastoreNewStateSubscription(auto session, auto& dsChangesMock, const std::string& moduleName)
{
    return session.onModuleChange(
        moduleName,
        [moduleName, &dsChangesMock](auto session, auto, auto, auto, auto, auto) {
            datastoreNewState(session, dsChangesMock, "/" + moduleName + ":*");
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

    trompeloeil::sequence seq1;

    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    setupRealNacm(srSess);

    DatastoreChangesMock dsChangesMock;

    // setup some data we can delete it
    srSess.setItem("/example:top-level-leaf", "str");
    srSess.setItem("/example:top-level-list[name='key1']", std::nullopt);
    srSess.setItem("/example:top-level-list[name='key2']", std::nullopt);
    srSess.setItem("/example:top-level-leaf-list[.='1']", std::nullopt);
    srSess.setItem("/example:top-level-leaf-list[.='2']", std::nullopt);
    srSess.setItem("/example:two-leafs/a", "a");
    srSess.setItem("/example:two-leafs/b", "b");
    srSess.setItem("/example:a/b/c/enabled", "true");
    srSess.setItem("/example:a/b/c/blower", "str");
    srSess.applyChanges();
    auto changesExampleRunning = datastoreChangesSubscription(srSess, dsChangesMock, "example");

    srSess.switchDatastore(sysrepo::Datastore::Startup);
    srSess.setItem("/example:two-leafs/a", "startup_a");
    srSess.setItem("/example:two-leafs/b", "startup_b");
    srSess.applyChanges();
    auto changesExampleStartup = datastoreChangesSubscription(srSess, dsChangesMock, "example");

    SECTION("anonymous deletes disabled by NACM")
    {
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-leaf", {}) == Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/example:top-level-leaf",
        "error-message": "Access denied."
      }
    ]
  }
}
)"});
    }

    SECTION("deleting not present non-mandatory nodes")
    {
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:tlc/status", {AUTH_ROOT}) == Response{404, noContentTypeHeaders, ""});
    }

    SECTION("leafs")
    {
        EXPECT_CHANGE(DELETED("/example:top-level-leaf", "str"));
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-leaf", {AUTH_ROOT}) == Response{204, noContentTypeHeaders, ""});

        EXPECT_CHANGE(DELETED("/example:two-leafs/a", "a"));
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:two-leafs/a", {AUTH_ROOT}) == Response{204, noContentTypeHeaders, ""});
    }

    SECTION("container")
    {
        EXPECT_CHANGE(
                DELETED("/example:two-leafs/a", "a"),
                DELETED("/example:two-leafs/b", "b"));
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:two-leafs", {AUTH_ROOT}) == Response{204, noContentTypeHeaders, ""});
    }

    SECTION("lists")
    {
        EXPECT_CHANGE(
            DELETED("/example:top-level-list[name='key1']", std::nullopt),
            DELETED("/example:top-level-list[name='key1']/name", "key1"));
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-list=key1", {AUTH_ROOT}) == Response{204, noContentTypeHeaders, ""});

        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-list=ThisKeyDoesNotExist", {AUTH_ROOT}) == Response{404, noContentTypeHeaders, ""});

        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-list", {AUTH_ROOT}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "List '/example:top-level-list' requires 1 keys"
      }
    ]
  }
}
)"});
    }

    SECTION("leaf-lists")
    {
        EXPECT_CHANGE(DELETED("/example:top-level-leaf-list[.='2']", "2"));
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=2", {AUTH_ROOT}) == Response{204, noContentTypeHeaders, ""});

        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=666", {AUTH_ROOT}) == Response{404, noContentTypeHeaders, ""});

        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-leaf-list", {AUTH_ROOT}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Leaf-list '/example:top-level-leaf-list' requires exactly one key"
      }
    ]
  }
}
)"});
    }

    SECTION("NMDA")
    {
        EXPECT_CHANGE(
                DELETED("/example:two-leafs/a", "startup_a"),
                DELETED("/example:two-leafs/b", "startup_b"));
        REQUIRE(httpDelete(RESTCONF_ROOT_DS("startup") "/example:two-leafs", {AUTH_ROOT}) == Response{204, noContentTypeHeaders, ""});
    }

    SECTION("RPC nodes")
    {
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:test-rpc", {AUTH_ROOT}) == Response{405, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-not-supported",
        "error-message": "'/example:test-rpc' is an RPC/Action node"
      }
    ]
  }
}
)"});

        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:test-rpc/input/i", {AUTH_ROOT}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "'/example:test-rpc' is an RPC/Action node, any child of it can't be requested"
      }
    ]
  }
}
)"});
    }
}

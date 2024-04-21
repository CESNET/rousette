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
#include "tests/datastoreUtils.h"
#include "tests/pretty_printers.h"

TEST_CASE("deleting data")
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
    srSess.setItem("/example-delete:secret[name='existing-key']", std::nullopt);
    srSess.setItem("/example-delete:immutable[name='existing-key']", std::nullopt);
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
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:tlc/status", {AUTH_ROOT}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "data-missing",
        "error-path": "/example:tlc/status",
        "error-message": "Data is missing."
      }
    ]
  }
}
)"});
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

        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-list=ThisKeyDoesNotExist", {AUTH_ROOT}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "data-missing",
        "error-path": "/example:top-level-list[name='ThisKeyDoesNotExist']",
        "error-message": "Data is missing."
      }
    ]
  }
}
)"});

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

        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:top-level-leaf-list=666", {AUTH_ROOT}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "data-missing",
        "error-path": "/example:top-level-leaf-list[.='666']",
        "error-message": "Data is missing."
      }
    ]
  }
}
)"});

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

    SECTION("NACM 403 vs 404")
    {
        // User only has read permission, 403 makes sense
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example-delete:immutable=existing-key", {}) == Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/example-delete:immutable[name='existing-key']",
        "error-message": "Access denied."
      }
    ]
  }
}
)"});

        // User has read permission but the node is not present, 404 makes sense
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example-delete:immutable=non-existing-key", {}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "data-missing",
        "error-path": "/example-delete:immutable[name='non-existing-key']",
        "error-message": "Data is missing."
      }
    ]
  }
}
)"});

        // User does not know that this node actually exist
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example-delete:secret=existing-key", {}) == Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/example-delete:secret[name='existing-key']",
        "error-message": "Access denied."
      }
    ]
  }
}
)"});

        // FIXME: User does not know that this node actually does not exist but sysrepo does report data is missing error here. See https://github.com/sysrepo/sysrepo/issues/3283
        REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example-delete:secret=non-existing-key", {}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "data-missing",
        "error-path": "/example-delete:secret[name='non-existing-key']",
        "error-message": "Data is missing."
      }
    ]
  }
}
)"});
    }
}

/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10090";
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/datastoreUtils.h"
#include "tests/pretty_printers.h"

TEST_CASE("YANG patch")
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
    auto changesExample = datastoreChangesSubscription(srSess, dsChangesMock, "example");

    EXPECT_CHANGE(CREATED("/example:top-level-leaf", "sorry"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "comment" : "This thing can have comments, right?",
    "edit" : [
      {
        "edit-id" : "edit",
        "operation" : "create",
        "target" : "/example:top-level-leaf",
        "value" : {
          "example:top-level-leaf" : "sorry"
        }
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});

    EXPECT_CHANGE(MODIFIED("/example:top-level-leaf", "sorry not sorry"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit",
        "operation" : "replace",
        "target" : "/example:top-level-leaf",
        "value" : {
          "example:top-level-leaf" : "sorry not sorry"
        }
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});

    // Create multiple things
    EXPECT_CHANGE(
        MODIFIED("/example:top-level-leaf", "whatever"),
        CREATED("/example:two-leafs/a", "value-a"),
        CREATED("/example:two-leafs/b", "value-b"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit1",
        "operation" : "replace",
        "target" : "/example:top-level-leaf",
        "value" : {
          "example:top-level-leaf" : "whatever"
        }
      },
      {
        "edit-id" : "edit2",
        "operation" : "create",
        "target" : "/example:two-leafs",
        "value" : {
          "example:two-leafs" : {
            "a": "value-a",
            "b": "value-b",
          }
        }
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});

    EXPECT_CHANGE(DELETED("/example:top-level-leaf", "whatever"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit",
        "operation" : "remove",
        "target" : "/example:top-level-leaf"
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});

    // edits cancel themselves
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit1",
        "operation" : "create",
        "target" : "/example:top-level-leaf",
        "value" : {
          "example:top-level-leaf" : "hi"
        }
      },
      {
        "edit-id" : "edit2",
        "operation" : "remove",
        "target" : "/example:top-level-leaf"
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});

    // empty edit list
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : []
  }
})") == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "No edits present"
      }
    ]
  }
}
)"});


    EXPECT_CHANGE(
        MODIFIED("/example:two-leafs/a", "aaa"),
        MODIFIED("/example:two-leafs/b", "bbb"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:two-leafs", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit",
        "operation" : "replace",
        "target" : "/example:a",
        "value" : {
          "example:a" : "aaa"
        }
      },
      {
        "edit-id" : "edit",
        "operation" : "replace",
        "target" : "/example:b",
        "value" : {
          "example:b" : "bbb"
        }
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});





    // create list entries

    EXPECT_CHANGE(
        CREATED("/example:tlc/list[name='libyang']", std::nullopt),
        CREATED("/example:tlc/list[name='libyang']/name", "libyang"),
        CREATED("/example:tlc/list[name='libyang']/choice1", "libyang"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit1",
        "operation" : "create",
        "target" : "/list=libyang",
        "value" : {
          "example:list" : [
            {
              "name" : "libyang",
              "choice1": "libyang"
            }
          ]
        }
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});

    EXPECT_CHANGE(
        CREATED("/example:tlc/list[name='sysrepo']", std::nullopt),
        CREATED("/example:tlc/list[name='sysrepo']/name", "sysrepo"),
        CREATED("/example:tlc/list[name='sysrepo']/choice1", "sysrepo"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit1",
        "operation" : "create",
        "target" : "/example:tlc/list=sysrepo",
        "value" : {
          "example:list" : [
            {
              "name" : "sysrepo",
              "choice1": "sysrepo"
            }
          ]
        }
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});

    // delete list entry
    EXPECT_CHANGE(
        DELETED("/example:tlc/list[name='libyang']", std::nullopt),
        DELETED("/example:tlc/list[name='libyang']/name", "libyang"),
        DELETED("/example:tlc/list[name='libyang']/choice1", "libyang"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit1",
        "operation" : "remove",
        "target" : "/list=libyang"
      }
    ]
  }
})") == Response{204, noContentTypeHeaders, ""});
}

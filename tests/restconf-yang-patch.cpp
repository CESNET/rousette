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
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});


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
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

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
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

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
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

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
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

    // empty edit list
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : []
  }
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
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
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});


    // create list entries

    EXPECT_CHANGE(
        CREATED("/example:tlc/list[name='libyang']", std::nullopt),
        CREATED("/example:tlc/list[name='libyang']/name", "libyang"),
        CREATED("/example:tlc/list[name='libyang']/choice1", "libyang"),
        CREATED("/example:tlc/list[name='netopeer2']", std::nullopt),
        CREATED("/example:tlc/list[name='netopeer2']/name", "netopeer2"),
        CREATED("/example:tlc/list[name='netopeer2']/choice2", "netopeer2"));
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
      },
      {
        "edit-id" : "edit2",
        "operation" : "create",
        "target" : "/list=netopeer2",
        "value" : {
          "example:list" : [
            {
              "name" : "netopeer2",
              "choice2": "netopeer2"
            }
          ]
        }
      }
    ]
  }
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

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
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

    // delete list entry
    EXPECT_CHANGE(
        DELETED("/example:tlc/list[name='sysrepo']", std::nullopt),
        DELETED("/example:tlc/list[name='sysrepo']/name", "sysrepo"),
        DELETED("/example:tlc/list[name='sysrepo']/choice1", "sysrepo"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit1",
        "operation" : "remove",
        "target" : "/example:tlc/list=sysrepo"
      }
    ]
  }
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

    // delete list entry
    EXPECT_CHANGE(
        DELETED("/example:tlc/list[name='netopeer2']", std::nullopt),
        DELETED("/example:tlc/list[name='netopeer2']/name", "netopeer2"),
        DELETED("/example:tlc/list[name='netopeer2']/choice2", "netopeer2"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit1",
        "operation" : "remove",
        "target" : "/list=netopeer2"
      }
    ]
  }
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});


    // modify list entry
    EXPECT_CHANGE(MODIFIED("/example:tlc/list[name='libyang']/choice1", "libyang-cpp"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit",
        "operation" : "replace",
        "target" : "/list=libyang",
        "value" : {
          "example:list" : [{
            "name": "libyang",
            "choice1": "libyang-cpp"
          }]
        }
      }
    ]
  }
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit",
        "operation" : "replace",
        "target" : "/list=libyang",
        "value" : {
          "example:list" : [{
            "name": "asdasdauisbdhaijbsdad",
            "choice1": "libyang-cpp"
          }]
        }
      }
    ]
  }
})") == Response{400, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "errors": {
      "error": [
        {
          "error-type": "protocol",
          "error-tag": "invalid-value",
          "error-path": "/example:tlc/list[name='asdasdauisbdhaijbsdad']/name",
          "error-message": "List key mismatch between URI path and data."
        }
      ]
    }
  }
}
)"});


    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({"ietf-yang-patch:yang-patch" : {}})") == Response{400, noContentTypeHeaders, ""});


    EXPECT_CHANGE(
        CREATED("/example:ordered-lists/ll[.='4']", "4"),
        CREATED("/example:ordered-lists/ll[.='2']", "2"),
        CREATED("/example:ordered-lists/ll[.='6']", "6"),
        CREATED("/example:ordered-lists/ll[.='3']", "3"),
        CREATED("/example:ordered-lists/ll[.='1']", "1"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:ordered-lists", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit-1",
        "operation" : "create",
        "target" : "/ll=4",
        "value" : {"example:ll" : ["4"]}
      },
      {
        "edit-id" : "edit-2",
        "operation" : "insert",
        "where" : "first",
        "target" : "/ll=2",
        "value" : {"example:ll" : ["2"]}
      },
      {
        "edit-id" : "edit-3",
        "operation" : "insert",
        "where" : "last",
        "target" : "/ll=6",
        "value" : {"example:ll" : ["6"]}
      },
      {
        "edit-id" : "edit-4",
        "operation" : "insert",
        "where" : "after",
        "point" : "/ll=2",
        "target" : "/ll=3",
        "value" : {"example:ll" : ["3"]}
      },
      {
        "edit-id" : "edit-5",
        "operation" : "insert",
        "where" : "before",
        "point" : "/ll=2",
        "target" : "/ll=1",
        "value" : {"example:ll" : ["1"]}
      }
    ]
  }
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

            REQUIRE(get(RESTCONF_DATA_ROOT "/example:ordered-lists", {AUTH_ROOT}) == Response{200, jsonHeaders, R"({
  "example:ordered-lists": {
    "ll": [
      "1",
      "2",
      "3",
      "4",
      "6"
    ]
  }
}
)"});

    EXPECT_CHANGE(
            MOVED("/example:ordered-lists/ll[.='2']", "2"),
            MOVED("/example:ordered-lists/ll[.='4']", "4"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:ordered-lists", {AUTH_ROOT, CONTENT_TYPE_YANG_PATCH_JSON}, R"({
  "ietf-yang-patch:yang-patch" : {
    "patch-id" : "patch",
    "edit" : [
      {
        "edit-id" : "edit-1",
        "operation" : "move",
        "target" : "/ll=2",
        "where" : "after",
        "point" : "/ll=3",
      },
      {
        "edit-id" : "edit-2",
        "operation" : "move",
        "target" : "/ll=4",
        "where" : "first"
      }
    ]
  }
})") == Response{200, jsonHeaders, R"({
  "ietf-yang-patch:yang-patch-status": {
    "patch-id": "patch",
    "ok": [null]
  }
}
)"});

            REQUIRE(get(RESTCONF_DATA_ROOT "/example:ordered-lists", {AUTH_ROOT}) == Response{200, jsonHeaders, R"({
  "example:ordered-lists": {
    "ll": [
      "4",
      "1",
      "3",
      "2",
      "6"
    ]
  }
}
)"});
}

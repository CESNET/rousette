/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10089";
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/datastoreUtils.h"
#include "tests/pretty_printers.h"

TEST_CASE("Plain patch")
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

    // create some data
    EXPECT_CHANGE(
        CREATED("/example:top-level-leaf", "str"),
        CREATED("/example:tlc/list[name='libyang']", std::nullopt),
        CREATED("/example:tlc/list[name='libyang']/name", "libyang"),
        CREATED("/example:tlc/list[name='libyang']/choice1", "libyang"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:top-level-leaf": "str", "example:tlc": {"list": [{"name": "libyang", "choice1": "libyang"}]}})") == Response{204, noContentTypeHeaders, ""});

    // replace a leaf value
    EXPECT_CHANGE(MODIFIED("/example:top-level-leaf", "other-str"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:top-level-leaf": "other-str"})") == Response{204, noContentTypeHeaders, ""});

    EXPECT_CHANGE(
        CREATED("/example:two-leafs/a", "a-val"),
        CREATED("/example:two-leafs/b", "b-val"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:two-leafs", {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:two-leafs": {"a": "a-val", "b": "b-val"}})") == Response{204, noContentTypeHeaders, ""});

    // replace only one value in the container
    EXPECT_CHANGE(MODIFIED("/example:two-leafs/a", "a-val-2"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:two-leafs", {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:two-leafs": {"a": "a-val-2"}})") == Response{204, noContentTypeHeaders, ""});

    // replace list entry value through the root URI
    EXPECT_CHANGE(MODIFIED("/example:tlc/list[name='libyang']/choice1", "libyang-1"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:tlc": {"list": [{"name": "libyang", "choice1": "libyang-1"}]}})") == Response{204, noContentTypeHeaders, ""});

    // replace list entry value through list entry URI
    EXPECT_CHANGE(MODIFIED("/example:tlc/list[name='libyang']/choice1", "libyang-2"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc/list=libyang", {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:list": [{"name": "libyang", "choice1": "libyang-2"}]})") == Response{204, noContentTypeHeaders, ""});

    // replace list entry value through the leaf URI
    EXPECT_CHANGE(MODIFIED("/example:tlc/list[name='libyang']/choice1", "libyang-3"));
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc/list=libyang/choice1", {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:choice1": "libyang-3"})") == Response{204, noContentTypeHeaders, ""});

    // key value mismatch in URI and data
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc/list=libyang", {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:list": [{"name": "blabla"}]})") == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/example:tlc/list[name='blabla']/name",
        "error-message": "List key mismatch between URI path and data."
      }
    ]
  }
}
)"});

    // list entry does not exist
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:tlc/list=blabla", {CONTENT_TYPE_JSON, AUTH_ROOT}, R"({"example:list": [{"name": "blabla", "choice1": "sysrepo"}]})") == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Target resource does not exist"
      }
    ]
  }
}
)"});


    // test XML content
    EXPECT_CHANGE(MODIFIED("/example:top-level-leaf", "yet-another-str"));
    REQUIRE(patch(RESTCONF_DATA_ROOT, {CONTENT_TYPE_XML, AUTH_ROOT}, R"(<top-level-leaf xmlns="http://example.tld/example">yet-another-str</top-level-leaf>)") == Response{204, noContentTypeHeaders, ""});

    // mismatch between content-type header and the data
    REQUIRE(patch(RESTCONF_DATA_ROOT, {CONTENT_TYPE_JSON, AUTH_ROOT}, R"(<top-level-leaf xmlns="http://example.tld/example">yet-another-yet-another-str</top-level-leaf>)") == Response{400, jsonHeaders, R"({
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

    // unsupported content type
    REQUIRE(patch(RESTCONF_DATA_ROOT, {{"content-type", "text/plain"}, AUTH_ROOT}, R"({"example:top-level-leaf": "other-str"})") == Response{415, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-not-supported",
        "error-message": "content-type format value not supported"
      }
    ]
  }
}
)"});

    // no content type
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT}, R"({"example:top-level-leaf": "other-str"})") == Response{400, jsonHeaders, R"({
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

    // sysrepo modifying meta data not allowed
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:top-level-leaf", {AUTH_ROOT, CONTENT_TYPE_JSON}, R"({"example:top-level-leaf": "a-value", "@example:top-level-leaf": {"ietf-netconf:operation": "replace"}})") == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-path": "/example:top-level-leaf",
        "error-message": "Meta attribute 'ietf-netconf:operation' not allowed."
      }
    ]
  }
}
)"});

    // empty JSON objects
    REQUIRE(patch(RESTCONF_DATA_ROOT, {AUTH_ROOT, CONTENT_TYPE_JSON}, "{}") == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "malformed-message",
        "error-message": "Empty data tree received."
      }
    ]
  }
}
)"});
    REQUIRE(patch(RESTCONF_DATA_ROOT "/example:two-leafs", {AUTH_ROOT, CONTENT_TYPE_JSON}, "{}") == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Node indicated by URI is missing."
      }
    ]
  }
}
)"});
}

/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10087";
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/datastoreUtils.h"

TEST_CASE("default handling")
{
    trompeloeil::sequence seq1;

    spdlog::set_level(spdlog::level::trace);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    auto nacmGuard = manageNacm(srSess);

    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    setupRealNacm(srSess);

    DatastoreChangesMock dsChangesMock;
    auto changesExampleRunning = datastoreChangesSubscription(srSess, dsChangesMock, "example");

    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};

    // default value of /example:a/b/c/enabled is implicitly set so it should not be printed
    REQUIRE(get(RESTCONF_DATA_ROOT "/example:a", {}) == Response{200, jsonHeaders, R"({

}
)"});

    // RFC 6243, sec. 2.3.3: A valid 'delete' operation attribute for a data node that has been set by the server to its schema default value MUST fail with a 'data-missing' error-tag.
    REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:a/b/c/enabled", {AUTH_ROOT}) == Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "data-missing",
        "error-path": "/example:a/b/c/enabled",
        "error-message": "Data is missing."
      }
    ]
  }
}
)"});

    // RFC 6243, sec. 2.3.3: A valid 'create' operation attribute for a data node that has been set by the server to its schema default value MUST succeed.
    REQUIRE(post(RESTCONF_DATA_ROOT "/example:a/b/c", R"({"example:enabled":true}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{201, jsonHeaders, ""});

    // RFC 6243, sec. 2.3.3: A valid 'create' operation attribute for a data node that has been set by a client to its schema default value MUST fail with a 'data-exists' error-tag.
    // RFC 8040, sec. 4.4.1: If the data resource already exists, then the POST request MUST fail and a "409 Conflict" status-line MUST be returned. The error-tag value "resource-denied" is used in this case.
    // This conflict of RFCs seems to be reported in errata https://www.rfc-editor.org/errata/eid5761 but no action was taken. Let's test according to implementation in RFC 8040
    REQUIRE(post(RESTCONF_DATA_ROOT "/example:a/b/c", R"({"example:enabled":true}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{409, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "resource-denied",
        "error-message": "Resource already exists."
      }
    ]
  }
}
)"});

    // default value is explicitly set so it should be printed
    REQUIRE(get(RESTCONF_DATA_ROOT "/example:a", {}) == Response{200, jsonHeaders, R"({
  "example:a": {
    "b": {
      "c": {
        "enabled": true
      }
    }
  }
}
)"});

    // RFC 6243, sec. 2.3.3: A valid 'delete' operation attribute for a data node that has been set by a client to its schema default value MUST succeed.
    REQUIRE(httpDelete(RESTCONF_DATA_ROOT "/example:a/b/c/enabled", {AUTH_ROOT}) == Response{204, noContentTypeHeaders, ""});

    // default value is implicitly set so it should be printed
    REQUIRE(get(RESTCONF_DATA_ROOT "/example:a", {}) == Response{200, jsonHeaders, R"({

}
)"});
}

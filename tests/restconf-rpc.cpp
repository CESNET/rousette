/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

static const auto SERVER_PORT = "10084";
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/datastoreUtils.h"
#include "tests/pretty_printers.h"

struct RpcCall {
    MAKE_MOCK2(rpcCall, void(std::string_view, const std::map<std::string, std::string>&));
};

std::map<std::string, std::string> nodesToMap(libyang::DataNode node)
{
    std::map<std::string, std::string> res;

    for (const auto& c : node.childrenDfs()) {
        if (c.isTerm()) {
            res[c.path()] = c.asTerm().valueStr();
        }
    }

    return res;
}

TEST_CASE("invoking actions and rpcs")
{
    spdlog::set_level(spdlog::level::trace);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    auto sub = subscribeRunningForOperDs(srSess, "example");
    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};

    setupRealNacm(srSess);

    trompeloeil::sequence seq1;
    RpcCall rpcCall;

    // rpc callbacks
    auto rpc1 = srSess.onRPCAction("/example:test-rpc", [&](sysrepo::Session, auto, auto path, libyang::DataNode input, auto, auto, libyang::DataNode output) {
        rpcCall.rpcCall(path, nodesToMap(input));
        output.newPath("out1", "some-output-string", libyang::CreationOptions::Output);
        output.newPath("out2", "some-output-string-2", libyang::CreationOptions::Output);
        return sysrepo::ErrorCode::Ok;
    });
    auto rpc2 = srSess.onRPCAction("/example:test-rpc-no-output", [&](sysrepo::Session, auto, auto path, libyang::DataNode input, auto, auto, auto) {
        rpcCall.rpcCall(path, nodesToMap(input));
        return sysrepo::ErrorCode::Ok;
    });
    auto rpc3 = std::make_unique<sysrepo::Subscription>(srSess.onRPCAction("/example:test-rpc-no-input-no-output", [&](sysrepo::Session, auto, auto path, auto, auto, auto, auto) {
        rpcCall.rpcCall(path, {});
        return sysrepo::ErrorCode::Ok;
    }));
    auto rpc4 = srSess.onRPCAction("/example:test-rpc-no-input", [&](sysrepo::Session, auto, auto path, auto, auto, auto, libyang::DataNode output) {
        rpcCall.rpcCall(path, {});
        output.newPath("out1", "some-output-string", libyang::CreationOptions::Output);
        output.newPath("out2", "some-output-string-2", libyang::CreationOptions::Output);
        return sysrepo::ErrorCode::Ok;
    });
    auto rpc5 = srSess.onRPCAction("/example:tlc/list/example-action", [&](sysrepo::Session, auto, auto path, libyang::DataNode input, auto, auto, libyang::DataNode output) {
        rpcCall.rpcCall(path, nodesToMap(input));
        output.newPath("o", "some-output-string", libyang::CreationOptions::Output);
        return sysrepo::ErrorCode::Ok;
    });


    SECTION("RPC")
    {
        // create a list entry so we can test actions in a list
        srSess.setItem("/example:tlc/list[name='1']/choice1", "bla");
        srSess.applyChanges();

        SECTION("Basic calls")
        {
            REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc", std::map<std::string, std::string>({{"/example:test-rpc/i", "ahoj"}})));
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"({"example:input": {"i":"ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{200, jsonHeaders, R"({
  "example:output": {
    "out1": "some-output-string",
    "out2": "some-output-string-2"
  }
}
)"});

            REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc-no-output", std::map<std::string, std::string>({{"/example:test-rpc-no-output/number", "42"}, {"/example:test-rpc-no-output/string", "ahoj"}})));
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc-no-output", R"({"example:input": {"number": 42, "string":"ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, noContentTypeHeaders, ""});

            REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc-no-input-no-output", std::map<std::string, std::string>({})));
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc-no-input-no-output", "", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, noContentTypeHeaders, ""});
        }

        SECTION("Data format")
        {
            SECTION("JSON -> JSON")
            {
                REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc", std::map<std::string, std::string>({{"/example:test-rpc/i", "ahoj"}})));
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"({"example:input": {"i":"ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{200, jsonHeaders, R"({
  "example:output": {
    "out1": "some-output-string",
    "out2": "some-output-string-2"
  }
}
)"});
            }

            SECTION("XML -> XML")
            {
                REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc", std::map<std::string, std::string>({{"/example:test-rpc/i", "ahoj"}})));
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"(<input xmlns="http://example.tld/example"><i>ahoj</i></input>)", {AUTH_ROOT, CONTENT_TYPE_XML}) == Response{200, xmlHeaders, R"(<output xmlns="http://example.tld/example">
  <out1>some-output-string</out1>
  <out2>some-output-string-2</out2>
</output>
)"});
            }

            SECTION("XML -> JSON")
            {
                REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc", std::map<std::string, std::string>({{"/example:test-rpc/i", "ahoj"}})));
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"(<input xmlns="http://example.tld/example"><i>ahoj</i></input>)", {AUTH_ROOT, CONTENT_TYPE_XML, {"accept", "application/yang-data+json"}}) == Response{200, jsonHeaders, R"({
  "example:output": {
    "out1": "some-output-string",
    "out2": "some-output-string-2"
  }
}
)"});
            }

            SECTION("JSON -> XML")
            {
                REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc", std::map<std::string, std::string>({{"/example:test-rpc/i", "ahoj"}})));
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"({"example:input": {"i":"ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON, {"accept", "application/yang-data+xml"}}) == Response{200, xmlHeaders, R"(<output xmlns="http://example.tld/example">
  <out1>some-output-string</out1>
  <out2>some-output-string-2</out2>
</output>
)"});
            }

            SECTION("Missing content-type, some data sent")
            {
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"(<input xmlns="http://example.tld/example"><i>ahoj</i></input>)", {AUTH_ROOT}) == Response{400, jsonHeaders, R"({
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
            }

            SECTION("Missing content-type, no input nodes for RPC")
            {
                REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc-no-input", std::map<std::string, std::string>({})));
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc-no-input", "", {AUTH_ROOT}) == Response{200, jsonHeaders, R"({
  "example:output": {
    "out1": "some-output-string",
    "out2": "some-output-string-2"
  }
}
)"});
            }

            SECTION("No output does not send content-type")
            {
                REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc-no-output", std::map<std::string, std::string>({{"/example:test-rpc-no-output/number", "42"}, {"/example:test-rpc-no-output/string", "ahoj"}})));
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc-no-output", R"({"example:input": {"number":42, "string": "ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{204, noContentTypeHeaders, ""});
            }
        }

        SECTION("Calling RPC through /restconf/data")
        {
            REQUIRE(post(RESTCONF_DATA_ROOT "/example:test-rpc", R"({"example:i": "ahoj"}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-failed",
        "error-message": "RPC '/example:test-rpc' must be requested using operation prefix"
      }
    ]
  }
}
)"});

            SECTION("Unknown input nodes")
            {
                REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"({"example:input": {"i":"ahoj", "nope": "nope"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: Can't parse into operation data tree: LY_EVALID"
      }
    ]
  }
}
)"});
            }
        }

        SECTION("Missing mandatory input node")
        {
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"({"example:input": {}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"eof({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Input data validation failed"
      }
    ]
  }
}
)eof"});
        }

        SECTION("Missing input for RPC with input nodes")
        {
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", "", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"eof({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Input data validation failed"
      }
    ]
  }
}
)eof"});
        }

        SECTION("Input not wrapped in example:input")
        {
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc", R"({"example:test-rpc/i": "ahoj"})", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-message": "Validation failure: Can't parse into operation data tree: LY_EVALID"
      }
    ]
  }
}
)"});
        }

        SECTION("Callback does not return OK")
        {
            rpc3.reset();
            rpc3 = std::make_unique<sysrepo::Subscription>(srSess.onRPCAction("/example:test-rpc-no-input-no-output", [&](sysrepo::Session, auto, auto path, auto, auto, auto, auto) {
                rpcCall.rpcCall(path, {});
                return sysrepo::ErrorCode::OperationFailed;
            }));

            REQUIRE_CALL(rpcCall, rpcCall("/example:test-rpc-no-input-no-output", std::map<std::string, std::string>({})));
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:test-rpc-no-input-no-output", "", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{500, jsonHeaders, R"eof({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Internal server error due to sysrepo exception: Couldn't send RPC: SR_ERR_CALLBACK_FAILED\u000A Operation failed (SR_ERR_OPERATION_FAILED)\u000A User callback failed. (SR_ERR_CALLBACK_FAILED)"
      }
    ]
  }
}
)eof"});
        }
    }

    SECTION("Actions")
    {
        SECTION("Basic calls")
        {
            REQUIRE_CALL(rpcCall, rpcCall("/example:tlc/list/example-action", std::map<std::string, std::string>({{"/example:tlc/list[name='1']/example-action/i", "ahoj"}})));
            REQUIRE(post(RESTCONF_DATA_ROOT "/example:tlc/list=1/example-action", R"({"example:input": {"example:i": "ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{200, jsonHeaders, R"({
  "example:output": {
    "o": "some-output-string"
  }
}
)"});
        }

        SECTION("List entry with action not present")
        {
            REQUIRE(post(RESTCONF_DATA_ROOT "/example:tlc/list=666/example-action", R"({"example:input": {"example:i": "ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"eof({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "operation-failed",
        "error-message": "Action data node '/example:tlc/list[name='666']/example-action' does not exist."
      }
    ]
  }
}
)eof"});
        }

        SECTION("Invoking action through /restconf/operations")
        {
            REQUIRE(post(RESTCONF_OPER_ROOT "/example:tlc/list=1/example-action", R"({"example:input": {"example:i": "ahoj"}}")", {AUTH_ROOT, CONTENT_TYPE_JSON}) == Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "operation-failed",
        "error-message": "Action '/example:tlc/list/example-action' must be requested using data prefix"
      }
    ]
  }
}
)"});
        }
    }
}

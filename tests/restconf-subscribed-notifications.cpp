/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
static const auto SERVER_PORT = "10092";
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <regex>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/event_watchers.h"
#include "tests/pretty_printers.h"

#define SEND_NOTIFICATION(DATA) notifSession.sendNotification(*ctx.parseOp(DATA, libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);

constexpr auto uuidV4Regex = "[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-4[a-fA-F0-9]{3}-[89abAB][a-fA-F0-9]{3}-[a-fA-F0-9]{12}";

using namespace std::chrono_literals;
using namespace std::string_literals;

struct EstablishSubscriptionResult {
    uint32_t id;
    std::string url;
};

/** @brief Calls establish-subscription rpc, returns the url of the stream associated with the created subscription */
EstablishSubscriptionResult establishSubscription(
    const libyang::Context& ctx,
    const libyang::DataFormat rpcEncoding,
    const std::optional<std::pair<std::string, std::string>>& rpcRequestAuthHeader,
    const std::optional<std::string>& encodingLeafValue)
{
    constexpr auto jsonPrefix = "ietf-subscribed-notifications";
    constexpr auto xmlNamespace = "urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications";

    auto stopTime = libyang::yangTimeFormat(std::chrono::system_clock::now() + 5s, libyang::TimezoneInterpretation::Local);
    std::map<std::string, std::string> requestHeaders;
    ng::header_map expectedHeaders;

    if (rpcRequestAuthHeader) {
        requestHeaders.insert(*rpcRequestAuthHeader);
    }

    std::optional<libyang::DataNode> envelope;
    auto rpcTree = ctx.newPath("/ietf-subscribed-notifications:establish-subscription");
    rpcTree.newPath("stream", "NETCONF");
    rpcTree.newPath("stop-time", stopTime);

    if (encodingLeafValue) {
        rpcTree.newPath("encoding", *encodingLeafValue);
    }

    switch (rpcEncoding) {
    case libyang::DataFormat::JSON:
        requestHeaders.insert(CONTENT_TYPE_JSON);
        expectedHeaders = jsonHeaders;
        envelope = ctx.newOpaqueJSON({jsonPrefix, jsonPrefix, "input"}, std::nullopt);
        break;
    case libyang::DataFormat::XML:
        requestHeaders.insert(CONTENT_TYPE_XML);
        expectedHeaders = xmlHeaders;
        envelope = ctx.newOpaqueXML({xmlNamespace, jsonPrefix, "input"}, std::nullopt);
        break;
    default:
        FAIL("Unhandled libyang DataFormat");
        break;
    }

    // reconnect everything
    auto data = rpcTree.child();
    data->unlinkWithSiblings();
    envelope->insertChild(*data);

    auto body = *envelope->printStr(rpcEncoding, libyang::PrintFlags::WithSiblings);
    auto resp = post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", requestHeaders, body);
    REQUIRE(resp.equalStatusCodeAndHeaders(Response{200, expectedHeaders, ""}));

    auto reply = ctx.newPath("/ietf-subscribed-notifications:establish-subscription");
    REQUIRE(reply.parseOp(resp.data, rpcEncoding, libyang::OperationType::ReplyRestconf).tree);

    auto idNode = reply.findPath("id", libyang::InputOutputNodes::Output);
    REQUIRE(idNode);

    auto urlNode = reply.findPath("ietf-restconf-subscribed-notifications:uri", libyang::InputOutputNodes::Output);
    REQUIRE(urlNode);

    return {
        std::get<uint32_t>(idNode->asTerm().value()),
        urlNode->asTerm().valueStr(),
    };
}

TEST_CASE("RESTCONF subscribed notifications")
{
    trompeloeil::sequence seq1, seq2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    sysrepo::setGlobalContextOptions(sysrepo::ContextFlags::LibYangPrivParsed | sysrepo::ContextFlags::NoPrinted, sysrepo::GlobalContextEffect::Immediate);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    // parent for nested notification
    srSess.switchDatastore(sysrepo::Datastore::Operational);
    srSess.setItem("/example:tlc/list[name='k1']/choice1", "something must me here");
    srSess.applyChanges();
    std::vector<std::string> notificationsJSON{
        R"({"example:eventA":{"message":"blabla","progress":11}})",
        R"({"example:eventB":{}})",
        R"({"example-notif:something-happened":{}})",
        R"({"example:eventA":{"message":"almost finished","progress":99}})",
        R"({"example:tlc":{"list":[{"name":"k1","notif":{"message":"nested"}}]}})",
    };
    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    libyang::DataFormat rpcRequestEncoding = libyang::DataFormat::JSON;
    std::optional<std::string> rpcSubscriptionEncoding;
    std::optional<std::pair<std::string, std::string>> rpcRequestAuthHeader;

    SECTION("NACM authorization")
    {
        SECTION("Anonymous access for establish-subscription is disabled")
        {
            /* Remove anonymous user's permission to execute RPCs in ietf-subscribed-notifications.
             * Intentionally a SECTION. We can remove NACM rule for a while in order to test access denied.
             * The rule gets automatically restored for the rest of the tests.
             */
            srSess.switchDatastore(sysrepo::Datastore::Running);
            srSess.deleteItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='16']");
            srSess.applyChanges();

            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF" } })###")
                    == Response{403, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:establish-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)###"});
        }

        SECTION("User DWDM establishes subscription")
        {
            std::pair<std::string, std::string> user = AUTH_DWDM;
            auto [id, uri] = establishSubscription(srSess.getContext(), libyang::DataFormat::JSON, user, std::nullopt);

            std::map<std::string, std::string> headers;

            SECTION("Users who cannot GET")
            {
                SECTION("anonymous") { }
                SECTION("norules") { headers.insert(AUTH_NORULES); }
                REQUIRE(get(uri, headers) == Response{404, plaintextHeaders, "Subscription not found."});
            }

            SECTION("Users who can GET")
            {
                SECTION("root") { headers.insert(AUTH_ROOT); }
                SECTION("dwdm") { headers.insert(AUTH_DWDM); }
                REQUIRE(head(uri, headers) == Response{200, eventStreamHeaders, ""});
            }

            SECTION("GET on the same subscription concurently")
            {
                std::map<std::string, std::string> headers;
                std::string response;
                int status;

                SECTION("Allowed users")
                {
                    response = "There is already another GET request on this subscription.";
                    status = 409;

                    SECTION("Same user")
                    {
                        headers.insert(AUTH_DWDM);
                    }
                    SECTION("Different user")
                    {
                        headers.insert(AUTH_ROOT);
                    }
                }
                SECTION("Disallowed user")
                {
                    headers.insert(AUTH_NORULES);
                    response = "Subscription not found.";
                    status = 404;
                }

                PREPARE_LOOP_WITH_EXCEPTIONS
                auto thr = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
                    WAIT_UNTIL_SSE_CLIENT_REQUESTS;
                    REQUIRE(get(uri, headers) == Response{status, plaintextHeaders, response});
                }));

                SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_DWDM});
                RUN_LOOP_WITH_EXCEPTIONS;
            }

            SECTION("GET on the same subscription sequentially")
            {
                boost::asio::io_service io;
                {
                    std::binary_semaphore requestSent(0);
                    SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_DWDM});
                }
                {
                    std::binary_semaphore requestSent(0);
                    SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_DWDM});
                }
            }
        }
    }

    SECTION("Invalid establish-subscription requests")
    {
        // stop-time in the past
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "stop-time": "1999-09-09T09:09:09Z" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Couldn't create notification subscription: SR_ERR_INVAL_ARG\u000A Specified \"stop-time\" is in the past. (SR_ERR_INVAL_ARG)"
      }
    ]
  }
}
)###"});

        // invalid stream
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "ajsdhauisds" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Couldn't create notification subscription: SR_ERR_NOT_FOUND\u000A Failed to collect modules to subscribe to, invalid stream and/or XPath filter (Item not found). (SR_ERR_NOT_FOUND)"
      }
    ]
  }
}
)###"});

        // stream-filter-name is unsupported, but leafref validation triggers first
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {CONTENT_TYPE_JSON}, R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "stream-filter-name": "xyz" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "protocol",
        "error-tag": "invalid-value",
        "error-path": "/ietf-subscribed-notifications:establish-subscription/stream-filter-name",
        "error-message": "Invalid leafref value \"xyz\" - no target instance \"/sn:filters/sn:stream-filter/sn:name\" with the same value."
      }
    ]
  }
}
)###"});
    }

    SECTION("Valid requests")
    {
        SECTION("XML stream")
        {
            netconfWatcher.setDataFormat(libyang::DataFormat::XML);
            rpcRequestAuthHeader = AUTH_ROOT;

            SECTION("Stream encoding inferred from request content-type")
            {
                rpcRequestEncoding = libyang::DataFormat::XML;
            }

            SECTION("Explicitly asked for XML stream encoding")
            {
                rpcSubscriptionEncoding = "encode-xml";

                SECTION("Request content-type JSON")
                {
                    rpcRequestEncoding = libyang::DataFormat::JSON;
                }

                SECTION("Request content-type XML")
                {
                    rpcRequestEncoding = libyang::DataFormat::XML;
                }
            }

            EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[2], seq2);
            EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
        }

        SECTION("JSON stream")
        {
            netconfWatcher.setDataFormat(libyang::DataFormat::JSON);

            SECTION("NACM: anonymous user cannot read example-notif module")
            {
                rpcRequestAuthHeader = std::nullopt;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                rpcSubscriptionEncoding = "encode-json";

                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            }

            SECTION("Content-type with set encode leaf")
            {
                rpcRequestAuthHeader = AUTH_ROOT;

                SECTION("Stream encoding inferred from request content-type")
                {
                    rpcRequestEncoding = libyang::DataFormat::JSON;
                }

                SECTION("Explicitly asked for JSON stream encoding")
                {
                    rpcSubscriptionEncoding = "encode-json";
                    SECTION("Request content-type JSON")
                    {
                        rpcRequestEncoding = libyang::DataFormat::JSON;
                    }

                    SECTION("Request content-type XML")
                    {
                        rpcRequestEncoding = libyang::DataFormat::XML;
                    }
                }

                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[2], seq2);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            }
        }

        auto [id, uri] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding);
        REQUIRE(std::regex_match(uri, std::regex("/streams/subscribed/"s + uuidV4Regex)));

        PREPARE_LOOP_WITH_EXCEPTIONS

        // Here's how these two threads work together.
        //
        // The main test thread (this one):
        // - sets up all the expectations
        // - has an HTTP client which calls/spends the expectations based on the incoming SSE data
        // - blocks while it runs the ASIO event loop
        //
        // The auxiliary thread (the notificationThread):
        // - waits for the HTTP client having issued its long-lived HTTP GET
        // - sends a bunch of notifications to sysrepo
        // - waits for all the expectations getting spent, and then terminates the ASIO event loop cleanly

        std::jthread notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            WAIT_UNTIL_SSE_CLIENT_REQUESTS;

            SEND_NOTIFICATION(notificationsJSON[0]);
            SEND_NOTIFICATION(notificationsJSON[1]);
            std::this_thread::sleep_for(500ms); // simulate some delays; server might be slow in creating notifications, client should still remain connected
            SEND_NOTIFICATION(notificationsJSON[2]);
            SEND_NOTIFICATION(notificationsJSON[3]);
            std::this_thread::sleep_for(500ms);
            SEND_NOTIFICATION(notificationsJSON[4]);

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seq1);
            waitForCompletionAndBitMore(seq2);
        }));

        std::map<std::string, std::string> streamHeaders;
        if (rpcRequestAuthHeader) {
            streamHeaders.insert(*rpcRequestAuthHeader);
        }
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("delete-subscription")
    {
        std::optional<Response> expectedResponse;
        std::map<std::string, std::string> headers;
        headers.insert(CONTENT_TYPE_JSON);

        SECTION("Subscription created by dwdm")
        {
            rpcRequestAuthHeader = AUTH_DWDM;

            SECTION("dwdm (author) can delete")
            {
                headers.insert(AUTH_DWDM);
                expectedResponse = Response{204, noContentTypeHeaders, ""};
            }

            SECTION("Anonymous cannot delete anything because of NACM")
            {
                expectedResponse = Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)"};
            }

            SECTION("norules user")
            {
                headers.insert(AUTH_NORULES);
                expectedResponse = Response{404, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-value",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Subscription not found."
      }
    ]
  }
}
)"};
            }

            SECTION("root user")
            {
                headers.insert(AUTH_ROOT);
                expectedResponse = Response{400, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Trying to delete subscription not created by root. Use kill-subscription instead."
      }
    ]
  }
}
)"};
            }

            auto [id, uri] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding);
            auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + "}}";
            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:delete-subscription", headers, body) == expectedResponse);
        }

        SECTION("Anonymous user cannot delete subscription craeted by anonymous user")
        {
            rpcRequestAuthHeader = std::nullopt;
            auto [id, uri] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding);
            auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + "}}";
            REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:delete-subscription", headers, body) == Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:delete-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)"});
        }
    }

    SECTION("kill-subscription")
    {
        std::optional<Response> expectedResponse;
        std::map<std::string, std::string> headers;
        headers.insert(CONTENT_TYPE_JSON);

        SECTION("User cannot kill")
        {
            SECTION("dwdm (author)")
            {
                headers.insert(AUTH_DWDM);
            }

            SECTION("anonymous")
            {
            }

            expectedResponse = Response{403, jsonHeaders, R"({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "access-denied",
        "error-path": "/ietf-subscribed-notifications:kill-subscription",
        "error-message": "Access denied."
      }
    ]
  }
}
)"};
        }

        SECTION("root")
        {
            headers.insert(AUTH_ROOT);
            expectedResponse = Response{204, noContentTypeHeaders, ""};
        }

        auto [id, uri] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding);
        auto body = R"({"ietf-subscribed-notifications:input": { "id": )" + std::to_string(id) + "}}";
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:kill-subscription", headers, body) == expectedResponse);
    }
}

TEST_CASE("Terminating server under notification load")
{
    trompeloeil::sequence seq1;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = std::make_unique<rousette::restconf::Server>(srConn, SERVER_ADDRESS, SERVER_PORT);
    setupRealNacm(srSess);

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());
    constexpr auto notif = R"({"example:eventB":{}})";

    std::optional<std::string> rpcSubscriptionEncoding;
    std::optional<std::pair<std::string, std::string>> rpcRequestAuthHeader;

    std::pair<std::string, std::string> auth = AUTH_ROOT;
    auto [id, uri] = establishSubscription(srSess.getContext(), libyang::DataFormat::JSON, auth, std::nullopt);

    PREPARE_LOOP_WITH_EXCEPTIONS;

    std::atomic<bool> serverRunning = true;
    std::atomic<size_t> notificationsReceived = 0;
    constexpr size_t NOTIFICATIONS_BEFORE_TERMINATE = 50;

    auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
        auto notifSession = sysrepo::Connection{}.sessionStart();
        auto ctx = notifSession.getContext();

        WAIT_UNTIL_SSE_CLIENT_REQUESTS;

        while (serverRunning) {
            SEND_NOTIFICATION(notif);
        }
    }));

    auto serverShutdownThread = std::jthread([&]() {
        while (notificationsReceived <= NOTIFICATIONS_BEFORE_TERMINATE) {
            // A condition variable would be more elegant, but this is just a test...
            std::this_thread::sleep_for(20ms);
        }
        server.reset();
        serverRunning = false;
    });

    netconfWatcher.setDataFormat(libyang::DataFormat::JSON);
    REQUIRE_CALL(netconfWatcher, data(notif)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(NOTIFICATIONS_BEFORE_TERMINATE)).LR_SIDE_EFFECT(notificationsReceived++);
    REQUIRE_CALL(netconfWatcher, data(R"({ietf-subscribed-notifications:subscription-terminated":{"id":1,"reason":"no-such-subscription"}})")).IN_SEQUENCE(seq1).TIMES(AT_MOST(1));
    SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {auth});
    RUN_LOOP_WITH_EXCEPTIONS;
}

TEST_CASE("Cleaning up inactive subscriptions")
{
    trompeloeil::sequence seq1, seq2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    auto nacmGuard = manageNacm(srSess);
    constexpr auto inactivityTimeout = 2s;
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT, 0ms, 55s, inactivityTimeout};

    auto [id, uri] = establishSubscription(srSess.getContext(), libyang::DataFormat::JSON, {AUTH_ROOT}, std::nullopt);

    SECTION("Client connects and disconnects")
    {
        PREPARE_LOOP_WITH_EXCEPTIONS
        auto thr = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            std::this_thread::sleep_for(1s);
        }));

        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, {AUTH_ROOT});
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    std::this_thread::sleep_for(inactivityTimeout + 1500ms);
    REQUIRE(get(uri, {AUTH_ROOT}) == Response{404, plaintextHeaders, "Subscription not found."});
}

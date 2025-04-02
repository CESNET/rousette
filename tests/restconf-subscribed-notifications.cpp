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
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/event_watchers.h"
#include "tests/pretty_printers.h"

#define SEND_NOTIFICATION(DATA) notifSession.sendNotification(*ctx.parseOp(DATA, libyang::DataFormat::JSON, libyang::OperationType::NotificationYang).op, sysrepo::Wait::No);

using namespace std::chrono_literals;

struct EstablishSubscriptionResult {
    std::string url;
    std::optional<sysrepo::NotificationTimeStamp> replayStartTimeRevision;
};

using XPath = std::string;
using Filter = std::variant<std::monostate, XPath, libyang::XML>;

struct SubscribedNotifications {
    std::string stream;
    std::optional<Filter> filter;
    std::optional<sysrepo::NotificationTimeStamp> replayStartTime;
};

/** @brief Calls establish-subscription rpc, returns the url of the stream associated with the created subscription */
EstablishSubscriptionResult establishSubscription(
    const libyang::Context& ctx,
    const libyang::DataFormat rpcEncoding,
    const std::optional<std::pair<std::string, std::string>>& rpcRequestAuthHeader,
    const std::optional<std::string>& encodingLeafValue,
    const SubscribedNotifications& params)
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
    rpcTree.newPath("stop-time", stopTime);

    if (encodingLeafValue) {
        rpcTree.newPath("encoding", *encodingLeafValue);
    }

    rpcTree.newPath("stream", params.stream);
    if (params.filter) {
        if (std::holds_alternative<XPath>(*params.filter)) {
            rpcTree.newPath("stream-xpath-filter", std::get<XPath>(*params.filter));
        } else if (std::holds_alternative<libyang::XML>(*params.filter)) {
            rpcTree.newPath2("stream-subtree-filter", std::get<libyang::XML>(*params.filter));
        }
    }

    if (params.replayStartTime) {
        rpcTree.newPath("replay-start-time", libyang::yangTimeFormat(*params.replayStartTime, libyang::TimezoneInterpretation::Local));
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

    auto urlNode = reply.findPath("ietf-restconf-subscribed-notifications:uri", libyang::InputOutputNodes::Output);
    REQUIRE(urlNode);

    std::optional<sysrepo::NotificationTimeStamp> replayStartTimeRevision;
    if (auto node = reply.findPath("ietf-subscribed-notifications:replay-start-time-revision", libyang::InputOutputNodes::Output)) {
        replayStartTimeRevision = libyang::fromYangTimeFormat<sysrepo::NotificationTimeStamp::clock>(node->asTerm().valueStr());
    }

    return {urlNode->asTerm().valueStr(), replayStartTimeRevision};
}

TEST_CASE("RESTCONF subscribed notifications")
{
    trompeloeil::sequence seq1, seq2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    libyang::DataFormat rpcRequestEncoding = libyang::DataFormat::JSON;
    std::optional<std::string> rpcSubscriptionEncoding;
    std::optional<std::pair<std::string, std::string>> rpcRequestAuthHeader;

    SECTION("Invalid establish-subscription requests")
    {
        // stop-time in the past
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription",
                     {CONTENT_TYPE_JSON},
                     R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "stop-time": "1999-09-09T09:09:09Z" } })###")
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
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription",
                     {CONTENT_TYPE_JSON},
                     R"###({ "ietf-subscribed-notifications:input": { "stream": "ajsdhauisds" } })###")
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

        // stream-filter-name is not supported
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription",
                     {CONTENT_TYPE_JSON},
                     R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "stream-filter-name": "xyz" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Stream filtering with predefined filters is not supported"
      }
    ]
  }
}
)###"});

        // replay-start-time > stop-time
        REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription",
                     {CONTENT_TYPE_JSON},
                     R"###({ "ietf-subscribed-notifications:input": { "stream": "NETCONF", "replay-start-time": "2000-11-11T11:22:33Z", "stop-time": "2000-01-01T00:00:00Z" } })###")
                == Response{400, jsonHeaders, R"###({
  "ietf-restconf:errors": {
    "error": [
      {
        "error-type": "application",
        "error-tag": "invalid-attribute",
        "error-message": "Couldn't create notification subscription: SR_ERR_INVAL_ARG\u000A Specified \"stop-time\" is earlier than \"start-time\". (SR_ERR_INVAL_ARG)"
      }
    ]
  }
}
)###"});
    }

    SECTION("Subscribed notifications requests")
    {
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
        const std::string notificationForReplayJSON = R"({"example:eventA":{"message":"this-should-be-sent-very-early","progress":0}})";

        std::pair<sysrepo::NotificationTimeStamp, sysrepo::NotificationTimeStamp> replayedNotificationSendInterval; // bounds for replayed notification event time
        bool shouldReviseStartTime = false;

        SubscribedNotifications subNotif;
        subNotif.stream = "NETCONF";

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

            SECTION("XPath filter set")
            {
                rpcRequestAuthHeader = AUTH_ROOT;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                subNotif.filter = XPath{"/example:eventA | /example:eventB"};
                rpcSubscriptionEncoding = "encode-json";
                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            }

            SECTION("Subtree filter set")
            {
                rpcRequestAuthHeader = AUTH_ROOT;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                subNotif.filter = libyang::XML{"<eventA xmlns='http://example.tld/example' />"};
                rpcSubscriptionEncoding = "encode-json";
                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            }

            SECTION("Replays")
            {
                // announce replaySupport and send one notification before the client connects
                srConn.setModuleReplaySupport("example", true);

                {
                    auto notifSession = sysrepo::Connection{}.sessionStart();
                    auto ctx = notifSession.getContext();
                    replayedNotificationSendInterval.first = std::chrono::system_clock::now();
                    SEND_NOTIFICATION(notificationForReplayJSON);
                    replayedNotificationSendInterval.second = std::chrono::system_clock::now();
                }

                rpcRequestAuthHeader = AUTH_ROOT;
                rpcRequestEncoding = libyang::DataFormat::JSON;
                rpcSubscriptionEncoding = "encode-json";

                SECTION("replay-start-time-revision is announced to the client")
                {
                    subNotif.replayStartTime = std::chrono::system_clock::now() - 6666s /* reasonable time in the past, earlier than the replayed notification was sent */;

                    EXPECT_NOTIFICATION(notificationForReplayJSON, seq1);
                    EXPECT_NOTIFICATION(R"({"ietf-subscribed-notifications:replay-completed":{"id":8}})", seq1);
                    shouldReviseStartTime = true;
                }

                SECTION("replay-start-time-revision not announced")
                {
                    subNotif.replayStartTime = replayedNotificationSendInterval.second; /* start right after the (not) replayed notification was sent, this should not revise the start time */

                    EXPECT_NOTIFICATION(R"({"ietf-subscribed-notifications:replay-completed":{"id":9}})", seq1);
                }

                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[2], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            }

            SECTION("Content-type with set encode leaf")
            {
                rpcRequestAuthHeader = AUTH_ROOT;
                EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[2], seq2);
                EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
                EXPECT_NOTIFICATION(notificationsJSON[4], seq1);

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
            }
        }

        auto [uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, subNotif);
        if (shouldReviseStartTime) {
            REQUIRE(replayStartTimeRevision);
            REQUIRE(replayedNotificationSendInterval.first <= *replayStartTimeRevision);
            REQUIRE(*replayStartTimeRevision <= replayedNotificationSendInterval.second);
        } else {
            REQUIRE(!replayStartTimeRevision);
        }

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
            std::this_thread::sleep_for(135ms); // simulate some delays; server might be slow in creating notifications, client should still remain connected
            SEND_NOTIFICATION(notificationsJSON[2]);
            SEND_NOTIFICATION(notificationsJSON[3]);
            std::this_thread::sleep_for(222ms);
            SEND_NOTIFICATION(notificationsJSON[4]);

            // once the main thread has processed all the notifications, stop the ASIO loop
            waitForCompletionAndBitMore(seq1);
            waitForCompletionAndBitMore(seq2);
        }));

        REQUIRE(get(RESTCONF_DATA_ROOT "/ietf-subscribed-notifications:streams/stream=NETCONF", {AUTH_ROOT}) == Response{200, jsonHeaders, R"({
  "ietf-subscribed-notifications:streams": {
    "stream": [
      {
        "name": "NETCONF",
        "description": "Default NETCONF notification stream"
      }
    ]
  }
}
)"});

        std::map<std::string, std::string> streamHeaders;
        SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, streamHeaders);
        RUN_LOOP_WITH_EXCEPTIONS;
    }
}

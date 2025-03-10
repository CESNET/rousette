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

/** @brief Calls establish-subscription rpc, returns the url of the stream associated with the created subscription */
EstablishSubscriptionResult establishSubscription(
    const libyang::Context& ctx,
    const libyang::DataFormat rpcEncoding,
    const auto& rpcRequestAuthHeader,
    const std::optional<std::string>& encodingLeafValue,
    const std::optional<std::string>& filter,
    const std::optional<sysrepo::NotificationTimeStamp>& replayStartTime)
{
    auto stopTime = libyang::yangTimeFormat(std::chrono::system_clock::now() + 1s, libyang::TimezoneInterpretation::Local);
    std::string body;
    std::map<std::string, std::string> requestHeaders;
    ng::header_map expectedHeaders;

    if (rpcRequestAuthHeader) {
        requestHeaders.insert(*rpcRequestAuthHeader);
    }

    switch (rpcEncoding) {
    case libyang::DataFormat::JSON: {
        std::string encodingJsonNode;
        std::string xpathFilterJsonNode;
        std::string replayStartTimeJsonNode;

        if (encodingLeafValue) {
            encodingJsonNode = "\"encoding\": \"" + *encodingLeafValue + "\", ";
        }

        if (filter) {
            xpathFilterJsonNode = "\"stream-xpath-filter\": \"" + *filter + "\", ";
        }

        if (replayStartTime) {
            replayStartTimeJsonNode = "\"replay-start-time\": \"" + libyang::yangTimeFormat(*replayStartTime, libyang::TimezoneInterpretation::Local) + "\", ";
        }

        body = "{\"ietf-subscribed-notifications:input\": {" + encodingJsonNode + xpathFilterJsonNode + replayStartTimeJsonNode + "\"stream\": \"NETCONF\", \"stop-time\": \"" + stopTime + "\"}}";
        requestHeaders.insert(CONTENT_TYPE_JSON);
        expectedHeaders = jsonHeaders;
        break;
    }
    case libyang::DataFormat::XML: {
        std::string encodingXmlNode;
        std::string xpathFilterXmlNode;
        std::string replayStartTimeXmlNode;

        if (encodingLeafValue) {
            encodingXmlNode = "<encoding>" + *encodingLeafValue + "</encoding>";
        }

        if (filter) {
            xpathFilterXmlNode = "<stream-xpath-filter>" + *filter + "</stream-xpath-filter>";
        }

        if (replayStartTime) {
            replayStartTimeXmlNode = "<replay-start-time>" + libyang::yangTimeFormat(*replayStartTime, libyang::TimezoneInterpretation::Local) + "</replay-start-time>";
        }

        body = "<input xmlns=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">" + encodingXmlNode + xpathFilterXmlNode + "<stream>NETCONF</stream><stop-time>" + stopTime + "</stop-time></input>";
        requestHeaders.insert(CONTENT_TYPE_XML);
        expectedHeaders = xmlHeaders;
        break;
    }
    default:
        FAIL("Unhandled libyang DataFormat");
        break;
    }

    auto resp = post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", requestHeaders, body);
    REQUIRE(resp.equalStatusCodeAndHeaders(Response{200, expectedHeaders, ""}));

    auto envelope = ctx.newPath("/ietf-subscribed-notifications:establish-subscription");
    REQUIRE(envelope.parseOp(resp.data, rpcEncoding, libyang::OperationType::ReplyRestconf).tree);

    auto urlNode = envelope.findPath("ietf-restconf-subscribed-notifications:uri", libyang::InputOutputNodes::Output);
    REQUIRE(urlNode);

    std::optional<sysrepo::NotificationTimeStamp> replayStartTimeRevision;
    if (auto node = envelope.findPath("ietf-subscribed-notifications:replay-start-time-revision", libyang::InputOutputNodes::Output)) {
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

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    libyang::DataFormat rpcRequestEncoding = libyang::DataFormat::JSON;
    std::optional<std::string> rpcSubscriptionEncoding;
    std::optional<std::string> rpcStreamXPathFilter;
    std::optional<sysrepo::NotificationTimeStamp> rpcReplayStart;
    std::optional<std::pair<std::string, std::string>> rpcRequestAuthHeader;
    std::pair<sysrepo::NotificationTimeStamp, sysrepo::NotificationTimeStamp> replayedNotificationSendInterval; // bounds for replayed notification event time

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
            rpcStreamXPathFilter = "/example:eventA | /example:eventB";
            rpcSubscriptionEncoding = "encode-json";
            EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
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
            rpcStreamXPathFilter = std::nullopt;

            SECTION("replay-start-time-revision is announced to the client")
            {
                rpcReplayStart = std::chrono::system_clock::now() - 6666s /* reasonable time in the past, earlier than the replayed notification was sent */;

                EXPECT_NOTIFICATION(notificationForReplayJSON, seq1);
                EXPECT_NOTIFICATION(R"({"ietf-subscribed-notifications:replay-completed":{"id":6}})", seq1);
            }

            SECTION("replay-start-time-revision not announced")
            {
                rpcReplayStart = replayedNotificationSendInterval.second; /* start right after the (not) replayed notification was sent, this should not revise the start time */

                EXPECT_NOTIFICATION(R"({"ietf-subscribed-notifications:replay-completed":{"id":7}})", seq1);
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

    auto [uri, replayStartTimeRevision] = establishSubscription(srSess.getContext(), rpcRequestEncoding, rpcRequestAuthHeader, rpcSubscriptionEncoding, rpcStreamXPathFilter, rpcReplayStart);
    if (replayStartTimeRevision) {
        REQUIRE(replayedNotificationSendInterval.first <= *replayStartTimeRevision);
        REQUIRE(*replayStartTimeRevision <= replayedNotificationSendInterval.second);
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

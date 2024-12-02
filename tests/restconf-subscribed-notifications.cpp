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

/** @brief Calls establish-subscription rpc, returns the url of the stream associated with the created subscription */
std::string establishSubscription(const libyang::Context& ctx, const std::map<std::string, std::string>& headers, const std::string& body)
{
    auto resp = post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {AUTH_ROOT, CONTENT_TYPE_JSON}, body);
    REQUIRE(resp.equalStatusCodeAndHeaders(Response{200, jsonHeaders, ""}));

    auto envelope = ctx.newPath("/ietf-subscribed-notifications:establish-subscription");
    REQUIRE(envelope.parseOp(resp.data, libyang::DataFormat::JSON, libyang::OperationType::ReplyRestconf).tree);

    auto urlNode = envelope.findPath("ietf-restconf-subscribed-notifications:uri", libyang::InputOutputNodes::Output);
    REQUIRE(urlNode);

    return urlNode->asTerm().valueStr();
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

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;
    std::map<std::string, std::string> headers;

    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    std::string encodingJsonNode;

    SECTION("XML stream")
    {
        netconfWatcher.setDataFormat(libyang::DataFormat::XML);
        headers = {AUTH_ROOT};

        EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
        EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
        EXPECT_NOTIFICATION(notificationsJSON[2], seq2);
        EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
        EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
        encodingJsonNode = R"("encoding": "encode-xml", )";
    }

    SECTION("JSON stream")
    {
        netconfWatcher.setDataFormat(libyang::DataFormat::JSON);
        SECTION("anonymous user cannot read example-notif module")
        {
            EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[2], seq1); // FIXME: this should not be here, is sr_sn not dealing with nacm?
            EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            encodingJsonNode = R"("encoding": "encode-json", )";
        }

        SECTION("root user can see everything")
        {
            headers = {AUTH_ROOT};
            EXPECT_NOTIFICATION(notificationsJSON[0], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[1], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[2], seq2);
            EXPECT_NOTIFICATION(notificationsJSON[3], seq1);
            EXPECT_NOTIFICATION(notificationsJSON[4], seq1);
            encodingJsonNode = ""; // use encoding of the caller's request message
        }
    }

    std::string stopTime = libyang::yangTimeFormat(std::chrono::system_clock::now() + 2s, libyang::TimezoneInterpretation::Local);
    auto uri = establishSubscription(
        srSess.getContext(),
        {AUTH_ROOT, CONTENT_TYPE_JSON},
        "{\"ietf-subscribed-notifications:input\": {" + encodingJsonNode + "\"stream\": \"NETCONF\", \"stop-time\": \"" + stopTime + "\"}}");

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
        std::this_thread::sleep_for(500ms);
        SEND_NOTIFICATION(notificationsJSON[3]);
        SEND_NOTIFICATION(notificationsJSON[4]);

        // once the main thread has processed all the notifications, stop the ASIO loop
        waitForCompletionAndBitMore(seq1);
        waitForCompletionAndBitMore(seq2);
    }));

    SSEClient cli(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, uri, headers);
    RUN_LOOP_WITH_EXCEPTIONS;
}

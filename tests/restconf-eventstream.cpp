/*
 * Copyright (C) 2025 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
static const auto SERVER_PORT = "10091";
#include <latch>
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

TEST_CASE("Event stream tests")
{
    trompeloeil::sequence seq1, seq2;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    setupRealNacm(srSess);

    const std::string notification(R"({"example:eventA":{"message":"blabla","progress":11}})");
    RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    SECTION("Termination on server shutdown")
    {
        auto server = std::make_unique<rousette::restconf::Server>(srConn, SERVER_ADDRESS, SERVER_PORT);

        EXPECT_NOTIFICATION(notification, seq1);
        EXPECT_NOTIFICATION(notification, seq1);
        EXPECT_NOTIFICATION(notification, seq1);

        auto notifSession = sysrepo::Connection{}.sessionStart();
        auto ctx = notifSession.getContext();

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            auto notifSession = sysrepo::Connection{}.sessionStart();
            auto ctx = notifSession.getContext();

            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            SEND_NOTIFICATION(notification);
            SEND_NOTIFICATION(notification);
            SEND_NOTIFICATION(notification);
            waitForCompletionAndBitMore(seq1);

            auto beforeShutdown = std::chrono::system_clock::now();
            server.reset();
            auto shutdownDuration = std::chrono::system_clock::now() - beforeShutdown;
            REQUIRE(shutdownDuration < 5s);
        }));

        SSEClient client(io, SERVER_ADDRESS, SERVER_PORT, requestSent, netconfWatcher, "/streams/NETCONF/JSON", std::map<std::string, std::string>{AUTH_ROOT});

        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("Keep-alive pings")
    {
        constexpr auto pingInterval = 1s;

        RestconfNotificationWatcher netconfWatcher(srConn.sessionStart().getContext());
        auto server = std::make_unique<rousette::restconf::Server>(srConn, SERVER_ADDRESS, SERVER_PORT, std::chrono::milliseconds{0}, pingInterval);

        auto notifSession = sysrepo::Connection{}.sessionStart();
        auto ctx = notifSession.getContext();

        EXPECT_NOTIFICATION(notification, seq1);
        expectations.emplace_back(NAMED_REQUIRE_CALL(netconfWatcher, comment(": keep-alive")).IN_SEQUENCE(seq2).TIMES(AT_LEAST(1)));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            SEND_NOTIFICATION(notification);
            std::this_thread::sleep_for(3s); // Wait for the server to send at least one keep-alive ping
            server.reset();
        }));

        SSEClient client(
            io,
            SERVER_ADDRESS,
            SERVER_PORT,
            requestSent,
            netconfWatcher,
            "/streams/NETCONF/JSON",
            std::map<std::string, std::string>{AUTH_ROOT},
            boost::posix_time::seconds{5},
            SSEClient::ReportIgnoredLines::Yes);

        RUN_LOOP_WITH_EXCEPTIONS;
    }
}

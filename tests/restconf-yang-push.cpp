/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
static const auto SERVER_PORT = "10089";
#include <latch>
#include <libyang-cpp/Time.hpp>
#include <nghttp2/asio_http2.h>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/pretty_printers.h"

#define EXPECT_NOTIFICATION(DATA, SEQ) expectations.emplace_back(NAMED_REQUIRE_CALL(netconfWatcher, data(DATA)).IN_SEQUENCE(SEQ));

using namespace std::chrono_literals;

TEST_CASE("Subscribed notifications")
{
    trompeloeil::sequence seq1;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    auto server = rousette::restconf::Server{srConn, SERVER_ADDRESS, SERVER_PORT};
    setupRealNacm(srSess);

    NotificationWatcher netconfWatcher(srConn.sessionStart().getContext());

    REQUIRE(post(RESTCONF_OPER_ROOT "/ietf-subscribed-notifications:establish-subscription", {AUTH_ROOT, CONTENT_TYPE_JSON}, R"({
   "ietf-subscribed-notifications:input": {
      "stream": "NETCONF"
   }
})") == Response{200, jsonHeaders, R"({
  "ietf-subscribed-notifications:output": {
    "id": 1,
    "ietf-restconf-subscribed-notifications:uri": "/streams/subscribed/1"
  }
}
)"});

    EXPECT_NOTIFICATION("", seq1);

    PREPARE_LOOP_WITH_EXCEPTIONS;

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

        // wait until the client sends its HTTP request
        requestSent.wait();

        notifSession.setItem("/example:top-level-leaf", "42");
        notifSession.applyChanges();
    }));

    std::string uri = "/streams/subscribed/1";
    std::map<std::string, std::string> headers = {AUTH_ROOT};

    SSEClient cli(io, requestSent, netconfWatcher, uri, headers);
    RUN_LOOP_WITH_EXCEPTIONS;
    waitForCompletionAndBitMore(seq1);
}

/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/utils.hpp>
#include "restconf/Server.h"
#include "tests/aux-utils.h"
#include "tests/event_watchers.h"
#include "tests/pretty_printers.h"
#include "tests/restconf-http-calls.h"

using namespace std::chrono_literals;
using namespace std::string_literals;

TEST_CASE("SSE proxy for configured subscriptions")
{
    trompeloeil::sequence seq1, seq2, seq3, seq4;
    sysrepo::setLogLevelStderr(sysrepo::LogLevel::Information);
    spdlog::set_level(spdlog::level::trace);

    std::vector<std::unique_ptr<trompeloeil::expectation>> expectations;

    sysrepo::setGlobalContextOptions(sysrepo::ContextFlags::LibYangPrivParsed | sysrepo::ContextFlags::NoPrinted, sysrepo::GlobalContextEffect::Immediate);
    auto srConn = sysrepo::Connection{};
    auto srSess = srConn.sessionStart(sysrepo::Datastore::Running);
    srSess.sendRPC(srSess.getContext().newPath("/ietf-factory-default:factory-reset"));

    auto nacmGuard = manageNacm(srSess);
    setupRealNacm(srSess);

    const auto sseProxy = "/ietf-subscribed-notifications:subscriptions/ietf-subscribed-notif-receivers:receiver-instances/receiver-instance[name='example']/rousette:sse-proxy"s;

    // Only the 'optics' group (i.e., the dwdm user) may consume
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='sse-proxy']/module-name", "rousette");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='sse-proxy']/action", "permit");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='sse-proxy']/access-operations", "read");
    srSess.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='sse-proxy']/path", sseProxy + "/nacm-access-check");
    srSess.applyChanges();

    // some data to push
    srSess.switchDatastore(sysrepo::Datastore::Startup);
    srSess.setItem("/example:top-level-leaf", "42");
    srSess.setItem("/example:top-level-list[name='hello']", std::nullopt);
    srSess.applyChanges();
    srSess.switchDatastore(sysrepo::Datastore::Running);

    const auto subs = "/ietf-subscribed-notifications:subscriptions"s;
    srSess.setItem(sseProxy + "/nacm-username", "root");
    srSess.setItem(sseProxy + "/nacm-access-check", std::nullopt);

    const auto sub1 = subs + "/subscription[id='1']";
    srSess.setItem(sub1 + "/encoding", "ietf-subscribed-notifications:encode-json");
    srSess.setItem(sub1 + "/ietf-yang-push:datastore", "ietf-datastores:startup");
    srSess.setItem(sub1 + "/ietf-yang-push:datastore-xpath-filter", "/example:top-level-leaf");
    srSess.setItem(sub1 + "/ietf-yang-push:periodic/period", "5"); // 5 centiseconds = 50ms
    srSess.setItem(sub1 + "/receivers/receiver[name='r1']/ietf-subscribed-notif-receivers:receiver-instance-ref", "example");

    const auto sub2 = subs + "/subscription[id='2']";
    srSess.setItem(sub2 + "/encoding", "ietf-subscribed-notifications:encode-json");
    srSess.setItem(sub2 + "/ietf-yang-push:datastore", "ietf-datastores:startup");
    srSess.setItem(sub2 + "/ietf-yang-push:datastore-xpath-filter", "/example:top-level-list");
    srSess.setItem(sub2 + "/ietf-yang-push:periodic/period", "5");
    srSess.setItem(sub2 + "/receivers/receiver[name='r1']/ietf-subscribed-notif-receivers:receiver-instance-ref", "example");

    srSess.applyChanges();

    auto server = std::make_unique<rousette::restconf::Server>(srConn, SERVER_ADDRESS, SERVER_PORT);

    SECTION("Simple test: 2 subs, 1 endpoint, 2 clients")
    {
        RestconfYangPushWatcher watcher1(srSess.getContext());
        RestconfYangPushWatcher watcher2(srSess.getContext());
        watcher1.setDataFormat(libyang::DataFormat::JSON);
        watcher2.setDataFormat(libyang::DataFormat::JSON);

        RestconfYangPushWatcher watcherDenied(srSess.getContext());
        watcherDenied.setDataFormat(libyang::DataFormat::JSON);

        const auto fromSubscription1 = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})"s;
        const auto fromSubscription2 = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-list":[{"name":"hello"}]}}})"s;

        // seq1-4 only act as guards for waitForCompletionAndBitMore
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher1, data(fromSubscription1)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)));
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher1, data(fromSubscription2)).IN_SEQUENCE(seq2).TIMES(AT_LEAST(1)));
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher2, data(fromSubscription1)).IN_SEQUENCE(seq3).TIMES(AT_LEAST(1)));
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher2, data(fromSubscription2)).IN_SEQUENCE(seq4).TIMES(AT_LEAST(1)));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            waitForCompletionAndBitMore(seq1);
            waitForCompletionAndBitMore(seq2);
            waitForCompletionAndBitMore(seq3);
            waitForCompletionAndBitMore(seq4);
        }));

        SSEClient client1(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcher1, "/streams/rousette:sse-proxy/example", {AUTH_DWDM}, 200, 6s);
        SSEClient client2(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcher2, "/streams/rousette:sse-proxy/example", {AUTH_ROOT}, 200, 6s);
        SSEClient clientDenied(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcherDenied, "/streams/rousette:sse-proxy/example", {AUTH_NORULES}, 403, 6s);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("a new endpoint appears when it is configured")
    {
        REQUIRE(get("/streams/rousette:sse-proxy/later", {AUTH_ROOT}) == Response{403, plaintextHeaders, "Access denied."});

        const auto later = "/ietf-subscribed-notifications:subscriptions/ietf-subscribed-notif-receivers:receiver-instances/receiver-instance[name='later']/rousette:sse-proxy"s;
        srSess.setItem(later + "/nacm-username", "root");
        srSess.setItem(later + "/nacm-access-check", std::nullopt);
        const auto sub = subs + "/subscription[id='3']";
        srSess.setItem(sub + "/encoding", "ietf-subscribed-notifications:encode-json");
        srSess.setItem(sub + "/ietf-yang-push:datastore", "ietf-datastores:startup");
        srSess.setItem(sub + "/ietf-yang-push:datastore-xpath-filter", "/example:top-level-leaf");
        srSess.setItem(sub + "/ietf-yang-push:periodic/period", "5");
        srSess.setItem(sub + "/receivers/receiver[name='r1']/ietf-subscribed-notif-receivers:receiver-instance-ref", "later");
        srSess.applyChanges();

        RestconfYangPushWatcher watcher(srSess.getContext());
        watcher.setDataFormat(libyang::DataFormat::JSON);
        const auto pushed = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})"s;
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(pushed)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            waitForCompletionAndBitMore(seq1);
        }));

        SSEClient client(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcher, "/streams/rousette:sse-proxy/later", {AUTH_ROOT}, 200, 6s);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("a client is not disconnected by a configuration change elsewhere")
    {
        RestconfYangPushWatcher watcher(srSess.getContext());
        watcher.setDataFormat(libyang::DataFormat::JSON);

        const auto before = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})"s;
        const auto afterwards = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"666"}}})"s;
        const auto theOtherSubscription = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-list":[{"name":"hello"}]}}})"s;

        ALLOW_CALL(watcher, data(theOtherSubscription)); // the endpoint's second feed, of no interest here
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(before)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)));
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(afterwards)).IN_SEQUENCE(seq2).TIMES(AT_LEAST(1)));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            waitForCompletionAndBitMore(seq1);

            // Configure an endpoint which our client has nothing to do with. Every subscription is re-established by
            // this, but 'example' is still configured, so the stream our client holds has to survive.
            const auto later = "/ietf-subscribed-notifications:subscriptions/ietf-subscribed-notif-receivers:receiver-instances/receiver-instance[name='later']/rousette:sse-proxy"s;
            srSess.setItem(later + "/nacm-username", "root");
            srSess.setItem(later + "/nacm-access-check", std::nullopt);
            srSess.applyChanges();

            // A disconnected client would never see this.
            srSess.switchDatastore(sysrepo::Datastore::Startup);
            srSess.setItem("/example:top-level-leaf", "666");
            srSess.applyChanges();
            srSess.switchDatastore(sysrepo::Datastore::Running);

            waitForCompletionAndBitMore(seq2);
        }));

        SSEClient client(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcher, "/streams/rousette:sse-proxy/example", {AUTH_ROOT}, 200, 6s);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("a subscription added to an existing endpoint reaches the clients already attached to it")
    {
        RestconfYangPushWatcher watcher(srSess.getContext());
        watcher.setDataFormat(libyang::DataFormat::JSON);

        const auto fromLeaf = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})"s;
        const auto fromList = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-list":[{"name":"hello"}]}}})"s;
        const auto fromLeaf2 = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf2":"y"}}})"s;

        ALLOW_CALL(watcher, data(fromList)); // the endpoint's other feed, of no interest here
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(fromLeaf)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)));
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(fromLeaf2)).IN_SEQUENCE(seq2).TIMES(AT_LEAST(1)));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            waitForCompletionAndBitMore(seq1);

            srSess.switchDatastore(sysrepo::Datastore::Startup);
            srSess.setItem("/example:top-level-leaf2", "y");
            srSess.applyChanges();
            srSess.switchDatastore(sysrepo::Datastore::Running);

            // A third subscription for the endpoint our client is already reading. It must reach that client without
            // it reconnecting, i.e. the endpoint has to keep its identity and just gain a feed.
            const auto sub = subs + "/subscription[id='3']";
            srSess.setItem(sub + "/encoding", "ietf-subscribed-notifications:encode-json");
            srSess.setItem(sub + "/ietf-yang-push:datastore", "ietf-datastores:startup");
            srSess.setItem(sub + "/ietf-yang-push:datastore-xpath-filter", "/example:top-level-leaf2");
            srSess.setItem(sub + "/ietf-yang-push:periodic/period", "5");
            srSess.setItem(sub + "/receivers/receiver[name='r1']/ietf-subscribed-notif-receivers:receiver-instance-ref", "example");
            srSess.applyChanges();

            waitForCompletionAndBitMore(seq2);
        }));

        SSEClient client(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcher, "/streams/rousette:sse-proxy/example", {AUTH_ROOT}, 200, 6s);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("a subscription removed from an endpoint stops feeding it")
    {
        RestconfYangPushWatcher watcher(srSess.getContext());
        watcher.setDataFormat(libyang::DataFormat::JSON);

        const auto fromLeaf = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})"s;
        const auto fromLeafAfterwards = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"666"}}})"s;
        const auto fromList = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-list":[{"name":"hello"}]}}})"s;

        ALLOW_CALL(watcher, data(fromList)); // whatever was in flight when the subscription went away
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(fromLeaf)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)));
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(fromLeafAfterwards)).IN_SEQUENCE(seq2).TIMES(AT_LEAST(1)));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            waitForCompletionAndBitMore(seq1);

            srSess.deleteItem(subs + "/subscription[id='2']");
            srSess.applyChanges();

            srSess.switchDatastore(sysrepo::Datastore::Startup);
            srSess.setItem("/example:top-level-list[name='world']", std::nullopt); // sub2 is gone, no notification
            srSess.setItem("/example:top-level-leaf", "666"); // still visible
            srSess.applyChanges();
            srSess.switchDatastore(sysrepo::Datastore::Running);

            waitForCompletionAndBitMore(seq2);
        }));

        SSEClient client(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcher, "/streams/rousette:sse-proxy/example", {AUTH_ROOT}, 200, 6s);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("an endpoint removed from the configuration stops serving its clients")
    {
        RestconfYangPushWatcher watcher(srSess.getContext());
        watcher.setDataFormat(libyang::DataFormat::JSON);

        const auto fromLeaf = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-leaf":"42"}}})"s;
        const auto fromList = R"({"ietf-yang-push:push-update":{"datastore-contents":{"example:top-level-list":[{"name":"hello"}]}}})"s;

        ALLOW_CALL(watcher, data(fromList)); // the endpoint's other feed
        expectations.emplace_back(NAMED_REQUIRE_CALL(watcher, data(fromLeaf)).IN_SEQUENCE(seq1).TIMES(AT_LEAST(1)));

        PREPARE_LOOP_WITH_EXCEPTIONS;
        auto notificationThread = std::jthread(wrap_exceptions_and_asio(bg, io, [&]() {
            WAIT_UNTIL_SSE_CLIENT_REQUESTS;
            waitForCompletionAndBitMore(seq1);

            // Delete endpoint and its subs
            srSess.deleteItem(subs + "/subscription[id='1']");
            srSess.deleteItem(subs + "/subscription[id='2']");
            srSess.deleteItem("/ietf-subscribed-notifications:subscriptions/ietf-subscribed-notif-receivers:receiver-instances/receiver-instance[name='example']");
            srSess.applyChanges();

            // this should be silent
            srSess.switchDatastore(sysrepo::Datastore::Startup);
            srSess.setItem("/example:top-level-leaf", "666");
            srSess.applyChanges();
            srSess.switchDatastore(sysrepo::Datastore::Running);

            // Give an endpoint which outlived its configuration the time to give itself away
            std::this_thread::sleep_for(250ms);
        }));

        SSEClient client(io, SERVER_ADDRESS, SERVER_PORT, requestSent, watcher, "/streams/rousette:sse-proxy/example", {AUTH_ROOT}, 200, 2s);
        RUN_LOOP_WITH_EXCEPTIONS;
    }

    SECTION("access is guarded by NACM")
    {
        // no read access to the nacm-access-check node
        REQUIRE(get("/streams/rousette:sse-proxy/example", {AUTH_NORULES}) == Response{403, plaintextHeaders, "Access denied."});
        REQUIRE(get("/streams/rousette:sse-proxy/example", {}) == Response{403, plaintextHeaders, "Access denied."});

        // an endpoint which is not configured at all is indistinguishable from one which is not accessible
        REQUIRE(get("/streams/rousette:sse-proxy/nonexistent", {AUTH_DWDM}) == Response{403, plaintextHeaders, "Access denied."});
        REQUIRE(get("/streams/rousette:sse-proxy/nonexistent", {AUTH_ROOT}) == Response{403, plaintextHeaders, "Access denied."});
    }
}

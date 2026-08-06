/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */
#pragma once

#include <boost/asio/io_context.hpp>
#include <libyang-cpp/Enum.hpp>
#include <list>
#include <memory>
#include <sysrepo-cpp/Subscription.hpp>
#include <type_traits>
#include <vector>
#include "http/EventStream.h"
#include "restconf/SubscriptionBroadcaster.h"

namespace rousette::restconf {

/** @brief One SSE endpoint served at /streams/rousette:sse-proxy/<name>, fed by one or more configured subscriptions. */
class SseProxyEndpoint {
public:
    struct SourceSubscription {
        sysrepo::DynamicSubscription subscription;
        libyang::DataFormat encoding;
    };

    SseProxyEndpoint(std::vector<SourceSubscription> sources, boost::asio::io_context& io);

    const std::shared_ptr<http::EventStream::EventSignal>& events() const { return m_events; }

private:
    struct Feed {
        sysrepo::DynamicSubscription subscription;
        SubscriptionBroadcaster broadcaster; ///< references `subscription` above, hence declared after it and destroyed first

        Feed(SourceSubscription source, boost::asio::io_context& io, std::shared_ptr<http::EventStream::EventSignal> events);
    };

    std::shared_ptr<http::EventStream::EventSignal> m_events;

    /* A node-based container, because a Feed must never be relocated: after a move, two things would still point at
     * the old storage. Its broadcaster references the subscription declared above it, and the pending async_wait
     * handler captured `this` -- asio keeps that handler in its own storage and does not retarget it, so the next
     * notification would reach the address the feed used to live at.
     *
     * Declared last so all feeds are destroyed before m_events is destroyed.
     */
    static_assert(!std::is_move_constructible_v<SubscriptionBroadcaster>);
    std::list<Feed> m_feeds;
};
}

/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <libyang-cpp/Enum.hpp>
#include <list>
#include <memory>
#include <mutex>
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
    ~SseProxyEndpoint();

    void replaceFeeds(std::vector<SourceSubscription> sources);

    const std::shared_ptr<http::EventStream::EventSignal>& events() const { return m_events; }

    /** @brief Fired when this endpoint goes away, so that the clients attached to it are closed.
     *
     * Destroying the endpoint would otherwise only disconnect the slots of @ref m_events, leaving every attached client
     * with a stream which never delivers anything and is never closed.
     */
    http::EventStream::Termination& termination() { return m_termination; }

    /** @brief Hands container over to the IO thread and lets it die there. */
    template <typename Container>
    static void deleteLater(boost::asio::io_context& io, Container container)
    {
        boost::asio::post(io, [container = std::move(container)]() {
            // The lifetime has been extended until now, let it die at the scope end.
        });
    }

private:
    struct Feed {
        sysrepo::DynamicSubscription subscription;
        SubscriptionBroadcaster broadcaster; ///< references `subscription` above, hence declared after it and destroyed first

        Feed(SourceSubscription source,
             boost::asio::io_context& io,
             std::shared_ptr<http::EventStream::EventSignal> events,
             std::mutex& processEventMutex);
    };

    boost::asio::io_context& m_io;
    std::shared_ptr<http::EventStream::EventSignal> m_events;
    http::EventStream::Termination m_termination;

    std::mutex m_processEventMutex;

    /* A node-based container, because a Feed must never be relocated: after a move, two things would still point at
     * the old storage. Its broadcaster references the subscription declared above it, and the pending async_wait
     * handler captured `this`. boost::asio keeps that handler in its own storage and does not retarget it, so the next
     * notification would reach the address the feed used to live at.
     *
     * Declared last so all feeds are destroyed before m_events is destroyed.
     */
    static_assert(!std::is_move_constructible_v<SubscriptionBroadcaster>);
    std::list<Feed> m_feeds;
};
}

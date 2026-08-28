/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>
#include "restconf/SseProxyEndpoint.h"

namespace rousette::restconf {

SseProxyEndpoint::Feed::Feed(SseProxyEndpoint::SourceSubscription source,
                             boost::asio::io_context& io,
                             std::shared_ptr<http::EventStream::EventSignal> events,
                             std::mutex& processEventMutex)
    : subscription(std::move(source.subscription))
    , broadcaster(io, subscription, source.encoding, std::move(events), processEventMutex)
{
}

SseProxyEndpoint::SseProxyEndpoint(std::vector<SourceSubscription> sources, boost::asio::io_context& io)
    : m_io(io)
    , m_events(std::make_shared<http::EventStream::EventSignal>())
{
    replaceFeeds(std::move(sources));
}

void SseProxyEndpoint::replaceFeeds(std::vector<SourceSubscription> sources)
{
    {
        std::lock_guard lock(m_processEventMutex);

        // Terminate old subscriptions
        for (auto& feed : m_feeds) {
            feed.subscription.terminate();
        }

        /* Subscriptions are terminated, but the Feeds cannot be destroyed here. Suppose:
         *  1) sysrepo writes a notification just before we took the lock,
         *  2) asio finishes the wait and queues the handler which captures `SubscriptionBroadcaster` object,
         *  3) we destroy the Feed, but the handler still runs later and reads an already freed m_subscription.
         *
         * Feed may only be destroyed when no handler of it is queued or running.
         * Only the IO thread knows if no Feeds are being queued/processed, it is the one which runs them,
         * so lets hand the Feeds over and let them die there. They will not be in the queue by now, we have
         * already terminated the subscriptions.
         */
        std::list<Feed> toDelete;
        toDelete.splice(toDelete.end(), m_feeds); // move nodes from m_feeds, splice() does not reallocate addresses
        deleteLater(m_io, std::move(toDelete));
    }

    // Each feed's broadcaster drains from construction and forwards notifications into the shared signal.
    // The signal has no receivers yet, which is intentional: this is a live stream, so notifications arriving
    // before any client connects are dropped rather than buffered.
    for (auto& source : sources) {
        m_feeds.emplace_back(std::move(source), m_io, events(), m_processEventMutex);
    }
}

/** @brief Asks every on-change feed to resync. */
void SseProxyEndpoint::resyncOnChangeFeeds()
{
    std::lock_guard lock(m_processEventMutex);

    for (const auto& feed : m_feeds) {
        if (feed.subscription.type() != sysrepo::DynamicSubscriptionType::YangPushOnChange) {
            continue;
        }

        try {
            feed.subscription.resyncOnChange();
        } catch (const std::exception& e) {
            spdlog::error("Could not resync the on-change subscription {}: {}", feed.subscription.subscriptionId(), e.what());
        }
    }
}

SseProxyEndpoint::~SseProxyEndpoint()
{
    // Close the attached clients before the feeds (and with them m_events) go away; see termination().
    m_termination();
}
}

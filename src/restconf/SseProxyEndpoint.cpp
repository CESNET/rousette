/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <boost/asio/post.hpp>
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

        /* Terminated is not the same as safe to destroy, so the feeds go to the io thread to die there.
         * Suppose:
         *  1) sysrepo writes a notification just before we got here
         *  2) asio has then already taken the operation out of the reactor and queued the broadcaster's handler,
         *     so release() in ~SubscriptionBroadcaster finds nothing left to cancel,
         *     and that handler later runs with a success code and reads already freed m_subscription.
         *
         * The mutex above does not help, because the io thread is not holding it yet, the handler is not even running yet.
         * So lets put a deletion task into the event loop queue to be safe.
         */
        std::list<Feed> toDelete;
        toDelete.splice(toDelete.end(), m_feeds); // move nodes from m_feeds, splice() does not reallocate addresses
        boost::asio::post(m_io, [toDelete = std::move(toDelete)]() mutable { toDelete.clear(); });
    }

    // Each feed's broadcaster drains from construction and forwards notifications into the shared signal.
    // The signal has no receivers yet, which is intentional: this is a live stream, so notifications arriving
    // before any client connects are dropped rather than buffered.
    for (auto& source : sources) {
        m_feeds.emplace_back(std::move(source), m_io, events(), m_processEventMutex);
    }
}

SseProxyEndpoint::~SseProxyEndpoint()
{
    // Close the attached clients before the feeds (and with them m_events) go away; see termination().
    m_termination();
}
}

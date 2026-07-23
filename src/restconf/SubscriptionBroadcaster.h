/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <libyang-cpp/Enum.hpp>
#include <memory>
#include <mutex>
#include <sysrepo-cpp/Subscription.hpp>
#include "http/EventStream.h"

namespace rousette::restconf {

/** @brief Reads from a single sysrepo dynamic subscription and broadcasts its notifications to any number of clients.
 *
 * Draining runs for the whole lifetime of the broadcaster:
 * it starts in the constructor and stops in the destructor, so the object must not be constructed until the signal
 * already has its receiver(s). Anything drained before then is lost.
 *
 * @warning  The subscription itself is only *referenced* and must outlive the broadcaster.
 * The subscription can be mutated from another thread, so pass the mutex guarding those mutators; it is held around processEvent(), which is not thread safe against them.
 */
class SubscriptionBroadcaster {
public:
    SubscriptionBroadcaster(boost::asio::io_context& io,
                            const sysrepo::DynamicSubscription& subscription,
                            libyang::DataFormat dataFormat,
                            std::shared_ptr<http::EventStream::EventSignal> events,
                            std::mutex& processEventMutex);
    ~SubscriptionBroadcaster();

private:
    const sysrepo::DynamicSubscription& m_subscription;
    libyang::DataFormat m_dataFormat;
    std::shared_ptr<http::EventStream::EventSignal> m_events; ///< Broadcast sink
    std::mutex& m_processEventMutex;
    boost::asio::posix::stream_descriptor m_stream;

    void awaitNextNotification();
};
}

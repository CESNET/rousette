/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
*/

#include <libyang-cpp/DataNode.hpp>
#include "restconf/SubscriptionBroadcaster.h"
#include "restconf/utils/io.h"
#include "restconf/utils/yang.h"

namespace rousette::restconf {

SubscriptionBroadcaster::SubscriptionBroadcaster(
    boost::asio::io_context& io,
    const sysrepo::DynamicSubscription& subscription,
    libyang::DataFormat dataFormat,
    std::shared_ptr<http::EventStream::EventSignal> events,
    std::mutex* processEventMutex)
    : m_subscription(subscription)
    , m_dataFormat(dataFormat)
    , m_events(std::move(events))
    , m_processEventMutex(processEventMutex)
    , m_stream(io, subscription.fd())
{
    awaitNextNotification();
}

SubscriptionBroadcaster::~SubscriptionBroadcaster()
{
    // The stream does not own the file descriptor, sysrepo does. It will be closed when the subscription terminates.
    m_stream.release();
}

/** @brief Waits for the next notifications and process them */
void SubscriptionBroadcaster::awaitNextNotification()
{
    constexpr auto MAX_EVENTS = 50;

    m_stream.async_wait(boost::asio::posix::stream_descriptor::wait_read, [this](const boost::system::error_code& err) {
        // Unfortunately wait_read does not return operation_aborted when the file descriptor is closed and poll results in POLLHUP
        if (err == boost::asio::error::operation_aborted || utils::pipeIsClosedAndNoData(m_subscription.fd())) {
            return;
        }

        size_t eventsProcessed = 0;
        /* Process all the available notifications, but at most N
         * In case sysrepo is providing the events fast enough, this loop would still run inside the event loop
         * and the event responsible for sending the data to the client would not get to be processed.
         * TODO: Is this enough? What if this async_wait keeps getting called and nothing gets sent?
         */
        while (++eventsProcessed < MAX_EVENTS && utils::pipeHasData(m_subscription.fd())) {
            // sysrepo-cpp's processEvent and terminate is not thread safe. No lock when nothing can mutate this subscription.
            std::unique_lock<std::mutex> lock;
            if (m_processEventMutex) {
                lock = std::unique_lock{*m_processEventMutex};
            }

            m_subscription.processEvent([this](const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
                (*m_events)(as_restconf_notification(m_subscription.getSession().getContext(), m_dataFormat, *notificationTree, time));
            });
        }

        awaitNextNotification();
    });
}
}

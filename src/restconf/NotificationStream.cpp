/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <libyang-cpp/Time.hpp>
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include "http/EventStream.h"
#include "restconf/NotificationStream.h"
#include "utils/yang.h"

namespace {
/** @brief Wraps a notification data tree with RESTCONF notification envelope. */
std::string as_restconf_notification(const libyang::Context& ctx, libyang::DataFormat dataFormat, libyang::DataNode notification, const sysrepo::NotificationTimeStamp& time)
{
    static const auto jsonNamespace = "ietf-restconf";
    static const auto xmlNamespace = "urn:ietf:params:xml:ns:netconf:notification:1.0";

    std::optional<libyang::DataNode> envelope;
    std::optional<libyang::DataNode> eventTime;
    std::string timeStr = libyang::yangTimeFormat(time, libyang::TimezoneInterpretation::Local);

    /* The namespaces for XML and JSON evenlopes are different. See https://datatracker.ietf.org/doc/html/rfc8040#section-6.4 */
    if (dataFormat == libyang::DataFormat::JSON) {
        envelope = ctx.newOpaqueJSON(jsonNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueJSON(jsonNamespace, "eventTime", libyang::JSON{timeStr});
    } else {
        envelope = ctx.newOpaqueXML(xmlNamespace, "notification", std::nullopt);
        eventTime = ctx.newOpaqueXML(xmlNamespace, "eventTime", libyang::XML{timeStr});
    }

    // the notification data node holds only the notification data tree but for nested notification we should print the whole YANG data tree
    while (notification.parent()) {
        notification = *notification.parent();
    }

    envelope->insertChild(*eventTime);
    envelope->insertChild(notification);

    auto res = *envelope->printStr(dataFormat, libyang::PrintFlags::WithSiblings);

    // notification node comes from sysrepo and sysrepo will free this; if not unlinked then envelope destructor would try to free this as well
    notification.unlink();

    return res;
}

void createNotificationSub(std::optional<sysrepo::Subscription>& sub, sysrepo::Session& session, const std::string& moduleName, rousette::http::EventStream::Signal& signal, libyang::DataFormat dataFormat)
{
    auto notifCb = [&signal, dataFormat](auto session, auto, sysrepo::NotificationType type, const std::optional<libyang::DataNode>& notificationTree, const sysrepo::NotificationTimeStamp& time) {
        if (type != sysrepo::NotificationType::Realtime) {
            return;
        }

        signal(as_restconf_notification(session.getContext(), dataFormat, *notificationTree, time));
    };

    if (!sub) {
        sub = session.onNotification(moduleName, std::move(notifCb));
    } else {
        sub->onNotification(moduleName, std::move(notifCb));
    }
}
}

namespace rousette::restconf {

NotificationStream::NotificationStream(const nghttp2::asio_http2::server::request& req, const nghttp2::asio_http2::server::response& res, std::shared_ptr<rousette::http::EventStream::Signal> signal, sysrepo::Session session, libyang::DataFormat dataFormat)
    : EventStream(req, res, *signal)
    , m_notificationSignal(signal)
    , m_session(std::move(session))
    , m_dataFormat(dataFormat)
{
}

void NotificationStream::activate()
{
    for (const auto& mod : m_session.getContext().modules()) {
        if (mod.implemented() && mod.name() != "sysrepo" /* unsupported to subscribe to sysrepo module */) {
            try {
                createNotificationSub(m_notifSubs, m_session, mod.name(), *m_notificationSignal, m_dataFormat);
            } catch (sysrepo::ErrorWithCode& e) {
                /* We are iterating through all modules in order to subscribe to every possible module.
                 * If the module does not define any notifications or the module does not exist then sysrepo throws with ErrorCode::NotFound
                 * (see sysrepo's sr_notif_subscribe and sr_subscr_notif_xpath_check).
                 *
                 * We can either scan the YANG schema and search for notifications nodes (like netopeer2) or ignore this particular exception.
                 */
                if (e.code() != sysrepo::ErrorCode::NotFound) {
                    throw;
                }
            }
        }
    }

    EventStream::activate();
}
}

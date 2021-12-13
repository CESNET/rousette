/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/signals2.hpp>
#include <memory>
#include <optional>
#include <sysrepo-cpp/Connection.hpp>

/** @short Communication with sysrepo */
namespace rousette::sr {

/** @short Listen for changes in the operational datastore */
class AllEvents {
public:
    /** @short What YANG-level attributes to keep */
    enum class WithAttributes {
        All, ///< Keep all attributes
        RemoveEmptyOperationAndOrigin, ///< Remove sysrepo:operation=none and ietf-origin:unknown
        None, ///< Remove all attributes
    };
    using Signal = boost::signals2::signal<void(const std::string& module, const std::string& json)>;
    AllEvents(sysrepo::Session session, const WithAttributes attrBehavior);

    Signal change;

private:
    sysrepo::ErrorCode onChange(sysrepo::Session session, const std::string& module);

    std::optional<sysrepo::Subscription> sub;
    WithAttributes attrBehavior;
};
}

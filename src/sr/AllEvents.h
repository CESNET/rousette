/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/signals2.hpp>
#include <memory>

namespace sysrepo {
class Session;
class Subscribe;
}

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
    using Signal = boost::signals2::signal<void(const std::string& json)>;
    AllEvents(std::shared_ptr<sysrepo::Session> session, const WithAttributes attrBehavior);

    Signal change;

private:
    int onChange(std::shared_ptr<sysrepo::Session> session, const std::string& module);

    std::shared_ptr<sysrepo::Subscribe> sub;
    WithAttributes attrBehavior;
};
}

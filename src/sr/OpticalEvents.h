/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/signals2.hpp>
#include <memory>
#include <mutex>
#include <sysrepo-cpp/Connection.hpp>

/** @short Communication with sysrepo */
namespace rousette::sr {

/** @short Listen for ops updates of DWDM-related parameters */
class OpticalEvents {
public:
    using Signal = boost::signals2::signal<void(const std::string& json)>;
    OpticalEvents(sysrepo::Session session);

    Signal change;

    std::string currentData() const;

private:
    sysrepo::ErrorCode onChange(sysrepo::Session session, const std::string& module);

    mutable std::mutex mtx;
    std::optional<sysrepo::Subscription> sub;
    std::string lastData;
};
}

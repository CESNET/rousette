/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/signals2.hpp>
#include <memory>
#include <mutex>

namespace sysrepo {
class Session;
class Subscribe;
}

/** @short Communication with sysrepo */
namespace rousette::sr {

/** @short Listen for ops updates of DWDM-related parameters */
class OpticalEvents {
public:
    using Signal = boost::signals2::signal<void(const std::string& json)>;
    OpticalEvents(std::shared_ptr<sysrepo::Session> session);

    Signal change;

    std::string currentData() const;

private:
    int onChange(std::shared_ptr<sysrepo::Session> session, const std::string& module);

    std::shared_ptr<sysrepo::Subscribe> sub;
    mutable std::mutex mtx;
    std::string lastData;
};
}

/*
 * Copyright (C) 2020, 2022 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <sysrepo-cpp/Connection.hpp>
#include "sysrepo.h"

namespace rousette::restconf {

ScopedDatastoreSwitch::ScopedDatastoreSwitch(sysrepo::Session session, sysrepo::Datastore ds)
    : m_session(std::move(session))
    , m_oldDatastore(m_session.activeDatastore())
{
    m_session.switchDatastore(ds);
}

ScopedDatastoreSwitch::~ScopedDatastoreSwitch()
{
    m_session.switchDatastore(m_oldDatastore);
}

sysrepo::Datastore datastoreFromString(const std::string& datastore)
{
    if (datastore == "ietf-datastores:running") {
        return sysrepo::Datastore::Running;
    } else if (datastore == "ietf-datastores:operational") {
        return sysrepo::Datastore::Operational;
    } else if (datastore == "ietf-datastores:candidate") {
        return sysrepo::Datastore::Candidate;
    } else if (datastore == "ietf-datastores:startup") {
        return sysrepo::Datastore::Startup;
    } else if (datastore == "ietf-datastores:factory-default") {
        return sysrepo::Datastore::FactoryDefault;
    }

    throw std::runtime_error("Unknown datastore '" + datastore + "'");
}
}

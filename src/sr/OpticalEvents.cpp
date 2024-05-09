/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <memory>
#include <set>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/utils/exception.hpp>
#include <sysrepo.h>
#include "sr/OpticalEvents.h"

namespace {
std::string dumpDataFrom(sysrepo::Session session, const std::string& module)
{
    return *session.getData('/' + module + ":*")->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings);
}
}

namespace rousette::sr {

OpticalEvents::OpticalEvents(sysrepo::Session session)
    : sub(std::nullopt)
{
    session.switchDatastore(sysrepo::Datastore::Operational);

    // Because it's "tricky" to request data from several top-level modules via sysrepo (and nothing else),
    // just pick a first module to listen for.
    for (const auto& mod : {"czechlight-roadm-device", "czechlight-coherent-add-drop", "czechlight-inline-amp", "czechlight-bidi-amp"}) {
        try {
            sysrepo::ModuleChangeCb cb = [this](const auto sess, auto, auto name, auto, auto, auto) {
                return onChange(sess, std::string{name});
            };
            sub = session.onModuleChange(mod, cb, std::nullopt, 0, sysrepo::SubscribeOptions::DoneOnly | sysrepo::SubscribeOptions::Passive);
            spdlog::debug("Listening for module {}", mod);
            std::unique_lock lock{mtx};
            lastData = dumpDataFrom(session, mod);
            return;
        } catch (sysrepo::ErrorWithCode& e) {
            if (e.code() == sysrepo::ErrorCode::NotFound) {
                // nothing to listen for, just ignore this
                continue;
            }
            throw;
        }
    }

    spdlog::warn("Telemetry disabled. No CzechLight YANG modules found.");
}

sysrepo::ErrorCode OpticalEvents::onChange(sysrepo::Session session, const std::string& module)
{
    std::unique_lock lock{mtx};
    assert(session.activeDatastore() == sysrepo::Datastore::Operational);
    lastData = dumpDataFrom(session, module);
    spdlog::debug("change: {} bytes", lastData.size());

    // I wanted this to be a bit smarter, with a subtree filter to remove "unwated changes" and what not.
    // Given that we do not have a full-blown subtree filtering (yet), let's just return the data upon any change.
    change(lastData);
    return sysrepo::ErrorCode::Ok;
}

std::string OpticalEvents::currentData() const
{
    std::unique_lock lock{mtx};
    return lastData;
}
}

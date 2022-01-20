/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <memory>
#include <set>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo.h>
#include "sr/OpticalEvents.h"

using namespace std::literals;

namespace {
std::string dumpDataFrom(std::shared_ptr<sysrepo::Session> session, const std::string& module)
{
    return session->get_data(('/' + module + ":*").c_str())->print_mem(LYD_JSON, LYP_WITHSIBLINGS);
}
}

namespace rousette::sr {

OpticalEvents::OpticalEvents(std::shared_ptr<sysrepo::Session> session)
    : sub(std::make_shared<sysrepo::Subscribe>(session))
{
    session->session_switch_ds(SR_DS_OPERATIONAL);

    // Because it's "tricky" to request data from several top-level modules via sysrepo (and nothing else),
    // just pick a first module to listen for.
    for (const auto& mod : {"czechlight-roadm-device", "czechlight-coherent-add-drop", "czechlight-inline-amp"}) {
        try {
            sub->module_change_subscribe(mod, [this](const auto sess, const auto name, const auto, const auto, const auto) {
                        return onChange(sess, name);
                    }, nullptr, 0, SR_SUBSCR_CTX_REUSE | SR_SUBSCR_DONE_ONLY | SR_SUBSCR_PASSIVE);
            spdlog::debug("Listening for module {}", mod);
            std::unique_lock lock{mtx};
            lastData = dumpDataFrom(session, mod);
            return;
        } catch (sysrepo::sysrepo_exception& e) {
            if (e.error_code() == SR_ERR_NOT_FOUND) {
                // nothing to listen for, just ignore this
                continue;
            }
            throw;
        }
    }
    throw std::runtime_error{"No DWDM modules found"};
}

int OpticalEvents::onChange(std::shared_ptr<sysrepo::Session> session, const std::string& module)
{
    assert(session->session_get_ds() == SR_DS_OPERATIONAL);
    std::unique_lock lock{mtx};
    lastData = dumpDataFrom(session, module);
    spdlog::debug("change: {} bytes", lastData.size());

    // I wanted this to be a bit smarter, with a subtree filter to remove "unwated changes" and what not.
    // Given that we do not have a full-blown subtree filtering (yet), let's just return the data upon any change.
    change(lastData);
    return SR_ERR_OK;
}

std::string OpticalEvents::currentData() const
{
    std::unique_lock lock{mtx};
    return lastData;
}
}

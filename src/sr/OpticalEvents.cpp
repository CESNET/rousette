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
constexpr std::initializer_list<std::string_view> prefixes {
    {"/czechlight-roadm-device:spectrum-scan"sv},
    {"/czechlight-roadm-device:media-channels/power"sv},
    {"/czechlight-roadm-device:aggregate-power"sv},
    {"/czechlight-coherent-add-drop:aggregate-power"sv},
    {"/czechlight-coherent-add-drop:client-ports/input-power"sv},
    {"/czechlight-inline-amp:east-to-west/optical-power"sv},
    {"/czechlight-inline-amp:west-to-east/optical-power"sv},
};
}

namespace rousette::sr {

OpticalEvents::OpticalEvents(std::shared_ptr<sysrepo::Session> session)
    : sub(std::make_shared<sysrepo::Subscribe>(session))
{
    session->session_switch_ds(SR_DS_OPERATIONAL);
    int subs = 0;
    for (const auto& mod : {"czechlight-roadm-device", "dummy-scan", "czechlight-coherent-add-drop", "czechlight-inline-amp"}) {
        try {
            sub->module_change_subscribe(mod, [this](const auto sess, const auto name, const auto, const auto, const auto) {
                        return onChange(sess, name);
                    }, nullptr, 0, SR_SUBSCR_CTX_REUSE | SR_SUBSCR_DONE_ONLY | SR_SUBSCR_PASSIVE);
            spdlog::debug("Listening for module {}", mod);
            ++subs;
        } catch (sysrepo::sysrepo_exception& e) {
            if (e.error_code() == SR_ERR_NOT_FOUND) {
                // nothing to listen for, just ignore this
                continue;
            }
            throw;
        }
    }
    if (!subs) {
        throw std::runtime_error{"No DWDM modules found"};
    }
}

int OpticalEvents::onChange(std::shared_ptr<sysrepo::Session> session, const std::string& module)
{
    assert(session->session_get_ds() == SR_DS_OPERATIONAL);
    spdlog::trace("change: {}", module);

    auto changes = session->get_changes_iter(("/" + module + ":*//.").c_str());
    bool shouldEmit = false;

    std::set<lyd_node*> seen;
    while (auto x = session->get_change_tree_next(changes)) {
        auto schemaPath = x->node()->schema()->path(LYS_PATH_FIRST_PREFIX);
        if (std::any_of(prefixes.begin(), prefixes.end(), [schemaPath](const auto prefix) {
                    if (prefix.size() > schemaPath.size())
                        return false;
                    return std::equal(prefix.begin(), prefix.end(), schemaPath.begin());
                })) {
            shouldEmit = true;
            break;
        } else {
        }
    }
    if (shouldEmit) {
        auto json = session->get_data(('/' + module + ":*").c_str())->print_mem(LYD_JSON, LYP_WITHSIBLINGS);
        spdlog::debug("change: {} bytes", json.size());
        change(json);
    }

    return SR_ERR_OK;
}
}

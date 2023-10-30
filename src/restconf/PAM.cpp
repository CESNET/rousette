/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/algorithm/string.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>
#include <boost/fusion/adapted/struct/adapt_struct.hpp>
#include <boost/spirit/home/x3.hpp>
#include <exception>
#include <security/pam_appl.h>
#include <spdlog/spdlog.h>
#include "restconf/PAM.h"

using namespace std::literals;

namespace rousette::auth {
namespace {

namespace x3 = boost::spirit::x3;

const auto base64Blob = x3::rule<class base64Blob, std::string>{"base64Blob"} = *(x3::alnum | x3::char_('+') | x3::char_('/')) >> x3::repeat(0, 2)[x3::lit('=')];
const auto headerGrammar = x3::no_skip[x3::no_case[x3::lit("basic")] >> x3::lit(' ') >> base64Blob];
const auto username = x3::rule<class username, std::string>{"username"} = +(x3::char_ - x3::lit(':'));
const auto password = x3::rule<class password, std::string>{"password"} = +x3::char_;
const auto userPass = x3::no_skip[username >> x3::lit(':') >> password];

std::string b64_decode(const std::string& val)
{
    using namespace boost::archive::iterators;
    using It = transform_width<binary_from_base64<std::string::const_iterator>, 8, 6>;
    return boost::algorithm::trim_right_copy_if(std::string(It(std::begin(val)), It(std::end(val))), [](char c) {
        return c == '\0';
    });
}

struct UserPass {
    std::string username;
    std::string password;
};

UserPass parseBasicAuth(const std::string& blob)
{
    std::string b64;
    auto iter = std::begin(blob);
    auto end = std::end(blob);

    if (!x3::parse(iter, end, headerGrammar >> x3::eoi, b64)) {
        throw Error{"Cannot parse the Basic authorization header"};
    }

    b64 = b64_decode(b64);

    iter = b64.begin();
    end = b64.end();

    UserPass res;
    if (!x3::parse(iter, end, userPass >> x3::eoi, res)) {
        throw Error{"Cannot parse the user-pass authorization blob"};
    }
    return res;
}

static int pam_userpass_conv(int num_msg, const struct pam_message** msg, struct pam_response** resp_r, void* appdata_ptr)
{
    const auto& userPass = *(const UserPass*)(appdata_ptr);

    auto release_response = [&num_msg](struct pam_response* resp) {
        for (int i = 0; i < num_msg; ++i) {
            free(resp[i].resp);
        }
        free(resp);
    };

    std::unique_ptr<struct pam_response, decltype(release_response)> resp{
        (pam_response*)calloc(num_msg, sizeof(struct pam_response)), release_response};

    for (int i = 0; i < num_msg; ++i) {
        char* str = nullptr;
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_ON:
            // assume that we're being asked about the username
            str = strdup(userPass.username.c_str());
            if (!str) {
                spdlog::critical("PAM: strdup(username) failed");
                return PAM_BUF_ERR;
            }
            break;
        case PAM_PROMPT_ECHO_OFF:
            // assume that this is the password
            str = strdup(userPass.password.c_str());
            if (!str) {
                spdlog::critical("PAM: strdup(password) failed");
                return PAM_BUF_ERR;
            }
            break;
        /* case PAM_ERROR_MSG: */
        /* case PAM_TEXT_INFO: */
        default:
            spdlog::critical("PAM: pam_userpass_conv: unexpected msg_style {}", msg[i]->msg_style);
            return PAM_CONV_ERR;
        }
        resp.get()[i].resp_retcode = PAM_SUCCESS;
        resp.get()[i].resp = str;
    }
    *resp_r = resp.release();
    return PAM_SUCCESS;
}

std::string authenticate_pam(const UserPass& userPass, const std::optional<std::filesystem::path>& dir, const std::optional<std::string>& remoteHost)
{
    pam_handle_t* pamh = nullptr;
    int res;
    pam_conv conv = {
        .conv = pam_userpass_conv,
        .appdata_ptr = (void*)(&userPass),
    };

    auto check = [&res, &pamh](const std::string& fun) {
        if (res != PAM_SUCCESS) {
            throw Error{"PAM: " + fun + ": " + pam_strerror(pamh, res)};
        }
    };

    auto pamh_deleter = [&res](pam_handle_t* pamh) {
        pam_end(pamh, res);
    };

    res = pam_start_confdir("rousette", userPass.username.c_str(), &conv, dir ? std::string(*dir).c_str() : nullptr, &pamh);
    std::unique_ptr<pam_handle_t, decltype(pamh_deleter)> guard{pamh, pamh_deleter};
    check("pam_start_confdir(" + (dir ? std::string(*dir) : std::string{}) + ")");

    if (remoteHost) {
        res = pam_set_item(pamh, PAM_RHOST, remoteHost->c_str());
        check("pam_set_item(PAM_RHOST)");
    }

    res = pam_authenticate(pamh, 0);
    check("pam_authenticate");

    res = pam_acct_mgmt(pamh, 0);
    check("pam_acct_mgmt");

    const void* item;
    res = pam_get_item(pamh, PAM_USER, &item);
    check("pam_get_item(PAM_USER)");
    return (const char*)(item);
}
}

std::string authenticate_pam(const std::string& blob, const std::optional<std::filesystem::path>& pamConfigDir, const std::optional<std::string>& remoteHost)
{
    return authenticate_pam(parseBasicAuth(blob), pamConfigDir, remoteHost);
}
}

BOOST_FUSION_ADAPT_STRUCT(rousette::auth::UserPass, username, password);

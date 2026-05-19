#pragma once
#include "restconf_utils.h"

Response get(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "GET", uri, "", headers, timeout);
}

Response options(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "OPTIONS", uri, "", headers, timeout);
}

Response head(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "HEAD", uri, "", headers, timeout);
}

Response put(auto xpath, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "PUT", xpath, data, headers, timeout);
}

Response post(auto xpath, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "POST", xpath, data, headers, timeout);
}

Response patch(auto uri, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "PATCH", uri, data, headers, timeout);
}

Response httpDelete(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "DELETE", uri, "", headers, timeout);
}

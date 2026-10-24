#ifndef TODOIST_SYNC_TRANSPORT_INTERFACE_HPP
#define TODOIST_SYNC_TRANSPORT_INTERFACE_HPP

#include <string>

namespace todoist {

class ISyncTransport {
public:
    virtual ~ISyncTransport() {}

    struct HttpResponse {
        int statusCode;
        std::string body;
        std::string errorMessage;

        HttpResponse()
            : statusCode(0)
            , body("")
            , errorMessage("")
        {}

        HttpResponse(int code, const std::string& responseBody, const std::string& error = "")
            : statusCode(code)
            , body(responseBody)
            , errorMessage(error)
        {}
    };

    // Synchronous HTTP POST to the Todoist sync endpoint
    virtual HttpResponse executeSyncPost(const std::string& endpointUrl,
                                        const std::string& requestBody,
                                        const std::string& bearerToken) = 0;
};

} // namespace todoist

#endif // TODOIST_SYNC_TRANSPORT_INTERFACE_HPP

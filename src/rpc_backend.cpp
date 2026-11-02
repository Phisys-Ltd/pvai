#include "rpc_backend.h"

#include <poll.h>

namespace pvai {

RpcBackend::RpcBackend(Host &host)
    : Backend(host)
{
}

void RpcBackend::findProgram()
{
    if (bin_.empty())
        bin_ = locate();
}

short RpcBackend::events() const
{
    return static_cast<short>(POLLIN | (link().wantsWrite() ? POLLOUT : 0));
}

void RpcBackend::call(const std::string &method, const json &params, Done done)
{
    if (!link().isOpen()) {
        if (done)
            done(json::object(), {{"code", -32603}, {"message", title() + " is not running"}});
        return;
    }
    const long long id = link().request(method, params);
    if (done)
        pending_[id] = std::move(done);
}

const json &RpcBackend::objectIn(const json &message, const char *key)
{
    static const json empty = json::object();
    const auto it = message.find(key);
    return it != message.end() && it->is_object() ? *it : empty;
}

void RpcBackend::dispatchResponse(const json &message)
{
    const json id = message.contains("id") ? message["id"] : json();
    const auto it = id.is_number_integer() ? pending_.find(id.get<long long>()) : pending_.end();
    if (it == pending_.end())
        return;
    auto done = std::move(it->second);
    pending_.erase(it);
    const json error = message.contains("error") && !message["error"].is_null() ? objectIn(message, "error") : json();
    done(objectIn(message, "result"), error);
}

bool RpcBackend::retryDue(Clock::time_point now)
{
    if (now < nextTry_)
        return false;
    nextTry_ = now + kRetry;
    return true;
}

void RpcBackend::initialize(const json &params)
{
    call("initialize", params, [this](const json &result, const json &error) {
        if (!error.is_null()) {
            initializeFailed(error);
            lost();
            return;
        }
        ready_ = true;
        initialized(result);
    });
}

void RpcBackend::lost()
{
    link().close();
    ready_ = false;
    // The app hears for each start or conversation request that it failed.
    // (An answer that asks again meets the same end.)
    while (!pending_.empty()) {
        auto pending = std::move(pending_);
        pending_.clear();
        for (auto &[id, done] : pending)
            done(json::object(), {{"code", -32603}, {"message", endedText()}});
    }
    onLost();
    nextTry_ = Clock::now() + std::chrono::seconds(2);
}

} // namespace pvai

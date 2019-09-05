#include "redpanda/kafka/requests/find_coordinator_request.h"

#include "model/metadata.h"
#include "redpanda/kafka/errors/errors.h"

#include <seastar/util/log.hh>

#include <string_view>

namespace kafka::requests {

future<response_ptr>
find_coordinator_request::process(request_context& ctx, smp_service_group g) {
    if (
      ctx.header().version < min_supported
      || ctx.header().version > max_supported) {
        return make_exception_future<response_ptr>(
          std::runtime_error(fmt::format(
            "Unsupported version {} for metadata API", ctx.header().version)));
    }
    // request
    auto key = ctx.reader().read_string();
    std::optional<int8_t> key_type;
    if (ctx.header().version >= api_version(1)) {
        key_type = ctx.reader().read_int8();
    }
    // response
    auto resp = std::make_unique<response>();
    if (ctx.header().version >= api_version(1)) {
        resp->writer().write(int32_t(0));
    }
    resp->writer().write(errors::error_code::none);
    if (ctx.header().version >= api_version(1)) {
        std::optional<std::string_view> error_message;
        resp->writer().write(error_message);
    }
    // TODO: return connection for raft group 0 leader
    resp->writer().write(int32_t(0));
    resp->writer().write("localhost");
    resp->writer().write(int32_t(9092));
    return make_ready_future<response_ptr>(std::move(resp));
}

} // namespace kafka::requests

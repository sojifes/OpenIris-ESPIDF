#include "RestAPI.hpp"

#include <utility>

#define PATCH_METHOD "PATCH"
#define POST_METHOD "POST"
#define GET_METHOD "GET"
#define DELETE_METHOD "DELETE"

// Mongoose has its own logging system that prints raw bytes with a custom
// format ("c55c   3 mongoose.c:7956:accept_conn   ...") which doesn't blend
// with ESP-IDF's ESP_LOG output. Bridge mongoose logging into ESP_LOG so the
// serial console stays consistent. Mongoose calls the sink one char at a
// time, so we buffer until newline. Mongoose runs single-threaded on the
// REST poll task, so the static buffer needs no synchronization.
namespace
{
constexpr size_t MG_LOG_BUF_SIZE = 256;
char s_mg_log_buf[MG_LOG_BUF_SIZE];
size_t s_mg_log_len = 0;

void mg_log_to_esp(char ch, void* /*param*/)
{
    if (ch == '\n' || ch == '\r' || s_mg_log_len >= MG_LOG_BUF_SIZE - 1)
    {
        if (s_mg_log_len > 0)
        {
            s_mg_log_buf[s_mg_log_len] = '\0';
            ESP_LOGI("MONGOOSE", "%s", s_mg_log_buf);
            s_mg_log_len = 0;
        }
        return;
    }
    s_mg_log_buf[s_mg_log_len++] = ch;
}
}  // namespace

bool getIsSuccess(const nlohmann::json& response)
{
    // since the commandManager will be returning CommandManagerResponse to simplify parsing on the clients end
    // we can slightly its json representation, and extract the status from there
    // note: This will only work for commands executed with CommandManager::executeFromType().
    if (!response.contains("result"))
    {
        return false;
    }

    return response.at("result").at("status").get<std::string>() == "success";
}

RestAPI::RestAPI(std::string url, std::shared_ptr<CommandManager> commandManager) : command_manager(commandManager)
{
    // until we stumble on a simpler way to handle the commands over the rest api
    // the formula will be like this:
    // each command gets its own endpoint
    // each endpoint must include the action it performs in its path
    // for example
    // /get/ for getters
    // /set/ for posts
    // /delete/ for deletes
    // /update/ for updates
    // additional actions on the resource should be appended after the resource name
    // like for example /api/set/config/save/
    //
    // one endpoint must not contain more than one action

    this->url = std::move(url);
    // updates via PATCH
    routes.emplace("/api/update/wifi/", RequestBaseData(PATCH_METHOD, CommandType::UPDATE_WIFI, 200, 400));
    routes.emplace("/api/update/device/mode/", RequestBaseData(PATCH_METHOD, CommandType::SWITCH_MODE, 200, 400));
    routes.emplace("/api/update/camera/", RequestBaseData(PATCH_METHOD, CommandType::UPDATE_CAMERA, 200, 400));
    routes.emplace("/api/update/ota/credentials", RequestBaseData(PATCH_METHOD, CommandType::UPDATE_OTA_CREDENTIALS, 200, 400));
    routes.emplace("/api/update/ap/", RequestBaseData(PATCH_METHOD, CommandType::UPDATE_AP_WIFI, 200, 400));
    routes.emplace("/api/update/led_duty_cycle/", RequestBaseData(PATCH_METHOD, CommandType::SET_LED_DUTY_CYCLE, 200, 400));

    // POST will set the data
    routes.emplace("/api/set/pause/", RequestBaseData(POST_METHOD, CommandType::PAUSE, 200, 400));
    routes.emplace("/api/set/wifi/", RequestBaseData(POST_METHOD, CommandType::SET_WIFI, 200, 400));
    routes.emplace("/api/set/mdns/", RequestBaseData(POST_METHOD, CommandType::SET_MDNS, 200, 400));
    routes.emplace("/api/set/config/save/", RequestBaseData(POST_METHOD, CommandType::SAVE_CONFIG, 200, 400));
    routes.emplace("/api/set/wifi/connect/", RequestBaseData(POST_METHOD, CommandType::CONNECT_WIFI, 200, 400));

    // resets via POST as well
    routes.emplace("/api/reset/config/", RequestBaseData(POST_METHOD, CommandType::RESET_CONFIG, 200, 400));

    // gets via GET
    routes.emplace("/api/get/config/", RequestBaseData(GET_METHOD, CommandType::GET_CONFIG, 200, 400));
    routes.emplace("/api/get/mdns/", RequestBaseData(GET_METHOD, CommandType::GET_MDNS_NAME, 200, 400));
    routes.emplace("/api/get/led_duty_cycle/", RequestBaseData(GET_METHOD, CommandType::GET_LED_DUTY_CYCLE, 200, 400));
    routes.emplace("/api/get/serial_number/", RequestBaseData(GET_METHOD, CommandType::GET_SERIAL, 200, 400));
    routes.emplace("/api/get/led_current/", RequestBaseData(GET_METHOD, CommandType::GET_LED_CURRENT, 200, 400));
    routes.emplace("/api/get/battery/", RequestBaseData(GET_METHOD, CommandType::GET_BATTERY_STATUS, 200, 400));
    routes.emplace("/api/get/who_am_i/", RequestBaseData(GET_METHOD, CommandType::GET_WHO_AM_I, 200, 400));

    // deletes via DELETE
    routes.emplace("/api/delete/wifi", RequestBaseData(DELETE_METHOD, CommandType::DELETE_NETWORK, 200, 400));

    // reboots via POST
    routes.emplace("/api/reboot/device/", RequestBaseData(GET_METHOD, CommandType::RESTART_DEVICE, 200, 500));

    // heartbeat via GET
    routes.emplace("/api/ping/", RequestBaseData(GET_METHOD, CommandType::PING, 200, 400));
}

void RestAPI::begin()
{
    // Route mongoose log output through ESP_LOG so it shares the project's
    // standard timestamp / tag format. Lower the level from DEBUG (per-byte
    // read/write spam) to ERROR — accept/close events become invisible at
    // steady state, but real failures still surface with consistent format.
    mg_log_set_fn(mg_log_to_esp, nullptr);
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&mgr);
    // every route is handled through this class, with commands themselves by a command manager
    // hence we pass a pointer to this in mg_http_listen
    mg_http_listen(&mgr, this->url.c_str(), (mg_event_handler_t)RestAPIHelpers::event_handler, this);
}

void RestAPI::handle_request(struct mg_connection* connection, int event, void* event_data)
{
    if (event == MG_EV_HTTP_MSG)
    {
        auto const* message = static_cast<struct mg_http_message*>(event_data);
        auto const uri = std::string(message->uri.buf, message->uri.len);

        if (this->routes.find(uri) == this->routes.end())
        {
            mg_http_reply(connection, 404, "", "Wrong URL");
            return;
        }

        auto const base_request_params = this->routes.at(uri);

        auto* context = new RequestContext{
            .connection = connection,
            .method = std::string(message->method.buf, message->method.len),
            .body = std::string(message->body.buf, message->body.len),
        };
        this->handle_endpoint_command(context, base_request_params.allowed_method, base_request_params.command_type, base_request_params.success_code,
                                      base_request_params.error_code);
    }
}

void RestAPIHelpers::event_handler(struct mg_connection* connection, int event, void* event_data)
{
    auto* rest_api_handler = static_cast<RestAPI*>(connection->fn_data);
    rest_api_handler->handle_request(connection, event, event_data);
}

void RestAPI::poll()
{
    mg_mgr_poll(&mgr, 100);
}

void HandleRestAPIPollTask(void* pvParameter)
{
    auto* rest_api_handler = static_cast<RestAPI*>(pvParameter);
    while (true)
    {
        rest_api_handler->poll();
        vTaskDelay(1000);
    }
}

void RestAPI::handle_endpoint_command(RequestContext* context, std::string allowed_method, CommandType command_type, int success_code, int error_code)
{
    if (context->method != allowed_method)
    {
        mg_http_reply(context->connection, 401, JSON_RESPONSE, "{%m:%m}", MG_ESC("error"), "Method not allowed");
        return;
    }

    const nlohmann::json result = command_manager->executeFromType(command_type, context->body);
    const auto code = getIsSuccess(result) ? success_code : error_code;
    mg_http_reply(context->connection, code, JSON_RESPONSE, result.dump().c_str());
}
#include "hakoniwa/pdu/action/c_action.h"

#include "hakoniwa/pdu/action/action_services_client.hpp"
#include "hakoniwa/pdu/action/action_services_mux_server.hpp"
#include "hakoniwa/pdu/action/action_services_server.hpp"
#include "hakoniwa/pdu/endpoint_container.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace action = hakoniwa::pdu::action;
using hakoniwa::pdu::EndpointContainer;

namespace {

constexpr const char* kClientImpl = "ActionClientEndpointImpl";
constexpr const char* kServerImpl = "ActionServerEndpointImpl";
constexpr const char* kDefaultTimeSource = "real";

bool valid_text(const char* value)
{
    return value != nullptr && value[0] != '\0';
}

void copy_name(char* destination, std::size_t capacity, const std::string& source)
{
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const auto count = std::min(capacity - 1, source.size());
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

action::PduData make_pdu(const std::uint8_t* data, std::size_t size)
{
    return size == 0 ? action::PduData{} : action::PduData(data, data + size);
}

hako_pdu_action_error_t copy_pdu(
    const action::PduData& source,
    std::uint8_t* destination,
    std::size_t capacity,
    std::size_t* out_size)
{
    if (out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    *out_size = source.size();
    if (source.size() > capacity
        || (!source.empty() && destination == nullptr)) {
        return HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL;
    }
    if (!source.empty()) {
        std::memcpy(destination, source.data(), source.size());
    }
    return HAKO_PDU_ACTION_OK;
}

hako_pdu_action_error_t allocate_pdu(
    const action::PduData& source,
    std::uint8_t** out_buffer,
    std::size_t* out_size)
{
    if (out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    *out_buffer = nullptr;
    *out_size = 0;
    if (source.empty()) {
        return HAKO_PDU_ACTION_OK;
    }
    auto* buffer = static_cast<std::uint8_t*>(std::malloc(source.size()));
    if (buffer == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
    std::memcpy(buffer, source.data(), source.size());
    *out_buffer = buffer;
    *out_size = source.size();
    return HAKO_PDU_ACTION_OK;
}

action::GoalId to_cpp_goal_id(const hako_pdu_action_goal_id_t& source)
{
    action::GoalId goal_id{};
    std::copy_n(source.bytes, goal_id.size(), goal_id.begin());
    return goal_id;
}

void to_c_goal_id(
    const action::GoalId& source,
    hako_pdu_action_goal_id_t& destination)
{
    std::copy(source.begin(), source.end(), destination.bytes);
}

action::ClientGoalHandle to_cpp_goal(
    const hako_pdu_action_client_goal_handle_t& source)
{
    return action::ClientGoalHandle{to_cpp_goal_id(source.goal_id)};
}

action::ServerGoalHandle to_cpp_goal(
    const hako_pdu_action_server_goal_handle_t& source)
{
    return action::ServerGoalHandle{to_cpp_goal_id(source.goal_id)};
}

hako_pdu_action_client_event_t map_client_event(action::ClientEventType event)
{
    switch (event) {
    case action::ClientEventType::GOAL_RESPONSE:
        return HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE;
    case action::ClientEventType::FEEDBACK:
        return HAKO_PDU_ACTION_CLIENT_EVENT_FEEDBACK;
    case action::ClientEventType::CANCEL_RESPONSE:
        return HAKO_PDU_ACTION_CLIENT_EVENT_CANCEL_RESPONSE;
    case action::ClientEventType::RESULT:
        return HAKO_PDU_ACTION_CLIENT_EVENT_RESULT;
    case action::ClientEventType::TIMEOUT:
        return HAKO_PDU_ACTION_CLIENT_EVENT_TIMEOUT;
    case action::ClientEventType::ERROR:
        return HAKO_PDU_ACTION_CLIENT_EVENT_ERROR;
    case action::ClientEventType::NONE:
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }
    return HAKO_PDU_ACTION_CLIENT_EVENT_ERROR;
}

hako_pdu_action_server_event_t map_server_event(action::ServerEventType event)
{
    switch (event) {
    case action::ServerEventType::GOAL_REQUEST:
        return HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST;
    case action::ServerEventType::CANCEL_REQUEST:
        return HAKO_PDU_ACTION_SERVER_EVENT_CANCEL_REQUEST;
    case action::ServerEventType::RUNTIME_CANCEL_REQUEST:
        return HAKO_PDU_ACTION_SERVER_EVENT_RUNTIME_CANCEL_REQUEST;
    case action::ServerEventType::ERROR:
        return HAKO_PDU_ACTION_SERVER_EVENT_ERROR;
    case action::ServerEventType::NONE:
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    return HAKO_PDU_ACTION_SERVER_EVENT_ERROR;
}

action::TerminalStatus to_cpp_status(
    hako_pdu_action_terminal_status_t status)
{
    switch (status) {
    case HAKO_PDU_ACTION_TERMINAL_SUCCEEDED:
        return action::TerminalStatus::SUCCEEDED;
    case HAKO_PDU_ACTION_TERMINAL_CANCELED:
        return action::TerminalStatus::CANCELED;
    case HAKO_PDU_ACTION_TERMINAL_ABORTED:
        return action::TerminalStatus::ABORTED;
    case HAKO_PDU_ACTION_TERMINAL_UNSPECIFIED:
        return action::TerminalStatus::UNSPECIFIED;
    }
    return action::TerminalStatus::UNSPECIFIED;
}

hako_pdu_action_error_t map_goal_send_result(action::GoalSendResult result)
{
    switch (result) {
    case action::GoalSendResult::SUCCESS:
        return HAKO_PDU_ACTION_OK;
    case action::GoalSendResult::INVALID_ARGUMENT:
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    case action::GoalSendResult::ACTION_NOT_FOUND:
        return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
    case action::GoalSendResult::NOT_INITIALIZED:
        return HAKO_PDU_ACTION_ERROR_INITIALIZE;
    case action::GoalSendResult::DUPLICATE_GOAL:
        return HAKO_PDU_ACTION_ERROR_DUPLICATE_GOAL;
    case action::GoalSendResult::NO_FREE_SLOT:
        return HAKO_PDU_ACTION_ERROR_NO_FREE_SLOT;
    case action::GoalSendResult::INVALID_PACKET:
        return HAKO_PDU_ACTION_ERROR_INVALID_PACKET;
    case action::GoalSendResult::TRANSPORT_ERROR:
        return HAKO_PDU_ACTION_ERROR_SEND;
    }
    return HAKO_PDU_ACTION_ERROR_INTERNAL;
}

} // namespace

struct hako_pdu_action_client_handle {
    std::shared_ptr<EndpointContainer> endpoints;
    std::unique_ptr<action::ActionServicesClient> services;
    std::mutex poll_mutex;
    std::optional<std::pair<std::string, action::ClientEvent>> pending_event;
    bool started{false};
};

struct hako_pdu_action_server_handle {
    std::shared_ptr<EndpointContainer> endpoints;
    std::unique_ptr<action::ActionServicesServer> services;
    std::mutex poll_mutex;
    std::optional<std::pair<std::string, action::ServerEvent>> pending_event;
    bool started{false};
};

struct hako_pdu_action_mux_server_handle {
    std::unique_ptr<action::ActionServicesMuxServer> services;
    std::mutex poll_mutex;
    std::optional<std::pair<std::string, action::ServerEvent>> pending_event;
    bool started{false};
};

namespace {

bool stop_client(hako_pdu_action_client_handle_t* handle) noexcept
{
    if (handle == nullptr) {
        return true;
    }
    bool success = true;
    try {
        if (handle->endpoints
            && handle->endpoints->stop_all() != HAKO_PDU_ERR_OK) {
            success = false;
        }
    } catch (...) {
        success = false;
    }
    try {
        if (handle->services) {
            handle->services->stop_all_services();
            handle->services->clear_all_instances();
        }
    } catch (...) {
        success = false;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        handle->pending_event.reset();
    } catch (...) {
        success = false;
    }
    handle->started = false;
    return success;
}

bool stop_server(hako_pdu_action_server_handle_t* handle) noexcept
{
    if (handle == nullptr) {
        return true;
    }
    bool success = true;
    try {
        if (handle->endpoints
            && handle->endpoints->stop_all() != HAKO_PDU_ERR_OK) {
            success = false;
        }
    } catch (...) {
        success = false;
    }
    try {
        if (handle->services) {
            handle->services->stop_all_services();
            handle->services->clear_all_instances();
        }
    } catch (...) {
        success = false;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        handle->pending_event.reset();
    } catch (...) {
        success = false;
    }
    handle->started = false;
    return success;
}

bool stop_mux_server(hako_pdu_action_mux_server_handle_t* handle) noexcept
{
    if (handle == nullptr) {
        return true;
    }
    bool success = true;
    try {
        if (handle->services) {
            handle->services->stop();
        }
    } catch (...) {
        success = false;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        handle->pending_event.reset();
    } catch (...) {
        success = false;
    }
    handle->started = false;
    return success;
}

void fill_client_info(
    const std::string& action_name,
    const action::ClientEvent& event,
    hako_pdu_action_client_event_info_t& info)
{
    std::memset(&info, 0, sizeof(info));
    copy_name(info.action_name, sizeof(info.action_name), action_name);
    to_c_goal_id(event.goal.goal_id, info.goal.goal_id);
    info.decision = static_cast<hako_pdu_action_decision_t>(event.decision);
    info.terminal_status =
        static_cast<hako_pdu_action_terminal_status_t>(event.terminal_status);
    info.feedback_sequence = event.feedback_sequence;
    info.pdu_size = event.pdu.size();
}

void fill_server_info(
    const std::string& action_name,
    const action::ServerEvent& event,
    hako_pdu_action_server_event_info_t& info)
{
    std::memset(&info, 0, sizeof(info));
    copy_name(info.action_name, sizeof(info.action_name), action_name);
    to_c_goal_id(event.goal.goal_id, info.goal.goal_id);
    info.runtime_cancel_cause =
        static_cast<hako_pdu_action_runtime_cancel_cause_t>(
            event.runtime_cancel_cause);
    info.pdu_size = event.pdu.size();
}

} // namespace

extern "C" {

void hako_pdu_action_buffer_free(std::uint8_t* buffer)
{
    std::free(buffer);
}

hako_pdu_action_client_handle_t* hako_pdu_action_client_create(
    const char* node_id,
    const char* client_name,
    const char* action_config_path,
    const char* endpoint_config_path,
    std::uint64_t delta_time_usec,
    const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(client_name)
        || !valid_text(action_config_path)
        || !valid_text(endpoint_config_path)) {
        return nullptr;
    }
    try {
        auto handle = std::make_unique<hako_pdu_action_client_handle_t>();
        handle->endpoints = std::make_shared<EndpointContainer>(
            node_id, endpoint_config_path);
        if (handle->endpoints->initialize() != HAKO_PDU_ERR_OK) {
            return nullptr;
        }
        handle->services = std::make_unique<action::ActionServicesClient>(
            node_id,
            client_name,
            action_config_path,
            kClientImpl,
            delta_time_usec,
            valid_text(time_source_type) ? time_source_type : kDefaultTimeSource);
        if (!handle->services->initialize_services(handle->endpoints)) {
            return nullptr;
        }
        return handle.release();
    } catch (...) {
        return nullptr;
    }
}

void hako_pdu_action_client_destroy(hako_pdu_action_client_handle_t* handle)
{
    if (handle == nullptr) {
        return;
    }
    (void)stop_client(handle);
    delete handle;
}

hako_pdu_action_error_t hako_pdu_action_client_start(
    hako_pdu_action_client_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (handle->started) {
        return HAKO_PDU_ACTION_OK;
    }
    if (!handle->endpoints || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_INITIALIZE;
    }
    try {
        if (handle->endpoints->start_all() != HAKO_PDU_ERR_OK) {
            return HAKO_PDU_ACTION_ERROR_START;
        }
        if (!handle->services->start_all_services()) {
            (void)handle->endpoints->stop_all();
            return HAKO_PDU_ACTION_ERROR_START;
        }
        handle->started = true;
        return HAKO_PDU_ACTION_OK;
    } catch (...) {
        (void)stop_client(handle);
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_client_stop(
    hako_pdu_action_client_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    return stop_client(handle)
        ? HAKO_PDU_ACTION_OK
        : HAKO_PDU_ACTION_ERROR_INTERNAL;
}

hako_pdu_action_error_t hako_pdu_action_client_is_running(
    hako_pdu_action_client_handle_t* handle,
    int* out_running)
{
    if (handle == nullptr || out_running == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        *out_running = handle->started && handle->endpoints
            && handle->endpoints->is_running_all();
        return HAKO_PDU_ACTION_OK;
    } catch (...) {
        *out_running = 0;
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_client_create_goal_buffer(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size)
{
    if (handle == nullptr || !valid_text(action_name) || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_goal_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return copy_pdu(pdu, buffer, capacity, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_client_create_goal_buffer_alloc(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    std::uint8_t** out_buffer,
    std::size_t* out_size)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (handle == nullptr || !valid_text(action_name)
        || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_goal_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return allocate_pdu(pdu, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_client_send_goal(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const std::uint8_t* pdu,
    std::size_t pdu_size,
    const hako_pdu_action_goal_id_t* goal_id,
    hako_pdu_action_client_goal_handle_t* out_goal,
    std::uint64_t timeout_usec)
{
    if (out_goal != nullptr) {
        std::memset(out_goal, 0, sizeof(*out_goal));
    }
    if (handle == nullptr || !valid_text(action_name) || pdu == nullptr
        || pdu_size == 0 || goal_id == nullptr || out_goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal_id = to_cpp_goal_id(*goal_id);
    if (!action::is_valid_goal_id(native_goal_id)) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        action::ClientGoalHandle native_goal;
        const auto result = handle->services->send_goal_with_result(
                action_name,
                make_pdu(pdu, pdu_size),
                native_goal_id,
                native_goal,
                timeout_usec);
        if (result != action::GoalSendResult::SUCCESS) {
            return map_goal_send_result(result);
        }
        to_c_goal_id(native_goal.goal_id, out_goal->goal_id);
        return HAKO_PDU_ACTION_OK;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_client_cancel_goal(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_client_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->send_cancel(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_client_event_t hako_pdu_action_client_poll(
    hako_pdu_action_client_handle_t* handle,
    hako_pdu_action_client_event_info_t* out_info,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size,
    hako_pdu_action_error_t* out_error)
{
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_ACTION_OK;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (out_info != nullptr) {
        std::memset(out_info, 0, sizeof(*out_info));
    }
    if (handle == nullptr || out_info == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }
    if (!handle->started || !handle->services) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        if (!handle->pending_event.has_value()) {
            std::string action_name;
            action::ClientEvent event;
            const auto type = handle->services->poll(action_name, event);
            if (type == action::ClientEventType::NONE) {
                return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
            }
            event.type = type;
            handle->pending_event.emplace(
                std::move(action_name), std::move(event));
        }
        const auto& action_name = handle->pending_event->first;
        const auto& event = handle->pending_event->second;
        const auto type = event.type;
        fill_client_info(action_name, event, *out_info);
        const auto result = copy_pdu(event.pdu, buffer, capacity, out_size);
        if (result != HAKO_PDU_ACTION_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
        }
        if (type == action::ClientEventType::ERROR && out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        handle->pending_event.reset();
        return map_client_event(type);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }
}

hako_pdu_action_client_event_t hako_pdu_action_client_poll_alloc(
    hako_pdu_action_client_handle_t* handle,
    hako_pdu_action_client_event_info_t* out_info,
    std::uint8_t** out_buffer,
    std::size_t* out_size,
    hako_pdu_action_error_t* out_error)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_ACTION_OK;
    }
    if (out_info != nullptr) {
        std::memset(out_info, 0, sizeof(*out_info));
    }
    if (handle == nullptr || out_info == nullptr
        || out_buffer == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }
    if (!handle->started || !handle->services) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        if (!handle->pending_event.has_value()) {
            std::string action_name;
            action::ClientEvent event;
            const auto type = handle->services->poll(action_name, event);
            if (type == action::ClientEventType::NONE) {
                return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
            }
            event.type = type;
            handle->pending_event.emplace(
                std::move(action_name), std::move(event));
        }
        const auto& action_name = handle->pending_event->first;
        const auto& event = handle->pending_event->second;
        const auto type = event.type;
        fill_client_info(action_name, event, *out_info);
        const auto result = allocate_pdu(event.pdu, out_buffer, out_size);
        if (result != HAKO_PDU_ACTION_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
        }
        if (type == action::ClientEventType::ERROR && out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        handle->pending_event.reset();
        return map_client_event(type);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        return HAKO_PDU_ACTION_CLIENT_EVENT_NONE;
    }
}

hako_pdu_action_server_handle_t* hako_pdu_action_server_create(
    const char* node_id,
    const char* action_config_path,
    const char* endpoint_config_path,
    std::uint64_t delta_time_usec,
    const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(action_config_path)
        || !valid_text(endpoint_config_path)) {
        return nullptr;
    }
    try {
        auto handle = std::make_unique<hako_pdu_action_server_handle_t>();
        handle->endpoints = std::make_shared<EndpointContainer>(
            node_id, endpoint_config_path);
        if (handle->endpoints->initialize() != HAKO_PDU_ERR_OK) {
            return nullptr;
        }
        handle->services = std::make_unique<action::ActionServicesServer>(
            node_id,
            action_config_path,
            kServerImpl,
            delta_time_usec,
            valid_text(time_source_type) ? time_source_type : kDefaultTimeSource);
        if (!handle->services->initialize_services(handle->endpoints)) {
            return nullptr;
        }
        return handle.release();
    } catch (...) {
        return nullptr;
    }
}

void hako_pdu_action_server_destroy(hako_pdu_action_server_handle_t* handle)
{
    if (handle == nullptr) {
        return;
    }
    (void)stop_server(handle);
    delete handle;
}

hako_pdu_action_error_t hako_pdu_action_server_start(
    hako_pdu_action_server_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (handle->started) {
        return HAKO_PDU_ACTION_OK;
    }
    if (!handle->endpoints || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_INITIALIZE;
    }
    try {
        if (handle->endpoints->start_all() != HAKO_PDU_ERR_OK) {
            return HAKO_PDU_ACTION_ERROR_START;
        }
        if (!handle->services->start_all_services()) {
            (void)handle->endpoints->stop_all();
            return HAKO_PDU_ACTION_ERROR_START;
        }
        handle->started = true;
        return HAKO_PDU_ACTION_OK;
    } catch (...) {
        (void)stop_server(handle);
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_stop(
    hako_pdu_action_server_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    return stop_server(handle)
        ? HAKO_PDU_ACTION_OK
        : HAKO_PDU_ACTION_ERROR_INTERNAL;
}

hako_pdu_action_error_t hako_pdu_action_server_is_running(
    hako_pdu_action_server_handle_t* handle,
    int* out_running)
{
    if (handle == nullptr || out_running == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        *out_running = handle->started && handle->endpoints
            && handle->endpoints->is_running_all();
        return HAKO_PDU_ACTION_OK;
    } catch (...) {
        *out_running = 0;
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_server_event_t hako_pdu_action_server_poll(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_server_event_info_t* out_info,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size,
    hako_pdu_action_error_t* out_error)
{
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_ACTION_OK;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (out_info != nullptr) {
        std::memset(out_info, 0, sizeof(*out_info));
    }
    if (handle == nullptr || out_info == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    if (!handle->started || !handle->services) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        if (!handle->pending_event.has_value()) {
            std::string action_name;
            action::ServerEvent event;
            const auto type = handle->services->poll(action_name, event);
            if (type == action::ServerEventType::NONE) {
                return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
            }
            event.type = type;
            handle->pending_event.emplace(
                std::move(action_name), std::move(event));
        }
        const auto& action_name = handle->pending_event->first;
        const auto& event = handle->pending_event->second;
        const auto type = event.type;
        fill_server_info(action_name, event, *out_info);
        const auto result = copy_pdu(event.pdu, buffer, capacity, out_size);
        if (result != HAKO_PDU_ACTION_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
        }
        if (type == action::ServerEventType::ERROR && out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        handle->pending_event.reset();
        return map_server_event(type);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
}

hako_pdu_action_server_event_t hako_pdu_action_server_poll_alloc(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_server_event_info_t* out_info,
    std::uint8_t** out_buffer,
    std::size_t* out_size,
    hako_pdu_action_error_t* out_error)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_ACTION_OK;
    }
    if (out_info != nullptr) {
        std::memset(out_info, 0, sizeof(*out_info));
    }
    if (handle == nullptr || out_info == nullptr
        || out_buffer == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    if (!handle->started || !handle->services) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        if (!handle->pending_event.has_value()) {
            std::string action_name;
            action::ServerEvent event;
            const auto type = handle->services->poll(action_name, event);
            if (type == action::ServerEventType::NONE) {
                return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
            }
            event.type = type;
            handle->pending_event.emplace(
                std::move(action_name), std::move(event));
        }
        const auto& action_name = handle->pending_event->first;
        const auto& event = handle->pending_event->second;
        const auto type = event.type;
        fill_server_info(action_name, event, *out_info);
        const auto result = allocate_pdu(event.pdu, out_buffer, out_size);
        if (result != HAKO_PDU_ACTION_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
        }
        if (type == action::ServerEventType::ERROR && out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        handle->pending_event.reset();
        return map_server_event(type);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_accept_goal(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->accept_goal(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_reject_goal(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->reject_goal(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_accept_cancel(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->accept_cancel(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_reject_cancel(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->reject_cancel(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_create_feedback_buffer(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size)
{
    if (handle == nullptr || !valid_text(action_name) || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_feedback_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return copy_pdu(pdu, buffer, capacity, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_create_feedback_buffer_alloc(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    std::uint8_t** out_buffer,
    std::size_t* out_size)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (handle == nullptr || !valid_text(action_name)
        || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_feedback_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return allocate_pdu(pdu, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_create_result_buffer(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size)
{
    if (handle == nullptr || !valid_text(action_name) || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_result_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return copy_pdu(pdu, buffer, capacity, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_create_result_buffer_alloc(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    std::uint8_t** out_buffer,
    std::size_t* out_size)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (handle == nullptr || !valid_text(action_name)
        || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_result_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return allocate_pdu(pdu, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_send_feedback(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal,
    const std::uint8_t* pdu,
    std::size_t pdu_size)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr
        || pdu == nullptr || pdu_size == 0) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->send_feedback(
                   action_name, native_goal, make_pdu(pdu, pdu_size))
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_server_complete(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal,
    hako_pdu_action_terminal_status_t status,
    const std::uint8_t* pdu,
    std::size_t pdu_size)
{
    const bool valid_status = status == HAKO_PDU_ACTION_TERMINAL_SUCCEEDED
        || status == HAKO_PDU_ACTION_TERMINAL_CANCELED
        || status == HAKO_PDU_ACTION_TERMINAL_ABORTED;
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr
        || pdu == nullptr || pdu_size == 0 || !valid_status) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->complete(
                   action_name,
                   native_goal,
                   to_cpp_status(status),
                   make_pdu(pdu, pdu_size))
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_mux_server_handle_t* hako_pdu_action_mux_server_create(
    const char* node_id,
    const char* action_config_path,
    const char* endpoint_mux_config_path,
    std::uint64_t delta_time_usec,
    const char* time_source_type)
{
    if (!valid_text(node_id) || !valid_text(action_config_path)
        || !valid_text(endpoint_mux_config_path) || delta_time_usec == 0) {
        return nullptr;
    }
    try {
        auto handle = std::make_unique<hako_pdu_action_mux_server_handle_t>();
        handle->services = std::make_unique<action::ActionServicesMuxServer>(
            node_id,
            action_config_path,
            endpoint_mux_config_path,
            kServerImpl,
            delta_time_usec,
            valid_text(time_source_type) ? time_source_type : kDefaultTimeSource);
        if (!handle->services->initialize()) {
            return nullptr;
        }
        return handle.release();
    } catch (...) {
        return nullptr;
    }
}

void hako_pdu_action_mux_server_destroy(
    hako_pdu_action_mux_server_handle_t* handle)
{
    if (handle == nullptr) {
        return;
    }
    (void)stop_mux_server(handle);
    delete handle;
}

hako_pdu_action_error_t hako_pdu_action_mux_server_start(
    hako_pdu_action_mux_server_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (handle->started) {
        return HAKO_PDU_ACTION_OK;
    }
    if (!handle->services) {
        return HAKO_PDU_ACTION_ERROR_INITIALIZE;
    }
    try {
        if (!handle->services->start()) {
            return HAKO_PDU_ACTION_ERROR_START;
        }
        handle->started = true;
        return HAKO_PDU_ACTION_OK;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_stop(
    hako_pdu_action_mux_server_handle_t* handle)
{
    if (handle == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    return stop_mux_server(handle)
        ? HAKO_PDU_ACTION_OK
        : HAKO_PDU_ACTION_ERROR_INTERNAL;
}

hako_pdu_action_server_event_t hako_pdu_action_mux_server_poll(
    hako_pdu_action_mux_server_handle_t* handle,
    hako_pdu_action_server_event_info_t* out_info,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size,
    hako_pdu_action_error_t* out_error)
{
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_ACTION_OK;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (out_info != nullptr) {
        std::memset(out_info, 0, sizeof(*out_info));
    }
    if (handle == nullptr || out_info == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    if (!handle->started || !handle->services) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        if (!handle->pending_event.has_value()) {
            std::string action_name;
            action::ServerEvent event;
            const auto type = handle->services->poll(action_name, event);
            if (type == action::ServerEventType::NONE) {
                return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
            }
            event.type = type;
            handle->pending_event.emplace(
                std::move(action_name), std::move(event));
        }
        const auto& action_name = handle->pending_event->first;
        const auto& event = handle->pending_event->second;
        const auto type = event.type;
        fill_server_info(action_name, event, *out_info);
        const auto result = copy_pdu(event.pdu, buffer, capacity, out_size);
        if (result != HAKO_PDU_ACTION_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
        }
        if (type == action::ServerEventType::ERROR && out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        handle->pending_event.reset();
        return map_server_event(type);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
}

hako_pdu_action_server_event_t hako_pdu_action_mux_server_poll_alloc(
    hako_pdu_action_mux_server_handle_t* handle,
    hako_pdu_action_server_event_info_t* out_info,
    std::uint8_t** out_buffer,
    std::size_t* out_size,
    hako_pdu_action_error_t* out_error)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (out_error != nullptr) {
        *out_error = HAKO_PDU_ACTION_OK;
    }
    if (out_info != nullptr) {
        std::memset(out_info, 0, sizeof(*out_info));
    }
    if (handle == nullptr || out_info == nullptr
        || out_buffer == nullptr || out_size == nullptr) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    if (!handle->started || !handle->services) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
    try {
        std::lock_guard<std::mutex> lock(handle->poll_mutex);
        if (!handle->pending_event.has_value()) {
            std::string action_name;
            action::ServerEvent event;
            const auto type = handle->services->poll(action_name, event);
            if (type == action::ServerEventType::NONE) {
                return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
            }
            event.type = type;
            handle->pending_event.emplace(
                std::move(action_name), std::move(event));
        }
        const auto& action_name = handle->pending_event->first;
        const auto& event = handle->pending_event->second;
        const auto type = event.type;
        fill_server_info(action_name, event, *out_info);
        const auto result = allocate_pdu(event.pdu, out_buffer, out_size);
        if (result != HAKO_PDU_ACTION_OK) {
            if (out_error != nullptr) {
                *out_error = result;
            }
            return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
        }
        if (type == action::ServerEventType::ERROR && out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        handle->pending_event.reset();
        return map_server_event(type);
    } catch (...) {
        if (out_error != nullptr) {
            *out_error = HAKO_PDU_ACTION_ERROR_INTERNAL;
        }
        return HAKO_PDU_ACTION_SERVER_EVENT_NONE;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_accept_goal(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->accept_goal(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_reject_goal(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->reject_goal(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_accept_cancel(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->accept_cancel(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_reject_cancel(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->reject_cancel(action_name, native_goal)
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_create_feedback_buffer(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size)
{
    if (handle == nullptr || !valid_text(action_name) || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_feedback_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return copy_pdu(pdu, buffer, capacity, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_create_feedback_buffer_alloc(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    std::uint8_t** out_buffer,
    std::size_t* out_size)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (handle == nullptr || !valid_text(action_name)
        || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_feedback_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return allocate_pdu(pdu, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_create_result_buffer(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t* out_size)
{
    if (handle == nullptr || !valid_text(action_name) || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_result_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return copy_pdu(pdu, buffer, capacity, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_create_result_buffer_alloc(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    std::uint8_t** out_buffer,
    std::size_t* out_size)
{
    if (out_buffer != nullptr) {
        *out_buffer = nullptr;
    }
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (handle == nullptr || !valid_text(action_name)
        || out_buffer == nullptr || out_size == nullptr) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    try {
        action::PduData pdu;
        if (!handle->services->create_result_buffer(action_name, pdu)) {
            return HAKO_PDU_ACTION_ERROR_NOT_FOUND;
        }
        return allocate_pdu(pdu, out_buffer, out_size);
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_send_feedback(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal,
    const std::uint8_t* pdu,
    std::size_t pdu_size)
{
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr
        || pdu == nullptr || pdu_size == 0) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->send_feedback(
                   action_name, native_goal, make_pdu(pdu, pdu_size))
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

hako_pdu_action_error_t hako_pdu_action_mux_server_complete(
    hako_pdu_action_mux_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal,
    hako_pdu_action_terminal_status_t status,
    const std::uint8_t* pdu,
    std::size_t pdu_size)
{
    const bool valid_status = status == HAKO_PDU_ACTION_TERMINAL_SUCCEEDED
        || status == HAKO_PDU_ACTION_TERMINAL_CANCELED
        || status == HAKO_PDU_ACTION_TERMINAL_ABORTED;
    if (handle == nullptr || !valid_text(action_name) || goal == nullptr
        || pdu == nullptr || pdu_size == 0 || !valid_status) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    if (!handle->started || !handle->services) {
        return HAKO_PDU_ACTION_ERROR_NOT_RUNNING;
    }
    const auto native_goal = to_cpp_goal(*goal);
    if (!native_goal.valid()) {
        return HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT;
    }
    try {
        return handle->services->complete(
                   action_name,
                   native_goal,
                   to_cpp_status(status),
                   make_pdu(pdu, pdu_size))
            ? HAKO_PDU_ACTION_OK
            : HAKO_PDU_ACTION_ERROR_INVALID_STATE;
    } catch (...) {
        return HAKO_PDU_ACTION_ERROR_INTERNAL;
    }
}

std::size_t hako_pdu_action_mux_server_connected_count(
    const hako_pdu_action_mux_server_handle_t* handle)
{
    return handle != nullptr && handle->services
        ? handle->services->connected_count()
        : 0;
}

std::size_t hako_pdu_action_mux_server_expected_count(
    const hako_pdu_action_mux_server_handle_t* handle)
{
    return handle != nullptr && handle->services
        ? handle->services->expected_count()
        : 0;
}

int hako_pdu_action_mux_server_is_ready(
    const hako_pdu_action_mux_server_handle_t* handle)
{
    return handle != nullptr && handle->services
        && handle->services->is_ready()
        ? 1
        : 0;
}

} // extern "C"

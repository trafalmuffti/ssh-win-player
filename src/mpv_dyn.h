// Dynamic loader for libmpv-2.dll.
// The DLL is loaded at runtime so the executable can start (and show a
// friendly error) even when the DLL is missing, and so the small askpass
// helper mode does not have to load the 100+ MB library.
#pragma once
#include <windows.h>
#include <mpv/client.h>
#include <mpv/stream_cb.h>

struct MpvApi {
    HMODULE dll = nullptr;
    decltype(&::mpv_client_api_version) client_api_version = nullptr;
    decltype(&::mpv_create) create = nullptr;
    decltype(&::mpv_initialize) initialize = nullptr;
    decltype(&::mpv_terminate_destroy) terminate_destroy = nullptr;
    decltype(&::mpv_set_option_string) set_option_string = nullptr;
    decltype(&::mpv_set_option) set_option = nullptr;
    decltype(&::mpv_command) command = nullptr;
    decltype(&::mpv_command_async) command_async = nullptr;
    decltype(&::mpv_set_property_async) set_property_async = nullptr;
    decltype(&::mpv_set_property) set_property = nullptr;
    decltype(&::mpv_observe_property) observe_property = nullptr;
    decltype(&::mpv_wait_event) wait_event = nullptr;
    decltype(&::mpv_set_wakeup_callback) set_wakeup_callback = nullptr;
    decltype(&::mpv_error_string) error_string = nullptr;
    decltype(&::mpv_free) free = nullptr;
    decltype(&::mpv_get_property_string) get_property_string = nullptr;
    decltype(&::mpv_request_log_messages) request_log_messages = nullptr;
    decltype(&::mpv_stream_cb_add_ro) stream_cb_add_ro = nullptr;

    bool load(const wchar_t* path) {
        dll = LoadLibraryW(path);
        if (!dll) return false;
        bool ok = true;
#define MPV_LOAD(field, name)                                              \
        field = reinterpret_cast<decltype(field)>(                         \
            reinterpret_cast<void*>(GetProcAddress(dll, name)));            \
        if (!field) ok = false;
        MPV_LOAD(client_api_version, "mpv_client_api_version")
        MPV_LOAD(create, "mpv_create")
        MPV_LOAD(initialize, "mpv_initialize")
        MPV_LOAD(terminate_destroy, "mpv_terminate_destroy")
        MPV_LOAD(set_option_string, "mpv_set_option_string")
        MPV_LOAD(set_option, "mpv_set_option")
        MPV_LOAD(command, "mpv_command")
        MPV_LOAD(command_async, "mpv_command_async")
        MPV_LOAD(set_property_async, "mpv_set_property_async")
        MPV_LOAD(set_property, "mpv_set_property")
        MPV_LOAD(observe_property, "mpv_observe_property")
        MPV_LOAD(wait_event, "mpv_wait_event")
        MPV_LOAD(set_wakeup_callback, "mpv_set_wakeup_callback")
        MPV_LOAD(error_string, "mpv_error_string")
        MPV_LOAD(free, "mpv_free")
        MPV_LOAD(get_property_string, "mpv_get_property_string")
        MPV_LOAD(request_log_messages, "mpv_request_log_messages")
        MPV_LOAD(stream_cb_add_ro, "mpv_stream_cb_add_ro")
#undef MPV_LOAD
        return ok;
    }
};

extern MpvApi g_mpv;

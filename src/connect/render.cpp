#include "render.hpp"

#include <segmented_json_macros.h>
#include <eeprom.h>
#include <lfn.h>
#include <gcode_filename.h>

#include <cassert>
#include <cstring>

using json::JsonOutput;
using json::JsonResult;
using std::get_if;
using std::make_tuple;
using std::move;
using std::tuple;
using std::visit;

#define JSON_MAC(NAME, VAL) JSON_FIELD_STR_FORMAT(NAME, "%02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX", VAL[0], VAL[1], VAL[2], VAL[3], VAL[4], VAL[5])
#define JSON_IP(NAME, VAL)  JSON_FIELD_STR_FORMAT(NAME, "%hhu.%hhu.%hhu.%hhu", VAL[0], VAL[1], VAL[2], VAL[3])

namespace connect_client {

namespace {

    const char *to_str(Printer::DeviceState state) {
        switch (state) {
        case Printer::DeviceState::Idle:
            return "IDLE";
        case Printer::DeviceState::Printing:
            return "PRINTING";
        case Printer::DeviceState::Paused:
            return "PAUSED";
        case Printer::DeviceState::Finished:
            return "FINISHED";
        case Printer::DeviceState::Ready:
            return "READY";
        case Printer::DeviceState::Error:
            return "ERROR";
        case Printer::DeviceState::Busy:
            return "BUSY";
        case Printer::DeviceState::Unknown:
        default:
            return "UNKNOWN";
        }
    }

    bool is_printing(Printer::DeviceState state) {
        return state == Printer::DeviceState::Printing || state == Printer::DeviceState::Paused;
    }

    bool filename_is_firmware(const char *fname) {
        return filename_has_ext(fname, ".bbf");
    };

    const char *file_type_by_ext(const char *fname) {
        if (filename_is_gcode(fname)) {
            return "PRINT_FILE";
        } else if (filename_is_firmware(fname)) {
            return "FIRMWARE";
        } else {
            return "FILE";
        }
    }

    const char *file_type(const dirent *ent) {
        if (ent->d_type == DT_DIR) {
            return "FOLDER";
        } else {
            return file_type_by_ext(ent->d_name);
        }
    }

    JsonResult render_msg(size_t resume_point, JsonOutput &output, const RenderState &state, const SendTelemetry &telemetry) {
        const auto params = state.printer.params();
        const bool printing = is_printing(params.state);
        // Keep the indentation of the JSON in here!
        // clang-format off
        JSON_START;
        JSON_OBJ_START;
            if (!telemetry.empty) {
                JSON_FIELD_FFIXED("temp_nozzle", params.temp_nozzle, 1) JSON_COMMA;
                JSON_FIELD_FFIXED("temp_bed", params.temp_bed, 1) JSON_COMMA;
                JSON_FIELD_FFIXED("target_nozzle", params.target_nozzle, 1) JSON_COMMA;
                JSON_FIELD_FFIXED("target_bed", params.target_bed, 1) JSON_COMMA;
                JSON_FIELD_INT("speed", params.print_speed) JSON_COMMA;
                JSON_FIELD_INT("flow", params.flow_factor) JSON_COMMA;
                if (!printing) {
                    // To avoid spamming the DB, connect doesn't want positions during printing
                    JSON_FIELD_FFIXED("axis_x", params.pos[Printer::X_AXIS_POS], 2) JSON_COMMA;
                    JSON_FIELD_FFIXED("axis_y", params.pos[Printer::Y_AXIS_POS], 2) JSON_COMMA;
                }
                JSON_FIELD_FFIXED("axis_z", params.pos[Printer::Z_AXIS_POS], 2) JSON_COMMA;
                if (printing) {
                    JSON_FIELD_INT("job_id", params.job_id) JSON_COMMA;
                    JSON_FIELD_INT("time_printing", params.print_duration) JSON_COMMA;
                    JSON_FIELD_INT("time_remaining", params.time_to_end) JSON_COMMA;
                    JSON_FIELD_INT("progress", params.progress_percent) JSON_COMMA;
                    JSON_FIELD_INT("fan_extruder", params.heatbreak_fan_rpm) JSON_COMMA;
                    JSON_FIELD_INT("fan_print", params.print_fan_rpm) JSON_COMMA;
                    JSON_FIELD_FFIXED("filament", params.filament_used, 1) JSON_COMMA;
                }
                JSON_FIELD_STR("state", to_str(params.state));
            }
        JSON_OBJ_END;
        JSON_END;
        // clang-format on
    }

    JsonResult render_msg(size_t resume_point, JsonOutput &output, RenderState &state, const Event &event) {
        const auto params = state.printer.params();
        const auto &info = state.printer.printer_info();
        const bool has_extra = (event.type != EventType::Accepted) && (event.type != EventType::Rejected);
        const bool printing = is_printing(params.state);

        bool reject = false;
        Printer::NetCreds creds = {};

        if (event.type == EventType::JobInfo && (!printing || event.job_id != params.job_id)) {
            // Can't send a job info when not printing, refuse instead.
            //
            // Can't provide historic/future jobs.
            reject = true;
        }

        if (event.type == EventType::FileInfo && !state.has_stat && !state.file_extra.renderer.holds_alternative<DirRenderer>()) {
            // The file probably doesn't exist or something
            // Exception for /usb, as that one doesn't have stat even though it exists.
            reject = true;
        }

        if (reject) {
            // The fact we can render in multiple steps doesn't matter, we would
            // descend into here every time and resume the Rejected event.
            Event rejected(event);
            rejected.type = EventType::Rejected;
            return render_msg(resume_point, output, state, rejected);
        }

        // Keep the indentation of the JSON in here!
        // clang-format off
        JSON_START;
        JSON_OBJ_START;
            if (has_extra && printing) {
                JSON_FIELD_INT("job_id", params.job_id) JSON_COMMA;
            }

            // Relevant "data" block, if any

            // Note: this would very much like to be a switch. Nevertheless, the
            // JSON_START/macros are already a big and quite nasty switch, and the
            // JSON_... macros don't work in a nested switch :-(.
            if (event.type == EventType::Info) {
                JSON_FIELD_OBJ("data");
                    JSON_FIELD_STR("firmware", info.firmware_version) JSON_COMMA;
                    JSON_FIELD_STR("sn", info.serial_number) JSON_COMMA;
                    JSON_FIELD_BOOL("appendix", info.appendix) JSON_COMMA;
                    JSON_FIELD_STR("fingerprint", info.fingerprint) JSON_COMMA;
                    // Technically, it would be better to store this as part of
                    // the render state. But that would be a bit wasteful, so
                    // we do it here in a "late" fasion. At worst, we would get
                    // the api key and ssid from two different times, but they
                    // are not directly related to each other anyway.
                    creds = state.printer.net_creds();
                    if (strlen(creds.api_key) > 0) {
                        JSON_FIELD_STR("api_key", creds.api_key) JSON_COMMA;
                    }
                    JSON_FIELD_OBJ("network_info");
                    if (state.lan.has_value()) {
                        JSON_MAC("lan_mac", state.lan->mac) JSON_COMMA;
                        JSON_IP("lan_ipv4", state.lan->ip);
                    }
                    if (state.lan.has_value() && state.wifi.has_value()) {
                        // Why oh why can't json accept a trailing comma :-(
                        JSON_COMMA;
                    }
                    if (state.wifi.has_value()) {
                        if (strlen(creds.ssid) > 0) {
                            JSON_FIELD_STR("wifi_ssid", creds.ssid) JSON_COMMA;
                        }
                        JSON_MAC("wifi_mac", state.wifi->mac) JSON_COMMA;
                        JSON_IP("wifi_ipv4", state.wifi->ip);
                    }
                    JSON_OBJ_END;
                JSON_OBJ_END JSON_COMMA;
            } else if (event.type == EventType::JobInfo) {
                JSON_FIELD_OBJ("data");
                    // The JobInfo doesn't claim the buffer, so we get it to store the path.
                    assert(params.job_path != nullptr);
                    if (state.has_stat) {
                        JSON_FIELD_INT("size", state.st.st_size) JSON_COMMA;
                        JSON_FIELD_INT("m_timestamp", state.st.st_mtime) JSON_COMMA;
                    }
                    JSON_FIELD_STR("path_sfn", params.job_path) JSON_COMMA;
                    JSON_FIELD_STR("path", params.job_path);
                JSON_OBJ_END JSON_COMMA;
            } else if (event.type == EventType::FileInfo) {
                JSON_FIELD_OBJ("data");
                    // Note: This chunk might or might not render anything.
                    //
                    // * In theory, it can be EmptyRenderer (though that should not happen in practice?)
                    // * In case of the PreviewRenderer, it could be that it is
                    //   not a gcode at all or doesn't contain the preview.
                    //
                    // For that reason, the renderer is responsible for
                    // rendering a trailing comma if it outputs anything at
                    // all.
                    JSON_CHUNK(state.file_extra.renderer);
                    if (state.has_stat) {
                        // has_stat might be off in case of /usb, that one acts
                        // "weird", as it is root of the FS.
                        JSON_FIELD_INT("size", state.st.st_size) JSON_COMMA;
                        JSON_FIELD_INT("m_timestamp", state.st.st_mtime) JSON_COMMA;
                    }
                    // Warning: the path->name() is there (hidden) for FileInfo
                    // but _not_ for JobInfo. Do not just copy that into that
                    // part!
                    JSON_FIELD_STR("name", event.path->name()) JSON_COMMA;
                    JSON_FIELD_STR("path_sfn", event.path->path()) JSON_COMMA;
                    JSON_FIELD_STR("type", state.file_extra.renderer.holds_alternative<DirRenderer>() ? "FOLDER" : file_type_by_ext(event.path->path())) JSON_COMMA;
                    JSON_FIELD_STR("path", event.path->path());
                    // TODO: There's a lot of other things we want to extract
                    // from the file. To do that, we would also pre-open the
                    // file, extract the preview, extract the info...
                JSON_OBJ_END JSON_COMMA;
            }

            JSON_FIELD_STR("state", to_str(params.state)) JSON_COMMA;
            JSON_FIELD_INT("command_id", event.command_id.value_or(0)) JSON_COMMA;
            JSON_FIELD_STR("event", to_str(event.type));
        JSON_OBJ_END;
        JSON_END;
        // clang-format on
    }

    JsonResult render_msg(size_t, JsonOutput &, const RenderState &, const Sleep &) {
        // Sleep is handled on upper layers, not through renderer.
        assert(0);
        return JsonResult::Abort;
    }

}

PreviewRenderer::PreviewRenderer(FILE *f)
    // FIXME: The 16x16 request gives us 220x124 image. Any idea why? :-O
    : decoder(f, 16, 16, false) {}

tuple<JsonResult, size_t> PreviewRenderer::render(uint8_t *buffer, size_t buffer_size) {
    constexpr const char *intro = "\"preview\":\"";
    constexpr const char *outro = "\",";
    constexpr size_t intro_len = strlen(intro);
    // Ending quote and comma
    constexpr size_t outro_len = strlen(outro);
    // Don't bother with too small buffers to make the code easier. Extra char
    // for trying out there's some preview in there.
    constexpr size_t min_len = intro_len + outro_len + 1;

    if (buffer_size < min_len) {
        // Will be retried next time with bigger buffer.
        return make_tuple(JsonResult::BufferTooSmall, 0);
    }

    size_t written = 0;

    if (!started) {
        // It's OK to write into the buffer even if we would claim not to have
        // written there later on.
        memcpy(buffer, intro, intro_len);
        written += intro_len;
        buffer += intro_len;
        buffer_size -= intro_len;
    }

    const size_t available = buffer_size - outro_len;
    assert(available > 0);
    size_t decoded = decoder.Read(reinterpret_cast<char *>(buffer), available);

    if (decoded == 0 && !started) {
        // No preview -> just say we didn't do anything at all.
        return make_tuple(JsonResult::Complete, 0);
    }

    started = true;
    written += decoded;
    buffer += decoded;
    buffer_size -= decoded;

    JsonResult result = JsonResult::Incomplete;

    if (decoded < available) {
        // This is the end!
        result = JsonResult::Complete;
        memcpy(buffer, outro, outro_len);
        written += outro_len;
    }

    return make_tuple(result, written);
}

DirRenderer::DirRenderer(const char *base_path, unique_dir_ptr dir)
    : JsonRenderer(DirState { move(dir), base_path }) {}

JsonResult DirRenderer::renderState(size_t resume_point, json::JsonOutput &output, DirState &state) const {
    // Keep the indentation of the JSON in here!
    // clang-format off
    JSON_START;
    JSON_FIELD_ARR("children");
    while (state.dir.get() && (state.ent = readdir(state.dir.get()))) {
        state.child_cnt ++;

        if (!state.first) {
            JSON_COMMA;
        } else {
            state.first = false;
        }

        JSON_OBJ_START;
            JSON_FIELD_STR("name_sfn", state.ent->d_name) JSON_COMMA;
            JSON_FIELD_STR_FORMAT("path_sfn", "%s/%s", state.base_path, state.ent->d_name) JSON_COMMA;
#ifdef UNITTESTS
            JSON_FIELD_STR("name", state.ent->d_name) JSON_COMMA;
            JSON_FIELD_STR_FORMAT("path", "%s/%s", state.base_path, state.ent->d_name) JSON_COMMA;
#else
            JSON_FIELD_STR("name", state.ent->lfn) JSON_COMMA;
            // This is kind of "hybrid" path. The basename / last segment is
            // LFN, but the stuff before it is _likely_ SFN (because we expect
            // to get SFN there).
            JSON_FIELD_STR_FORMAT("path", "%s/%s", state.base_path, state.ent->lfn) JSON_COMMA;
#endif
            // We assume USB is not read only for us.
            JSON_FIELD_BOOL("ro", false) JSON_COMMA;
            JSON_FIELD_STR("type", file_type(state.ent));
        JSON_OBJ_END;
    }
    JSON_ARR_END JSON_COMMA;
    JSON_FIELD_INT("file_count", state.child_cnt) JSON_COMMA;
    JSON_END;
    // clang-format on
}

FileExtra::FileExtra(unique_file_ptr file)
    : file(move(file))
    , renderer(move(PreviewRenderer(this->file.get()))) {}

FileExtra::FileExtra(const char *base_path, unique_dir_ptr dir)
    : renderer(move(DirRenderer(base_path, move(dir)))) {}

RenderState::RenderState(const Printer &printer, const Action &action)
    : printer(printer)
    , action(action)
    , lan(printer.net_info(Printer::Iface::Ethernet))
    , wifi(printer.net_info(Printer::Iface::Wifi)) {
    memset(&st, 0, sizeof st);

    if (const auto *event = get_if<Event>(&action); event != nullptr) {
        const char *path = nullptr;
        const auto params = printer.params();
        bool error = false;

        switch (event->type) {
        case EventType::JobInfo:
            if (is_printing(params.state)) {
                path = params.job_path;
            }
            break;
        case EventType::FileInfo: {
            assert(event->path.has_value());
            SharedPath spath = event->path.value();
            path = spath.path();

            if (unique_dir_ptr d(opendir(path)); d.get() != nullptr) {
                file_extra = FileExtra(path, move(d));
            } else if (unique_file_ptr f(fopen(path, "r")); f.get() != nullptr) {
                file_extra = FileExtra(move(f));
            } else {
                error = true;
            }
            // We are being rude here a bit. While the event is const, we modify the shared buffer. Nevertheless:
            // * The shared buffer is not shared into other threads, so nobody
            //   is reading it at the same time as we are writing into it.
            // * If this is ever called multiple times (it can be, if the same
            //   event needs to be resent), it results into the same values
            //   there.
            get_LFN(spath.name(), FILE_NAME_BUFFER_LEN, spath.path());
            break;
        }
        default:;
        }

        if (!error && path != nullptr && stat(path, &st) == 0) {
            has_stat = true;
        }
    }
}

JsonResult Renderer::renderState(size_t resume_point, JsonOutput &output, RenderState &state) const {
    return visit([&](auto action) -> JsonResult {
        return render_msg(resume_point, output, state, action);
    },
        state.action);
}

}

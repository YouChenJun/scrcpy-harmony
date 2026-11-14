/* Modified for scrcpy-harmony; see NOTICE. */
#include "file_pusher.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "adb/adb.h"
#include "control_msg.h"
#include "controller.h"
#include "util/log.h"
#include "util/env.h"
#include "util/process.h"

#define DEFAULT_PUSH_TARGET "/sdcard/Download/"

static void
sc_file_pusher_request_destroy(struct sc_file_pusher_request *req) {
    free(req->file);
}

bool
sc_file_pusher_init(struct sc_file_pusher *fp, struct sc_controller *controller,
                    const char *serial, const char *push_target, bool harmony) {
    assert(controller);
    assert(serial);

    sc_vecdeque_init(&fp->queue);

    bool ok = sc_mutex_init(&fp->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_cond_init(&fp->event_cond);
    if (!ok) {
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    ok = sc_intr_init(&fp->intr);
    if (!ok) {
        sc_cond_destroy(&fp->event_cond);
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    fp->serial = strdup(serial);
    if (!fp->serial) {
        LOG_OOM();
        sc_intr_destroy(&fp->intr);
        sc_cond_destroy(&fp->event_cond);
        sc_mutex_destroy(&fp->mutex);
        return false;
    }

    // lazy initialization
    fp->initialized = false;

    fp->stopped = false;

    fp->harmony = harmony;
    fp->push_target = push_target ? push_target : harmony
                   ? "/storage/media/100/local/files/Docs/scrcpy/" : DEFAULT_PUSH_TARGET;
    fp->controller = controller;

    return true;
}

void
sc_file_pusher_destroy(struct sc_file_pusher *fp) {
    sc_cond_destroy(&fp->event_cond);
    sc_mutex_destroy(&fp->mutex);
    sc_intr_destroy(&fp->intr);
    free(fp->serial);

    while (!sc_vecdeque_is_empty(&fp->queue)) {
        struct sc_file_pusher_request *req = sc_vecdeque_popref(&fp->queue);
        assert(req);
        sc_file_pusher_request_destroy(req);
    }
    sc_vecdeque_destroy(&fp->queue);
}

bool
sc_file_pusher_request(struct sc_file_pusher *fp,
                       enum sc_file_pusher_action action, char *file) {
    // start file_pusher if it's used for the first time
    if (!fp->initialized) {
        if (!sc_file_pusher_start(fp)) {
            return false;
        }
        fp->initialized = true;
    }

    LOGI("Request to %s %s", action == SC_FILE_PUSHER_ACTION_INSTALL_APK
                                 ? "install" : "push",
                             file);
    struct sc_file_pusher_request req = {
        .action = action,
        .file = file,
    };

    sc_mutex_lock(&fp->mutex);
    bool was_empty = sc_vecdeque_is_empty(&fp->queue);
    bool res = sc_vecdeque_push(&fp->queue, req);
    if (!res) {
        LOG_OOM();
        sc_mutex_unlock(&fp->mutex);
        return false;
    }

    if (was_empty) {
        sc_cond_signal(&fp->event_cond);
    }
    sc_mutex_unlock(&fp->mutex);

    return true;
}

static bool
request_scan_file(struct sc_file_pusher *fp, const char *source) {
    struct sc_control_msg msg;
    msg.type = SC_CONTROL_MSG_TYPE_SCAN_FILE;
    if (fp->harmony) {
        const char *name = strrchr(source, '/');
        const char *windows_name = strrchr(source, '\\');
        if (windows_name && (!name || windows_name > name)) { name = windows_name; }
        name = name ? name + 1 : source;
        size_t length = strlen(fp->push_target);
        if (asprintf(&msg.scan_file.path, "%s%s%s", fp->push_target,
                     length && fp->push_target[length - 1] == '/' ? "" : "/", name) < 0) {
            msg.scan_file.path = NULL;
        }
    } else { msg.scan_file.path = strdup(fp->push_target); }
    if (!msg.scan_file.path) {
        LOG_OOM();
        return false;
    }

    if (!sc_controller_push_msg(fp->controller, &msg)) {
        LOGW("Could not request 'scan file'");
        return false;
    }

    return true;
}

static int
run_file_pusher(void *data) {
    struct sc_file_pusher *fp = data;
    struct sc_intr *intr = &fp->intr;

    const char *serial = fp->serial;
    assert(serial);

    const char *push_target = fp->push_target;
    assert(push_target);

    for (;;) {
        sc_mutex_lock(&fp->mutex);
        while (!fp->stopped && sc_vecdeque_is_empty(&fp->queue)) {
            sc_cond_wait(&fp->event_cond, &fp->mutex);
        }
        if (fp->stopped) {
            // stop immediately, do not process further events
            sc_mutex_unlock(&fp->mutex);
            break;
        }

        assert(!sc_vecdeque_is_empty(&fp->queue));
        struct sc_file_pusher_request req = sc_vecdeque_pop(&fp->queue);
        sc_mutex_unlock(&fp->mutex);

        if (fp->harmony) {
            const char *hdc = getenv("HDC");
            if (!hdc || !*hdc) { hdc = "hdc"; }
            const char *extension = strrchr(req.file, '.');
            bool hap = extension && !strcmp(extension, ".hap");
            const char *const push[] = {hdc, "-t", serial, "file", "send", req.file, push_target, NULL};
            const char *const install[] = {hdc, "-t", serial, "install", "-r", req.file, NULL};
            sc_pid pid;
            sc_pipe output;
            bool ok = sc_process_execute_p(hap ? install : push, &pid, 0, NULL, &output, NULL) == SC_PROCESS_SUCCESS;
            if (ok) {
                if (!sc_intr_set_process(intr, pid)) { sc_process_terminate(pid); }
                char result[8192]; size_t used = 0;
                char chunk[1024]; ssize_t count;
                while ((count = sc_pipe_read(output, chunk, sizeof(chunk))) > 0) {
                    size_t copy = (size_t) count;
                    if (copy > sizeof(result) - 1 - used) { copy = sizeof(result) - 1 - used; }
                    memcpy(result + used, chunk, copy); used += copy;
                }
                result[used] = '\0'; sc_pipe_close(output);
                sc_exit_code code = sc_process_wait(pid, false);
                sc_intr_set_process(intr, SC_PROCESS_NONE);
                sc_process_close(pid);
                ok = code == 0 && !strstr(result, "[Fail]")
                  && strstr(result, hap ? "install bundle successfully" : "FileTransfer finish");
            }
            if (ok) {
                LOGI("%s %s via hdc", hap ? "Installed" : "Transferred", req.file);
                if (!hap) { request_scan_file(fp, req.file); }
            } else { LOGE("hdc %s failed for %s", hap ? "install" : "transfer", req.file); }
        } else if (req.action == SC_FILE_PUSHER_ACTION_INSTALL_APK) {
            LOGI("Installing %s...", req.file);
            bool ok = sc_adb_install(intr, serial, req.file, 0);
            if (ok) {
                LOGI("%s successfully installed", req.file);
            } else {
                LOGE("Failed to install %s", req.file);
            }
        } else {
            LOGI("Pushing %s...", req.file);
            bool ok = sc_adb_push(intr, serial, req.file, push_target, 0);
            if (ok) {
                LOGI("%s successfully pushed to %s", req.file, push_target);
                request_scan_file(fp, req.file); // any error already logged
            } else {
                LOGE("Failed to push %s to %s", req.file, push_target);
            }
        }

        sc_file_pusher_request_destroy(&req);
    }
    return 0;
}

bool
sc_file_pusher_start(struct sc_file_pusher *fp) {
    LOGD("Starting file_pusher thread");

    bool ok = sc_thread_create(&fp->thread, run_file_pusher, "scrcpy-file", fp);
    if (!ok) {
        LOGE("Could not start file_pusher thread");
        return false;
    }

    return true;
}

void
sc_file_pusher_stop(struct sc_file_pusher *fp) {
    if (fp->initialized) {
        sc_mutex_lock(&fp->mutex);
        fp->stopped = true;
        sc_cond_signal(&fp->event_cond);
        sc_intr_interrupt(&fp->intr);
        sc_mutex_unlock(&fp->mutex);
    }
}

void
sc_file_pusher_join(struct sc_file_pusher *fp) {
    if (fp->initialized) {
        sc_thread_join(&fp->thread, NULL);
    }
}

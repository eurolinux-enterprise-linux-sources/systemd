/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <curl/curl.h>
#include <sys/prctl.h>

#include "sd-daemon.h"
#include "json.h"
#include "strv.h"
#include "btrfs-util.h"
#include "utf8.h"
#include "mkdir.h"
#include "import-util.h"
#include "curl-util.h"
#include "aufs-util.h"
#include "import-job.h"
#include "import-common.h"
#include "import-dkr.h"

typedef enum DkrProgress {
        DKR_SEARCHING,
        DKR_RESOLVING,
        DKR_METADATA,
        DKR_DOWNLOADING,
        DKR_COPYING,
} DkrProgress;

struct DkrImport {
        sd_event *event;
        CurlGlue *glue;

        char *index_url;
        char *image_root;

        ImportJob *images_job;
        ImportJob *tags_job;
        ImportJob *ancestry_job;
        ImportJob *json_job;
        ImportJob *layer_job;

        char *name;
        char *tag;
        char *id;

        char *response_token;
        char **response_registries;

        char **ancestry;
        unsigned n_ancestry;
        unsigned current_ancestry;

        DkrImportFinished on_finished;
        void *userdata;

        char *local;
        bool force_local;

        char *temp_path;
        char *final_path;

        pid_t tar_pid;
};

#define PROTOCOL_PREFIX "https://"

#define HEADER_TOKEN "X-Do" /* the HTTP header for the auth token */ "cker-Token:"
#define HEADER_REGISTRY "X-Do" /*the HTTP header for the registry */ "cker-Endpoints:"

#define LAYERS_MAX 2048

static void dkr_import_job_on_finished(ImportJob *j);

DkrImport* dkr_import_unref(DkrImport *i) {
        if (!i)
                return NULL;

        if (i->tar_pid > 1) {
                (void) kill_and_sigcont(i->tar_pid, SIGKILL);
                (void) wait_for_terminate(i->tar_pid, NULL);
        }

        import_job_unref(i->images_job);
        import_job_unref(i->tags_job);
        import_job_unref(i->ancestry_job);
        import_job_unref(i->json_job);
        import_job_unref(i->layer_job);

        curl_glue_unref(i->glue);
        sd_event_unref(i->event);

        if (i->temp_path) {
                (void) btrfs_subvol_remove(i->temp_path);
                (void) rm_rf_dangerous(i->temp_path, false, true, false);
                free(i->temp_path);
        }

        free(i->name);
        free(i->tag);
        free(i->id);
        free(i->response_token);
        free(i->response_registries);
        strv_free(i->ancestry);
        free(i->final_path);
        free(i->index_url);
        free(i->image_root);
        free(i->local);
        free(i);

        return NULL;
}

int dkr_import_new(
                DkrImport **ret,
                sd_event *event,
                const char *index_url,
                const char *image_root,
                DkrImportFinished on_finished,
                void *userdata) {

        _cleanup_(dkr_import_unrefp) DkrImport *i = NULL;
        char *e;
        int r;

        assert(ret);
        assert(index_url);

        if (!http_url_is_valid(index_url))
                return -EINVAL;

        i = new0(DkrImport, 1);
        if (!i)
                return -ENOMEM;

        i->on_finished = on_finished;
        i->userdata = userdata;

        i->image_root = strdup(image_root ?: "/var/lib/machines");
        if (!i->image_root)
                return -ENOMEM;

        i->index_url = strdup(index_url);
        if (!i->index_url)
                return -ENOMEM;

        e = endswith(i->index_url, "/");
        if (e)
                *e = 0;

        if (event)
                i->event = sd_event_ref(event);
        else {
                r = sd_event_default(&i->event);
                if (r < 0)
                        return r;
        }

        r = curl_glue_new(&i->glue, i->event);
        if (r < 0)
                return r;

        i->glue->on_finished = import_job_curl_on_finished;
        i->glue->userdata = i;

        *ret = i;
        i = NULL;

        return 0;
}

static void dkr_import_report_progress(DkrImport *i, DkrProgress p) {
        unsigned percent;

        assert(i);

        switch (p) {

        case DKR_SEARCHING:
                percent = 0;
                if (i->images_job)
                        percent += i->images_job->progress_percent * 5 / 100;
                break;

        case DKR_RESOLVING:
                percent = 5;
                if (i->tags_job)
                        percent += i->tags_job->progress_percent * 5 / 100;
                break;

        case DKR_METADATA:
                percent = 10;
                if (i->ancestry_job)
                        percent += i->ancestry_job->progress_percent * 5 / 100;
                if (i->json_job)
                        percent += i->json_job->progress_percent * 5 / 100;
                break;

        case DKR_DOWNLOADING:
                percent = 20;
                percent += 75 * i->current_ancestry / MAX(1U, i->n_ancestry);
                if (i->layer_job)
                        percent += i->layer_job->progress_percent * 75 / MAX(1U, i->n_ancestry) / 100;

                break;

        case DKR_COPYING:
                percent = 95;
                break;

        default:
                assert_not_reached("Unknown progress state");
        }

        sd_notifyf(false, "X_IMPORT_PROGRESS=%u", percent);
        log_debug("Combined progress %u%%", percent);
}

static int parse_id(const void *payload, size_t size, char **ret) {
        _cleanup_free_ char *buf = NULL, *id = NULL, *other = NULL;
        union json_value v = {};
        void *json_state = NULL;
        const char *p;
        int t;

        assert(payload);
        assert(ret);

        if (size <= 0)
                return -EBADMSG;

        if (memchr(payload, 0, size))
                return -EBADMSG;

        buf = strndup(payload, size);
        if (!buf)
                return -ENOMEM;

        p = buf;
        t = json_tokenize(&p, &id, &v, &json_state, NULL);
        if (t < 0)
                return t;
        if (t != JSON_STRING)
                return -EBADMSG;

        t = json_tokenize(&p, &other, &v, &json_state, NULL);
        if (t < 0)
                return t;
        if (t != JSON_END)
                return -EBADMSG;

        if (!dkr_id_is_valid(id))
                return -EBADMSG;

        *ret = id;
        id = NULL;

        return 0;
}

static int parse_ancestry(const void *payload, size_t size, char ***ret) {
        _cleanup_free_ char *buf = NULL;
        void *json_state = NULL;
        const char *p;
        enum {
                STATE_BEGIN,
                STATE_ITEM,
                STATE_COMMA,
                STATE_END,
        } state = STATE_BEGIN;
        _cleanup_strv_free_ char **l = NULL;
        size_t n = 0, allocated = 0;

        if (size <= 0)
                return -EBADMSG;

        if (memchr(payload, 0, size))
                return -EBADMSG;

        buf = strndup(payload, size);
        if (!buf)
                return -ENOMEM;

        p = buf;
        for (;;) {
                _cleanup_free_ char *str;
                union json_value v = {};
                int t;

                t = json_tokenize(&p, &str, &v, &json_state, NULL);
                if (t < 0)
                        return t;

                switch (state) {

                case STATE_BEGIN:
                        if (t == JSON_ARRAY_OPEN)
                                state = STATE_ITEM;
                        else
                                return -EBADMSG;

                        break;

                case STATE_ITEM:
                        if (t == JSON_STRING) {
                                if (!dkr_id_is_valid(str))
                                        return -EBADMSG;

                                if (n+1 > LAYERS_MAX)
                                        return -EFBIG;

                                if (!GREEDY_REALLOC(l, allocated, n + 2))
                                        return -ENOMEM;

                                l[n++] = str;
                                str = NULL;
                                l[n] = NULL;

                                state = STATE_COMMA;

                        } else if (t == JSON_ARRAY_CLOSE)
                                state = STATE_END;
                        else
                                return -EBADMSG;

                        break;

                case STATE_COMMA:
                        if (t == JSON_COMMA)
                                state = STATE_ITEM;
                        else if (t == JSON_ARRAY_CLOSE)
                                state = STATE_END;
                        else
                                return -EBADMSG;
                        break;

                case STATE_END:
                        if (t == JSON_END) {

                                if (strv_isempty(l))
                                        return -EBADMSG;

                                if (!strv_is_uniq(l))
                                        return -EBADMSG;

                                l = strv_reverse(l);

                                *ret = l;
                                l = NULL;
                                return 0;
                        } else
                                return -EBADMSG;
                }

        }
}

static const char *dkr_import_current_layer(DkrImport *i) {
        assert(i);

        if (strv_isempty(i->ancestry))
                return NULL;

        return i->ancestry[i->current_ancestry];
}

static const char *dkr_import_current_base_layer(DkrImport *i) {
        assert(i);

        if (strv_isempty(i->ancestry))
                return NULL;

        if (i->current_ancestry <= 0)
                return NULL;

        return i->ancestry[i->current_ancestry-1];
}

static int dkr_import_add_token(DkrImport *i, ImportJob *j) {
        const char *t;

        assert(i);
        assert(j);

        if (i->response_token)
                t = strjoina("Authorization: Token ", i->response_token);
        else
                t = HEADER_TOKEN " true";

        j->request_header = curl_slist_new("Accept: application/json", t, NULL);
        if (!j->request_header)
                return -ENOMEM;

        return 0;
}

static bool dkr_import_is_done(DkrImport *i) {
        assert(i);
        assert(i->images_job);

        if (i->images_job->state != IMPORT_JOB_DONE)
                return false;

        if (!i->tags_job || i->tags_job->state != IMPORT_JOB_DONE)
                return false;

        if (!i->ancestry_job || i->ancestry_job->state != IMPORT_JOB_DONE)
                return false;

        if (!i->json_job || i->json_job->state != IMPORT_JOB_DONE)
                return false;

        if (i->layer_job && i->layer_job->state != IMPORT_JOB_DONE)
                return false;

        if (dkr_import_current_layer(i))
                return false;

        return true;
}

static int dkr_import_make_local_copy(DkrImport *i) {
        int r;

        assert(i);

        if (!i->local)
                return 0;

        if (!i->final_path) {
                i->final_path = strjoin(i->image_root, "/.dkr-", i->id, NULL);
                if (!i->final_path)
                        return log_oom();
        }

        r = import_make_local_copy(i->final_path, i->image_root, i->local, i->force_local);
        if (r < 0)
                return r;

        return 0;
}

static int dkr_import_job_on_open_disk(ImportJob *j) {
        const char *base;
        DkrImport *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;
        assert(i->layer_job == j);
        assert(i->final_path);
        assert(!i->temp_path);
        assert(i->tar_pid <= 0);

        r = tempfn_random(i->final_path, &i->temp_path);
        if (r < 0)
                return log_oom();

        mkdir_parents_label(i->temp_path, 0700);

        base = dkr_import_current_base_layer(i);
        if (base) {
                const char *base_path;

                base_path = strjoina(i->image_root, "/.dkr-", base);
                r = btrfs_subvol_snapshot(base_path, i->temp_path, false, true);
        } else
                r = btrfs_subvol_make(i->temp_path);
        if (r < 0)
                return log_error_errno(r, "Failed to make btrfs subvolume %s: %m", i->temp_path);

        j->disk_fd = import_fork_tar(i->temp_path, &i->tar_pid);
        if (j->disk_fd < 0)
                return j->disk_fd;

        return 0;
}

static void dkr_import_job_on_progress(ImportJob *j) {
        DkrImport *i;

        assert(j);
        assert(j->userdata);

        i = j->userdata;

        dkr_import_report_progress(
                        i,
                        j == i->images_job                       ? DKR_SEARCHING :
                        j == i->tags_job                         ? DKR_RESOLVING :
                        j == i->ancestry_job || j == i->json_job ? DKR_METADATA :
                                                                   DKR_DOWNLOADING);
}

static int dkr_import_pull_layer(DkrImport *i) {
        _cleanup_free_ char *path = NULL;
        const char *url, *layer = NULL;
        int r;

        assert(i);
        assert(!i->layer_job);
        assert(!i->temp_path);
        assert(!i->final_path);

        for (;;) {
                layer = dkr_import_current_layer(i);
                if (!layer)
                        return 0; /* no more layers */

                path = strjoin(i->image_root, "/.dkr-", layer, NULL);
                if (!path)
                        return log_oom();

                if (laccess(path, F_OK) < 0) {
                        if (errno == ENOENT)
                                break;

                        return log_error_errno(errno, "Failed to check for container: %m");
                }

                log_info("Layer %s already exists, skipping.", layer);

                i->current_ancestry++;

                free(path);
                path = NULL;
        }

        log_info("Pulling layer %s...", layer);

        i->final_path = path;
        path = NULL;

        url = strjoina(PROTOCOL_PREFIX, i->response_registries[0], "/v1/images/", layer, "/layer");
        r = import_job_new(&i->layer_job, url, i->glue, i);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate layer job: %m");

        r = dkr_import_add_token(i, i->layer_job);
        if (r < 0)
                return log_oom();

        i->layer_job->on_finished = dkr_import_job_on_finished;
        i->layer_job->on_open_disk = dkr_import_job_on_open_disk;
        i->layer_job->on_progress = dkr_import_job_on_progress;

        r = import_job_begin(i->layer_job);
        if (r < 0)
                return log_error_errno(r, "Failed to start layer job: %m");

        return 0;
}

static void dkr_import_job_on_finished(ImportJob *j) {
        DkrImport *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;
        if (j->error != 0) {
                if (j == i->images_job)
                        log_error_errno(j->error, "Failed to retrieve images list. (Wrong index URL?)");
                else if (j == i->tags_job)
                        log_error_errno(j->error, "Failed to retrieve tags list.");
                else if (j == i->ancestry_job)
                        log_error_errno(j->error, "Failed to retrieve ancestry list.");
                else if (j == i->json_job)
                        log_error_errno(j->error, "Failed to retrieve json data.");
                else
                        log_error_errno(j->error, "Failed to retrieve layer data.");

                r = j->error;
                goto finish;
        }

        if (i->images_job == j) {
                const char *url;

                assert(!i->tags_job);
                assert(!i->ancestry_job);
                assert(!i->json_job);
                assert(!i->layer_job);

                if (strv_isempty(i->response_registries)) {
                        r = -EBADMSG;
                        log_error("Didn't get registry information.");
                        goto finish;
                }

                log_info("Index lookup succeeded, directed to registry %s.", i->response_registries[0]);
                dkr_import_report_progress(i, DKR_RESOLVING);

                url = strjoina(PROTOCOL_PREFIX, i->response_registries[0], "/v1/repositories/", i->name, "/tags/", i->tag);
                r = import_job_new(&i->tags_job, url, i->glue, i);
                if (r < 0) {
                        log_error_errno(r, "Failed to allocate tags job: %m");
                        goto finish;
                }

                r = dkr_import_add_token(i, i->tags_job);
                if (r < 0) {
                        log_oom();
                        goto finish;
                }

                i->tags_job->on_finished = dkr_import_job_on_finished;
                i->tags_job->on_progress = dkr_import_job_on_progress;

                r = import_job_begin(i->tags_job);
                if (r < 0) {
                        log_error_errno(r, "Failed to start tags job: %m");
                        goto finish;
                }

        } else if (i->tags_job == j) {
                const char *url;
                char *id = NULL;

                assert(!i->ancestry_job);
                assert(!i->json_job);
                assert(!i->layer_job);

                r = parse_id(j->payload, j->payload_size, &id);
                if (r < 0) {
                        log_error_errno(r, "Failed to parse JSON id.");
                        goto finish;
                }

                free(i->id);
                i->id = id;

                log_info("Tag lookup succeeded, resolved to layer %s.", i->id);
                dkr_import_report_progress(i, DKR_METADATA);

                url = strjoina(PROTOCOL_PREFIX, i->response_registries[0], "/v1/images/", i->id, "/ancestry");
                r = import_job_new(&i->ancestry_job, url, i->glue, i);
                if (r < 0) {
                        log_error_errno(r, "Failed to allocate ancestry job: %m");
                        goto finish;
                }

                r = dkr_import_add_token(i, i->ancestry_job);
                if (r < 0) {
                        log_oom();
                        goto finish;
                }

                i->ancestry_job->on_finished = dkr_import_job_on_finished;
                i->ancestry_job->on_progress = dkr_import_job_on_progress;

                url = strjoina(PROTOCOL_PREFIX, i->response_registries[0], "/v1/images/", i->id, "/json");
                r = import_job_new(&i->json_job, url, i->glue, i);
                if (r < 0) {
                        log_error_errno(r, "Failed to allocate json job: %m");
                        goto finish;
                }

                r = dkr_import_add_token(i, i->json_job);
                if (r < 0) {
                        log_oom();
                        goto finish;
                }

                i->json_job->on_finished = dkr_import_job_on_finished;
                i->json_job->on_progress = dkr_import_job_on_progress;

                r = import_job_begin(i->ancestry_job);
                if (r < 0) {
                        log_error_errno(r, "Failed to start ancestry job: %m");
                        goto finish;
                }

                r = import_job_begin(i->json_job);
                if (r < 0) {
                        log_error_errno(r, "Failed to start json job: %m");
                        goto finish;
                }

        } else if (i->ancestry_job == j) {
                char **ancestry = NULL, **k;
                unsigned n;

                assert(!i->layer_job);

                r = parse_ancestry(j->payload, j->payload_size, &ancestry);
                if (r < 0) {
                        log_error_errno(r, "Failed to parse JSON id.");
                        goto finish;
                }

                n = strv_length(ancestry);
                if (n <= 0 || !streq(ancestry[n-1], i->id)) {
                        log_error("Ancestry doesn't end in main layer.");
                        strv_free(ancestry);
                        r = -EBADMSG;
                        goto finish;
                }

                log_info("Ancestor lookup succeeded, requires layers:\n");
                STRV_FOREACH(k, ancestry)
                        log_info("\t%s", *k);

                strv_free(i->ancestry);
                i->ancestry = ancestry;
                i->n_ancestry = n;
                i->current_ancestry = 0;

                dkr_import_report_progress(i, DKR_DOWNLOADING);

                r = dkr_import_pull_layer(i);
                if (r < 0)
                        goto finish;

        } else if (i->layer_job == j) {
                assert(i->temp_path);
                assert(i->final_path);

                j->disk_fd = safe_close(j->disk_fd);

                if (i->tar_pid > 0) {
                        r = wait_for_terminate_and_warn("tar", i->tar_pid, true);
                        i->tar_pid = 0;
                        if (r < 0)
                                goto finish;
                }

                r = aufs_resolve(i->temp_path);
                if (r < 0) {
                        log_error_errno(r, "Failed to resolve aufs whiteouts: %m");
                        goto finish;
                }

                r = btrfs_subvol_set_read_only(i->temp_path, true);
                if (r < 0) {
                        log_error_errno(r, "Failed to mark snapshot read-only: %m");
                        goto finish;
                }

                if (rename(i->temp_path, i->final_path) < 0) {
                        log_error_errno(errno, "Failed to rename snaphsot: %m");
                        goto finish;
                }

                log_info("Completed writing to layer %s.", i->final_path);

                i->layer_job = import_job_unref(i->layer_job);
                free(i->temp_path);
                i->temp_path = NULL;
                free(i->final_path);
                i->final_path = NULL;

                i->current_ancestry ++;
                r = dkr_import_pull_layer(i);
                if (r < 0)
                        goto finish;

        } else if (i->json_job != j)
                assert_not_reached("Got finished event for unknown curl object");

        if (!dkr_import_is_done(i))
                return;

        dkr_import_report_progress(i, DKR_COPYING);

        r = dkr_import_make_local_copy(i);
        if (r < 0)
                goto finish;

        r = 0;

finish:
        if (i->on_finished)
                i->on_finished(i, r, i->userdata);
        else
                sd_event_exit(i->event, r);
}

static int dkr_import_job_on_header(ImportJob *j, const char *header, size_t sz)  {
        _cleanup_free_ char *registry = NULL;
        char *token;
        DkrImport *i;
        int r;

        assert(j);
        assert(j->userdata);

        i = j->userdata;

        r = curl_header_strdup(header, sz, HEADER_TOKEN, &token);
        if (r < 0)
                return log_oom();
        if (r > 0) {
                free(i->response_token);
                i->response_token = token;
                return 0;
        }

        r = curl_header_strdup(header, sz, HEADER_REGISTRY, &registry);
        if (r < 0)
                return log_oom();
        if (r > 0) {
                char **l, **k;

                l = strv_split(registry, ",");
                if (!l)
                        return log_oom();

                STRV_FOREACH(k, l) {
                        if (!hostname_is_valid(*k)) {
                                log_error("Registry hostname is not valid.");
                                strv_free(l);
                                return -EBADMSG;
                        }
                }

                strv_free(i->response_registries);
                i->response_registries = l;
        }

        return 0;
}

int dkr_import_pull(DkrImport *i, const char *name, const char *tag, const char *local, bool force_local) {
        const char *url;
        int r;

        assert(i);

        if (!dkr_name_is_valid(name))
                return -EINVAL;

        if (tag && !dkr_tag_is_valid(tag))
                return -EINVAL;

        if (local && !machine_name_is_valid(local))
                return -EINVAL;

        if (i->images_job)
                return -EBUSY;

        if (!tag)
                tag = "latest";

        r = free_and_strdup(&i->local, local);
        if (r < 0)
                return r;
        i->force_local = force_local;

        r = free_and_strdup(&i->name, name);
        if (r < 0)
                return r;
        r = free_and_strdup(&i->tag, tag);
        if (r < 0)
                return r;

        url = strjoina(i->index_url, "/v1/repositories/", name, "/images");

        r = import_job_new(&i->images_job, url, i->glue, i);
        if (r < 0)
                return r;

        r = dkr_import_add_token(i, i->images_job);
        if (r < 0)
                return r;

        i->images_job->on_finished = dkr_import_job_on_finished;
        i->images_job->on_header = dkr_import_job_on_header;
        i->images_job->on_progress = dkr_import_job_on_progress;

        return import_job_begin(i->images_job);
}

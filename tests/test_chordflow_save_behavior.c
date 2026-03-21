#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

extern midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host);

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static int get_int_param(midi_fx_api_v1_t *api, void *inst, const char *key) {
    char buf[64];
    memset(buf, 0, sizeof(buf));
    if (api->get_param(inst, key, buf, sizeof(buf)) <= 0) {
        fail("get_param returned error");
    }
    return atoi(buf);
}

static void get_str_param(midi_fx_api_v1_t *api, void *inst, const char *key, char *dst, int dst_len) {
    memset(dst, 0, (size_t)dst_len);
    if (api->get_param(inst, key, dst, dst_len) <= 0) {
        fail("get_param returned error");
    }
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;
    void *inst;
    int before_count;
    int after_count;
    int named_count_before;
    int named_count_after;
    int bank_count;
    char name[64];
    char expected_name[64];
    char preset_list[4096];
    char bank_name[64];
    const char *named_preset = "My Neo Set";

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->destroy_instance || !api->set_param || !api->get_param) {
        fail("API callbacks missing");
    }

    inst = api->create_instance(".", NULL);
    if (!inst) fail("create_instance failed");

    before_count = get_int_param(api, inst, "preset_count");
    bank_count = get_int_param(api, inst, "bank_count");
    if (bank_count <= 0) {
        fprintf(stderr, "FAIL: expected bank_count > 0 got %d\n", bank_count);
        return 1;
    }

    api->set_param(inst, "save", "save");

    after_count = get_int_param(api, inst, "preset_count");
    if (after_count != before_count + 1) {
        fprintf(stderr, "FAIL: preset_count expected %d got %d\n", before_count + 1, after_count);
        return 1;
    }

    get_str_param(api, inst, "preset_name", name, sizeof(name));
    snprintf(expected_name, sizeof(expected_name), "Preset %d", before_count + 1);
    if (strcmp(name, expected_name) != 0) {
        fprintf(stderr, "FAIL: preset_name expected %s got %s\n", expected_name, name);
        return 1;
    }

    named_count_before = get_int_param(api, inst, "preset_count");
    api->set_param(inst, "save", named_preset);
    named_count_after = get_int_param(api, inst, "preset_count");
    if (named_count_after != named_count_before + 1) {
        fprintf(stderr, "FAIL: named save preset_count expected %d got %d\n", named_count_before + 1, named_count_after);
        return 1;
    }

    get_str_param(api, inst, "preset_name", name, sizeof(name));
    if (strcmp(name, named_preset) != 0) {
        fprintf(stderr, "FAIL: named preset_name expected %s got %s\n", named_preset, name);
        return 1;
    }
    get_str_param(api, inst, "bank_name", bank_name, sizeof(bank_name));
    if (strcmp(bank_name, "User") != 0) {
        fprintf(stderr, "FAIL: named save bank_name expected User got %s\n", bank_name);
        return 1;
    }

    get_str_param(api, inst, "preset_list", preset_list, sizeof(preset_list));
    if (strstr(preset_list, named_preset) == NULL) {
        fprintf(stderr, "FAIL: preset_list missing named preset %s\n", named_preset);
        return 1;
    }

    api->destroy_instance(inst);
    printf("PASS: chordflow save behavior\n");
    return 0;
}

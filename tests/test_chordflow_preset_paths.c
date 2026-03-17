#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

extern midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host);

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void write_text_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) fail("failed to open file for write");
    if (fputs(contents, f) == EOF) {
        fclose(f);
        fail("failed to write file");
    }
    fclose(f);
}

static void get_str_param(midi_fx_api_v1_t *api, void *inst, const char *key, char *dst, int dst_len) {
    memset(dst, 0, (size_t)dst_len);
    if (api->get_param(inst, key, dst, dst_len) <= 0) {
        fail("get_param returned error");
    }
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;
    void *inst;
    char tmp_template[] = "/tmp/chordflow-paths-XXXXXX";
    char *tmp_dir;
    char presets_dir[1024];
    char default_path[1024];
    char user_path[1024];
    char preset_name[128];
    char bank_name[128];
    char user_json[8192];
    FILE *user_file;

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    tmp_dir = mkdtemp(tmp_template);
    if (!tmp_dir) fail("mkdtemp failed");

    snprintf(presets_dir, sizeof(presets_dir), "%s/presets", tmp_dir);
    if (mkdir(presets_dir, 0755) != 0) fail("mkdir presets failed");

    snprintf(default_path, sizeof(default_path), "%s/default.json", presets_dir);
    write_text_file(
        default_path,
        "[{\"name\":\"Path Default\",\"global_octave\":2,\"global_transpose\":0,"
        "\"pads\":[{\"octave\":0,\"root\":0,\"chord_type\":\"maj\",\"inversion\":\"root\","
        "\"strum\":0,\"strum_dir\":\"up\",\"articulation\":\"off\",\"reverse_art\":\"off\"}]}]"
    );

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->destroy_instance || !api->set_param || !api->get_param) {
        fail("API callbacks missing");
    }

    inst = api->create_instance(tmp_dir, NULL);
    if (!inst) fail("create_instance failed");

    get_str_param(api, inst, "preset_name", preset_name, sizeof(preset_name));
    if (strcmp(preset_name, "Path Default") != 0) {
        fprintf(stderr, "FAIL: expected preset_name Path Default, got %s\n", preset_name);
        return 1;
    }
    get_str_param(api, inst, "bank_name", bank_name, sizeof(bank_name));
    if (strcmp(bank_name, "Factory") != 0) {
        fprintf(stderr, "FAIL: expected bank_name Factory, got %s\n", bank_name);
        return 1;
    }

    api->set_param(inst, "save", "Path User");

    snprintf(user_path, sizeof(user_path), "%s/user.json", presets_dir);
    if (!file_exists(user_path)) {
        fprintf(stderr, "FAIL: expected user presets file at %s\n", user_path);
        return 1;
    }

    user_file = fopen(user_path, "r");
    if (!user_file) fail("failed to open user presets file");
    size_t rd = fread(user_json, 1, sizeof(user_json) - 1, user_file);
    user_json[rd] = '\0';
    fclose(user_file);

    if (strstr(user_json, "\"name\":\"Path User\"") == NULL) {
        fprintf(stderr, "FAIL: user presets file missing saved name Path User\n");
        return 1;
    }

    api->destroy_instance(inst);
    printf("PASS: chordflow preset path behavior\n");
    return 0;
}

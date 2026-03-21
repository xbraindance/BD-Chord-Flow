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

static int send_msg(midi_fx_api_v1_t *api, void *inst, uint8_t status, uint8_t note, uint8_t vel) {
    uint8_t in[3] = { status, note, vel };
    uint8_t out_msgs[16][3];
    int out_lens[16];
    return api->process_midi(inst, in, 3, out_msgs, out_lens, 16);
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;
    void *inst;
    int out;

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->destroy_instance || !api->set_param || !api->process_midi) {
        fail("API callbacks missing");
    }

    inst = api->create_instance(".", NULL);
    if (!inst) fail("create_instance failed");

    /* Make pad 1 and 2 emit the same chord tones. */
    api->set_param(inst, "pad", "1");
    api->set_param(inst, "root", "c");
    api->set_param(inst, "chord_type", "maj");
    api->set_param(inst, "inversion", "root");
    api->set_param(inst, "pad_octave", "0");
    api->set_param(inst, "bass", "none");

    api->set_param(inst, "pad", "2");
    api->set_param(inst, "root", "c");
    api->set_param(inst, "chord_type", "maj");
    api->set_param(inst, "inversion", "root");
    api->set_param(inst, "pad_octave", "0");
    api->set_param(inst, "bass", "none");

    out = send_msg(api, inst, 0x90, 36, 100);
    if (out <= 0) fail("first note_on produced no output");

    out = send_msg(api, inst, 0x90, 37, 100);
    if (out <= 0) fail("second note_on produced no output");

    /* Releasing first trigger must not choke notes still held by second trigger. */
    out = send_msg(api, inst, 0x80, 36, 0);
    if (out != 0) fail("first note_off should not release overlapping active notes");

    out = send_msg(api, inst, 0x80, 37, 0);
    if (out <= 0) fail("second note_off should release chord");

    api->destroy_instance(inst);
    printf("PASS: chordflow overlap release\n");
    return 0;
}

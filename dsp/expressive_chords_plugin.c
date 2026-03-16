/**
 * expressive_chords_plugin.c
 *
 * Move Anything MIDI FX — midi_fx_api_v1
 *
 * Architecture:
 *   - 32 pad slots per preset, each with independent chord settings
 *   - Loading a preset copies all 32 pad configs into active_pad_slots[]
 *   - Pressing a pad (note 68-99) switches current_pad and plays that pad's chord
 *   - "save" param writes active_pad_slots[] as a new/overwritten preset
 *
 * Params (string key / string value):
 *   preset        int      0-based index — loads all 32 pad slots from that preset
 *   preset_count  int      (read-only)
 *   preset_name   string   (read-only) name of current preset
 *   current_pad   int      1-32 active pad for editing
 *   root          enum     c/c#/d/d#/e/f/f#/g/g#/a/a#/b
 *   type          enum     maj/min/dom7/maj7/min7/sus2/sus4/add9/min9/maj9/dim/aug/5th/6th/min6/dom9
 *   inversion     enum     root/1st/2nd/3rd
 *   strum         int      0-100 (ms between notes)
 *   strum_dir     enum     up/down
 *   articulation  enum     off/on
 *   reverse_art   enum     off/on
 *   save          string   preset name to save as (writes current pad collection)
 *   state         string   (read-only) JSON snapshot of current pad's settings
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

/* ── API version ──────────────────────────────────────────────────────────── */
#define MIDI_FX_API_VERSION   1
#define MIDI_FX_MAX_OUT       16
#define MAX_PRESETS           64
#define MAX_CHORD_NOTES       8
#define MAX_PRESET_NAME       48
#define PAD_COUNT             32
#define MAX_PENDING           64
#define USER_PRESETS_PATH     "/data/UserData/move-anything/expressive-chords/presets.json"

/* ── Logging macro per CLAUDE.md standards ────────────────────────────────── */
/* LOG: safe printf-style logging via host->log */
#define LOG(...) do { \
    if (inst && inst->host && inst->host->log) { \
        char _logbuf[256]; \
        snprintf(_logbuf, sizeof(_logbuf), __VA_ARGS__); \
        inst->host->log(_logbuf); \
    } \
} while(0)

/* ── Host API (passed to move_midi_fx_init) ──────────────────────────────── */
typedef struct {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int  (*midi_send_internal)(const uint8_t *msg, int len);
    int  (*midi_send_external)(const uint8_t *msg, int len);
    int  (*get_clock_status)(void);
} host_api_v1_t;

/* ── Plugin API vtable (per midi_fx_api_v1 spec) ─────────────────────────── */
typedef struct midi_fx_api_v1 {
    uint32_t api_version;  /* Must be 1 (MIDI_FX_API_VERSION) */

    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);

    int (*process_midi)(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out);

    int (*tick)(void *instance,
                int frames, int sample_rate,
                uint8_t out_msgs[][3], int out_lens[],
                int max_out);

    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} midi_fx_api_v1_t;

/* ── Enum tables ─────────────────────────────────────────────────────────── */
#define ROOT_COUNT 12
static const char *ROOT_NAMES[ROOT_COUNT] = {
    "c","c#","d","d#","e","f","f#","g","g#","a","a#","b"
};

#define TYPE_COUNT 16
static const char *TYPE_NAMES[TYPE_COUNT] = {
    "maj","min","dom7","maj7","min7","sus2","sus4","add9",
    "min9","maj9","dim","aug","5th","6th","min6","dom9"
};

#define INV_COUNT 4
static const char *INV_NAMES[INV_COUNT] = { "root","1st","2nd","3rd" };

#define DIR_COUNT 2
static const char *DIR_NAMES[DIR_COUNT] = { "up","down" };

#define ART_COUNT 2
static const char *ART_NAMES[ART_COUNT] = { "off","on" };

/* ── Chord interval tables ───────────────────────────────────────────────── */
typedef struct { int offs[8]; int count; } chord_def_t;

static const chord_def_t CHORD_DEFS[TYPE_COUNT] = {
    {{0,4,7},3},           /* maj   */
    {{0,3,7},3},           /* min   */
    {{0,4,7,10},4},        /* dom7  */
    {{0,4,7,11},4},        /* maj7  */
    {{0,3,7,10},4},        /* min7  */
    {{0,2,7},3},           /* sus2  */
    {{0,5,7},3},           /* sus4  */
    {{0,4,7,14},4},        /* add9  */
    {{0,3,7,10,14},5},     /* min9  */
    {{0,4,7,11,14},5},     /* maj9  */
    {{0,3,6},3},           /* dim   */
    {{0,4,8},3},           /* aug   */
    {{0,7},2},             /* 5th   */
    {{0,4,7,9},4},         /* 6th   */
    {{0,3,7,9},4},         /* min6  */
    {{0,4,7,10,14},5},     /* dom9  */
};

/* ── Data structures ─────────────────────────────────────────────────────── */
typedef struct {
    int root;         /* 0-11 */
    int type;         /* 0-15 */
    int inversion;    /* 0-3  */
    int strum;        /* 0-100 ms */
    int strum_dir;    /* 0=up 1=down */
    int articulation; /* 0=off 1=on */
    int reverse_art;  /* 0=off 1=on */
} pad_slot_t;

typedef struct {
    char      name[MAX_PRESET_NAME];
    pad_slot_t slots[PAD_COUNT];
} preset_t;

typedef struct {
    uint8_t note;
    uint8_t vel;
    uint8_t ch;
    int     delay_frames;
} pending_note_t;

/* Instance structure with host pointer for logging (per CLAUDE.md) */
typedef struct {
    const host_api_v1_t *host;      /* Host API reference for logging */
    char       module_dir[700];
    preset_t   presets[MAX_PRESETS];
    int        preset_count;
    int        active_preset;       /* index of loaded preset, -1 = none */
    pad_slot_t active_pad_slots[PAD_COUNT];
    int        current_pad;         /* 1-based */
    int8_t     held_out[128][MAX_CHORD_NOTES];
    int        held_out_count[128];
    pending_note_t pending[MAX_PENDING];
    int        pending_count;
    int        sample_rate;
} expchords_t;

/* ── Helpers (snake_case per CLAUDE.md) ─────────────────────────────────── */
static int clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int find_enum(const char **names, int count, const char *val) {
    int i;
    for (i = 0; i < count; i++)
        if (strcmp(names[i], val) == 0) return i;
    return -1;
}

static int parse_enum(const char **names, int count, const char *val) {
    int v = find_enum(names, count, val);
    if (v >= 0) return v;
    v = atoi(val);
    return (v >= 0 && v < count) ? v : 0;
}

/* ── JSON mini-parser ────────────────────────────────────────────────────── */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *parse_str(const char *p, char *dst, int dlen) {
    p = skip_ws(p);
    if (*p != '"') return p;
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') p++;
        if (i < dlen - 1) dst[i++] = *p;
        p++;
    }
    dst[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char *parse_int_val(const char *p, int *out) {
    p = skip_ws(p);
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    *out = 0;
    while (*p >= '0' && *p <= '9') { *out = *out * 10 + (*p - '0'); p++; }
    if (neg) *out = -(*out);
    return p;
}

static const char *json_find_key(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    p = skip_ws(p);
    if (*p != ':') return NULL;
    return p + 1;
}

static int json_get_int(const char *json, const char *key, int def) {
    const char *p = json_find_key(json, key);
    if (!p) return def;
    int v = def;
    parse_int_val(p, &v);
    return v;
}

static int json_get_str(const char *json, const char *key, char *dst, int dlen) {
    const char *p = json_find_key(json, key);
    if (!p) return 0;
    p = skip_ws(p);
    if (*p != '"') return 0;
    parse_str(p, dst, dlen);
    return 1;
}

/* ── Preset JSON loading ─────────────────────────────────────────────────── */
static pad_slot_t parse_slot(const char *obj) {
    pad_slot_t s;
    char tmp[32];

    /* root: try string first ("c","c#",...), fall back to int */
    if (json_get_str(obj, "root", tmp, sizeof(tmp))) {
        int v = find_enum(ROOT_NAMES, ROOT_COUNT, tmp);
        s.root = (v >= 0) ? v : clamp_i(atoi(tmp), 0, ROOT_COUNT-1);
    } else {
        s.root = clamp_i(json_get_int(obj,"root",0), 0, ROOT_COUNT-1);
    }

    /* type: try string first ("maj","min",...), fall back to int */
    if (json_get_str(obj, "type", tmp, sizeof(tmp))) {
        int v = find_enum(TYPE_NAMES, TYPE_COUNT, tmp);
        s.type = (v >= 0) ? v : clamp_i(atoi(tmp), 0, TYPE_COUNT-1);
    } else {
        s.type = clamp_i(json_get_int(obj,"type",0), 0, TYPE_COUNT-1);
    }

    /* inversion: int only */
    s.inversion = clamp_i(json_get_int(obj,"inversion",0), 0, INV_COUNT-1);

    /* strum: int */
    s.strum = clamp_i(json_get_int(obj,"strum",0), 0, 100);

    /* strum_dir: try string ("up"/"down"), fall back to int */
    if (json_get_str(obj, "strum_dir", tmp, sizeof(tmp))) {
        int v = find_enum(DIR_NAMES, DIR_COUNT, tmp);
        s.strum_dir = (v >= 0) ? v : clamp_i(atoi(tmp), 0, DIR_COUNT-1);
    } else {
        s.strum_dir = clamp_i(json_get_int(obj,"strum_dir",0), 0, DIR_COUNT-1);
    }

    /* articulation: try string ("off"/"on"), fall back to int */
    if (json_get_str(obj, "articulation", tmp, sizeof(tmp))) {
        int v = find_enum(ART_NAMES, ART_COUNT, tmp);
        s.articulation = (v >= 0) ? v : clamp_i(atoi(tmp), 0, ART_COUNT-1);
    } else {
        s.articulation = clamp_i(json_get_int(obj,"articulation",0), 0, ART_COUNT-1);
    }

    /* reverse_art: try string ("off"/"on"), fall back to int */
    if (json_get_str(obj, "reverse_art", tmp, sizeof(tmp))) {
        int v = find_enum(ART_NAMES, ART_COUNT, tmp);
        s.reverse_art = (v >= 0) ? v : clamp_i(atoi(tmp), 0, ART_COUNT-1);
    } else {
        s.reverse_art = clamp_i(json_get_int(obj,"reverse_art",0), 0, ART_COUNT-1);
    }

    return s;
}

static pad_slot_t default_slot(void) {
    pad_slot_t s;
    memset(&s, 0, sizeof(s));
    return s;
}

/* ── File I/O with host logging (CLAUDE.md compliant) ───────────────────── */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 2*1024*1024) {
        fclose(f);
        return NULL;
    }
    
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

/* Parse presets JSON — supports two formats:
 *   Old flat:  [{"name":"X", "root":0, "type":4, ...}, ...]
 *   New pads:  [{"name":"X", "pads":[{...},{...},...32...]}, ...]
 */
static void load_presets_from_json(expchords_t *inst, const char *json) {
    const char *p = skip_ws(json);
    if (*p != '[') return;
    p++;
    inst->preset_count = 0;

    while (*p && *p != ']' && inst->preset_count < MAX_PRESETS) {
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p != '{') { p++; continue; }

        /* Find end of this object */
        const char *start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        int len = (int)(p - start);
        char *obj = malloc(len + 1);
        if (!obj) continue;
        memcpy(obj, start, len);
        obj[len] = '\0';

        preset_t *pr = &inst->presets[inst->preset_count];
        strncpy(pr->name, "preset", MAX_PRESET_NAME - 1);
        pr->name[MAX_PRESET_NAME - 1] = '\0';
        json_get_str(obj, "name", pr->name, MAX_PRESET_NAME);

        /* Check for pads array */
        const char *pads_pos = strstr(obj, "\"pads\"");
        if (pads_pos) {
            /* New format: each pad has its own slot */
            const char *arr = strchr(pads_pos, '[');
            if (arr) {
                arr++;
                int pad_idx = 0;
                while (*arr && *arr != ']' && pad_idx < PAD_COUNT) {
                    arr = skip_ws(arr);
                    if (*arr == ',') { arr++; continue; }
                    if (*arr != '{') { arr++; continue; }
                    const char *ps = arr;
                    int d = 0;
                    while (*arr) {
                        if (*arr == '{') d++;
                        else if (*arr == '}') { d--; if (d==0){arr++;break;} }
                        arr++;
                    }
                    int pl = (int)(arr - ps);
                    char *pobj = malloc(pl + 1);
                    if (pobj) {
                        memcpy(pobj, ps, pl);
                        pobj[pl] = '\0';
                        pr->slots[pad_idx] = parse_slot(pobj);
                        free(pobj);
                    }
                    pad_idx++;
                }
                /* Fill remaining pads with default */
                while (pad_idx < PAD_COUNT)
                    pr->slots[pad_idx++] = default_slot();
            }
        } else {
            /* Old flat format: one slot definition applies to all pads */
            pad_slot_t s = parse_slot(obj);
            for (int i = 0; i < PAD_COUNT; i++) pr->slots[i] = s;
        }

        free(obj);
        inst->preset_count++;
    }
}

static void load_presets(expchords_t *inst) {
    char *json = read_file(USER_PRESETS_PATH);
    if (!json) {
        char path[800];
        snprintf(path, sizeof(path), "%s/presets_default.json", inst->module_dir);
        json = read_file(path);
    }
    
    LOG("loading presets...");
    if (json) {
        load_presets_from_json(inst, json);
        free(json);
        LOG("presets loaded successfully");
    }
    
    if (inst->preset_count == 0) {
        /* Hardcoded fallback */
        inst->preset_count = 1;
        strncpy(inst->presets[0].name, "Default", MAX_PRESET_NAME - 1);
        for (int i = 0; i < PAD_COUNT; i++)
            inst->presets[0].slots[i] = default_slot();
        LOG("using fallback preset");
    } else {
        LOG("preset count: %d", inst->preset_count);
    }
}

/* ── Directory creation (POSIX instead of system()) ──────────────────────── */
static void ensure_presets_dir(expchords_t *inst) {
    int ret = mkdir("/data/UserData/move-anything/expressive-chords", 0755);
    if (ret != 0 && errno != EEXIST) {
        LOG("failed to create presets dir: %s", strerror(errno));
    } else {
        LOG("presets directory ready");
    }
}

static void save_presets(expchords_t *inst) {
    ensure_presets_dir(inst);
    
    FILE *f = fopen(USER_PRESETS_PATH, "w");
    if (!f) {
        LOG("failed to open presets file for writing");
        return;
    }
    
    fprintf(f, "[\n");
    for (int i = 0; i < inst->preset_count; i++) {
        preset_t *pr = &inst->presets[i];
        fprintf(f, "  {\"name\":\"%s\",\"pads\":[\n", pr->name);
        for (int j = 0; j < PAD_COUNT; j++) {
            pad_slot_t *s = &pr->slots[j];
            fprintf(f, "    {\"root\":%d,\"type\":%d,\"inversion\":%d,"
                    "\"strum\":%d,\"strum_dir\":%d,"
                    "\"articulation\":%d,\"reverse_art\":%d}%s\n",
                    s->root, s->type, s->inversion,
                    s->strum, s->strum_dir, s->articulation, s->reverse_art,
                    j < PAD_COUNT-1 ? "," : "");
        }
        fprintf(f, "  ]}%s\n", i < inst->preset_count-1 ? "," : "");
    }
    fprintf(f, "]\n");
    
    fclose(f);
    LOG("presets saved successfully");
}

/* ── Load preset into active slots ──────────────────────────────────────── */
static void load_preset_into_slots(expchords_t *inst, int idx) {
    if (idx < 0 || idx >= inst->preset_count) return;
    
    inst->active_preset = idx;
    for (int i = 0; i < PAD_COUNT; i++)
        inst->active_pad_slots[i] = inst->presets[idx].slots[i];
    
    LOG("loaded preset %d: %s", idx, inst->presets[idx].name);
}

/* ── Chord building ──────────────────────────────────────────────────────── */
static int build_chord(pad_slot_t *s, int input_note,
                       int out_notes[], int max_notes) {
    const chord_def_t *def = &CHORD_DEFS[s->type];
    int count = def->count < max_notes ? def->count : max_notes;
    int notes[MAX_CHORD_NOTES];
    int i;

    for (i = 0; i < count; i++)
        notes[i] = input_note + s->root + def->offs[i];

    /* Inversion: raise bottom N notes by octave */
    int inv = s->inversion < count ? s->inversion : count - 1;
    for (i = 0; i < inv; i++) notes[i] += 12;

    /* Sort ascending */
    for (i = 0; i < count - 1; i++) {
        int j;
        for (j = i+1; j < count; j++) {
            if (notes[j] < notes[i]) { int t=notes[i]; notes[i]=notes[j]; notes[j]=t; }
        }
    }

    /* Articulation: add octave above top */
    if (s->articulation && count < max_notes) {
        notes[count] = notes[count-1] + 12;
        count++;
    }

    /* Reverse articulation */
    if (s->reverse_art) {
        for (i = 0; i < count/2; i++) {
            int t = notes[i]; notes[i] = notes[count-1-i]; notes[count-1-i] = t;
        }
    }

    /* Strum direction: down = reverse order */
    if (s->strum_dir) {
        for (i = 0; i < count/2; i++) {
            int t = notes[i]; notes[i] = notes[count-1-i]; notes[count-1-i] = t;
        }
    }

    int out = 0;
    for (i = 0; i < count; i++)
        if (notes[i] >= 0 && notes[i] <= 127) out_notes[out++] = notes[i];
    return out;
}

/* ── Plugin callbacks ────────────────────────────────────────────────────── */
static void *create_instance(const char *module_dir, const char *config_json) {
    expchords_t *inst = calloc(1, sizeof(expchords_t));
    if (!inst) return NULL;
    
    memset(inst->held_out, -1, sizeof(inst->held_out));
    inst->current_pad  = 1;
    inst->active_preset = -1;
    inst->sample_rate  = 44100;
    if (module_dir)
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir)-1);
    
    load_presets(inst);
    
    /* Auto-load first preset */
    if (inst->preset_count > 0)
        load_preset_into_slots(inst, 0);
    
    LOG("create_instance called");
    (void)config_json;
    return inst;
}

static void destroy_instance(void *instance) {
    free(instance);
}

static int process_midi(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[], int max_out) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || in_len < 3) return 0;

    uint8_t status = in_msg[0] & 0xF0;
    uint8_t ch     = in_msg[0] & 0x0F;
    uint8_t note   = in_msg[1];
    uint8_t vel    = in_msg[2];
    int is_on  = (status == 0x90) && vel > 0;
    int is_off = (status == 0x80) || (status == 0x90 && vel == 0);

    /* Pad press: switch active pad */
    if (is_on && note >= 68 && note <= 99) {
        inst->current_pad = note - 68 + 1;
        LOG("pad pressed: %d", inst->current_pad);
    }

    if (is_on) {
        /* Release previous chord on this input note */
        if (inst->held_out_count[note] > 0) {
            memset(inst->held_out[note], -1, sizeof(inst->held_out[note]));
            inst->held_out_count[note] = 0;
        }
        
        int pad_idx = inst->current_pad - 1;
        pad_slot_t *s = &inst->active_pad_slots[pad_idx];
        int chord_notes[MAX_CHORD_NOTES];
        int count = build_chord(s, note, chord_notes, MAX_CHORD_NOTES);

        /* Strum timing */
        int strum_frames = 0;
        if (s->strum > 0 && inst->sample_rate > 0 && count > 1)
            strum_frames = (s->strum * inst->sample_rate) / (1000 * (count - 1));

        int out = 0, i;
        for (i = 0; i < count && out < max_out; i++) {
            int n = chord_notes[i];
            if (strum_frames > 0 && i > 0 && inst->pending_count < MAX_PENDING) {
                inst->pending[inst->pending_count++] = (pending_note_t){
                    (uint8_t)n, vel, ch, strum_frames * i
                };
            } else {
                out_msgs[out][0] = 0x90 | ch;
                out_msgs[out][1] = (uint8_t)n;
                out_msgs[out][2] = vel;
                out_lens[out++]  = 3;
            }
            if (i < MAX_CHORD_NOTES) inst->held_out[note][i] = (int8_t)n;
        }
        inst->held_out_count[note] = count;
        
        LOG("midi on: note=%d vel=%d chords=%d", note, vel, out);
        return out;
    }

    if (is_off) {
        int count = inst->held_out_count[note];
        if (count == 0) { 
            memcpy(out_msgs[0], in_msg, 3); 
            out_lens[0]=3; 
            return 1; 
        }
        
        int out = 0, i;
        for (i = 0; i < count && out < max_out; i++) {
            int n = (int)inst->held_out[note][i];
            if (n < 0 || n > 127) continue;
            out_msgs[out][0] = 0x80 | ch;
            out_msgs[out][1] = (uint8_t)n;
            out_msgs[out][2] = 0;
            out_lens[out++]  = 3;
        }
        
        memset(inst->held_out[note], -1, sizeof(inst->held_out[note]));
        inst->held_out_count[note] = 0;
        
        LOG("midi off: note=%d chords_off=%d", note, out);
        return out;
    }

    /* Pass through all other MIDI */
    if (in_len <= 3) { 
        memcpy(out_msgs[0], in_msg, in_len); 
        out_lens[0]=in_len; 
        return 1; 
    }
    
    LOG("pass_through: len=%d", in_len);
    return 0;
}

static int tick_fn(void *instance, int frames, int sample_rate,
                   uint8_t out_msgs[][3], int out_lens[], int max_out) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || inst->pending_count == 0) return 0;
    
    inst->sample_rate = sample_rate;
    int out = 0, remaining = 0, i;
    
    for (i = 0; i < inst->pending_count; i++) {
        pending_note_t *pn = &inst->pending[i];
        pn->delay_frames -= frames;
        if (pn->delay_frames <= 0 && out < max_out) {
            out_msgs[out][0] = 0x90 | pn->ch;
            out_msgs[out][1] = pn->note;
            out_msgs[out][2] = pn->vel;
            out_lens[out++]  = 3;
        } else {
            inst->pending[remaining++] = *pn;
        }
    }
    
    inst->pending_count = remaining;
    LOG("tick: pending=%d output=%d", inst->pending_count, out);
    return out;
}

static void set_param(void *instance, const char *key, const char *val) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || !key || !val) return;

    int pad_idx = inst->current_pad - 1;
    pad_slot_t *s = &inst->active_pad_slots[pad_idx];

    LOG("set_param: key=%s val=%s", key, val);

    if (strcmp(key, "preset") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->preset_count)
            load_preset_into_slots(inst, idx);
        return;
    }
    
    if (strcmp(key, "pad") == 0) {
        int v = atoi(val);
        if (v >= 1 && v <= PAD_COUNT) {
            inst->current_pad = v;
            /* s pointer is now stale — update it so subsequent writes go to correct pad */
            s = &inst->active_pad_slots[inst->current_pad - 1];
        }
        return;
    }
    
    if (strcmp(key, "save") == 0) {
        /* val is preset name to save as; empty/save = auto-name */
        char name[MAX_PRESET_NAME];
        if (strlen(val) == 0 || strcmp(val,"save") == 0 || strcmp(val,"1") == 0)
            snprintf(name, sizeof(name), "Preset %d", inst->preset_count + 1);
        else
            strncpy(name, val, MAX_PRESET_NAME - 1);
        
        name[MAX_PRESET_NAME-1] = '\0';

        /* Find existing or append */
        int target = -1;
        for (int i = 0; i < inst->preset_count; i++)
            if (strcmp(inst->presets[i].name, name) == 0) { 
                target = i; 
                break; 
            }
        
        if (target < 0) {
            if (inst->preset_count >= MAX_PRESETS) return;
            target = inst->preset_count++;
        }
        
        strncpy(inst->presets[target].name, name, MAX_PRESET_NAME-1);
        for (int i = 0; i < PAD_COUNT; i++)
            inst->presets[target].slots[i] = inst->active_pad_slots[i];
            
        save_presets(inst);
        return;
    }

    /* Per-pad params */
    if (strcmp(key,"root")==0)          s->root         = parse_enum(ROOT_NAMES,ROOT_COUNT,val);
    else if (strcmp(key,"type")==0)     s->type         = parse_enum(TYPE_NAMES,TYPE_COUNT,val);
    else if (strcmp(key,"inversion")==0)s->inversion    = parse_enum(INV_NAMES,INV_COUNT,val);
    else if (strcmp(key,"strum")==0)    s->strum        = clamp_i(atoi(val),0,100);
    else if (strcmp(key,"strum_dir")==0)s->strum_dir    = parse_enum(DIR_NAMES,DIR_COUNT,val);
    else if (strcmp(key,"articulation")==0) s->articulation = parse_enum(ART_NAMES,ART_COUNT,val);
    else if (strcmp(key,"reverse_art")==0)  s->reverse_art  = parse_enum(ART_NAMES,ART_COUNT,val);
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    expchords_t *inst = (expchords_t*)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;

    int pad_idx = inst->current_pad - 1;
    pad_slot_t *s = &inst->active_pad_slots[pad_idx];

    if (strcmp(key,"preset_count")==0)
        return snprintf(buf,buf_len,"%d",inst->preset_count);
    if (strcmp(key,"preset")==0)
        return snprintf(buf,buf_len,"%d",inst->active_preset);
    if (strcmp(key,"preset_name")==0) {
        int idx = inst->active_preset;
        if (idx >= 0 && idx < inst->preset_count)
            return snprintf(buf,buf_len,"%s",inst->presets[idx].name);
        return snprintf(buf,buf_len,"---");
    }
    if (strcmp(key,"preset_list")==0) {
        /* JSON array of preset names for UI browser */
        int pos = 0;
        pos += snprintf(buf+pos, buf_len-pos, "[");
        for (int i = 0; i < inst->preset_count && pos < buf_len-4; i++) {
            if (i > 0) pos += snprintf(buf+pos, buf_len-pos, ",");
            pos += snprintf(buf+pos, buf_len-pos, "\"%s\"", inst->presets[i].name);
        }
        pos += snprintf(buf+pos, buf_len-pos, "]");
        return pos;
    }
    if (strcmp(key,"pad")==0)
        return snprintf(buf,buf_len,"%d",inst->current_pad);
    if (strcmp(key,"root")==0)
        return snprintf(buf,buf_len,"%s",ROOT_NAMES[s->root]);
    if (strcmp(key,"type")==0)
        return snprintf(buf,buf_len,"%s",TYPE_NAMES[s->type]);
    if (strcmp(key,"inversion")==0)
        return snprintf(buf,buf_len,"%s",INV_NAMES[s->inversion]);
    if (strcmp(key,"strum")==0)
        return snprintf(buf,buf_len,"%d",s->strum);
    if (strcmp(key,"strum_dir")==0)
        return snprintf(buf,buf_len,"%s",DIR_NAMES[s->strum_dir]);
    if (strcmp(key,"articulation")==0)
        return snprintf(buf,buf_len,"%s",ART_NAMES[s->articulation]);
    if (strcmp(key,"reverse_art")==0)
        return snprintf(buf,buf_len,"%s",ART_NAMES[s->reverse_art]);
    
    /* pad_label: same as preset_name but with trailing spaces encoding the pad
       number — invisible to the user but changes value on pad press so the
       Shadow UI detects the change and redraws all params */
    if (strcmp(key,"pad_label")==0) {
        int idx = inst->active_preset;
        const char *pname = (idx >= 0 && idx < inst->preset_count)
                            ? inst->presets[idx].name : "---";
        /* Append pad number of spaces (1-32) — invisible but unique per pad */
        int n = snprintf(buf, buf_len, "%s", pname);
        int i;
        for (i = 0; i < inst->current_pad && n < buf_len - 1; i++)
            buf[n++] = ' ';
        buf[n] = '\0';
        return n;
    }
    if (strcmp(key,"state")==0)
        return snprintf(buf,buf_len,
            "{\"current_pad\":%d,\"root\":\"%s\",\"type\":\"%s\","
            "\"inversion\":\"%s\",\"strum\":%d,"
            "\"strum_dir\":\"%s\",\"articulation\":\"%s\","
            "\"reverse_art\":\"%s\"}",
            inst->current_pad,
            ROOT_NAMES[s->root], TYPE_NAMES[s->type],
            INV_NAMES[s->inversion], s->strum,
            DIR_NAMES[s->strum_dir], ART_NAMES[s->articulation],
            ART_NAMES[s->reverse_art]);

    return -1;
}

/* ── Entry point (per midi_fx_api_v1 spec) ──────────────────────────────── */
static midi_fx_api_v1_t g_api = {
    .api_version      = MIDI_FX_API_VERSION,
    .create_instance  = create_instance,
    .destroy_instance = destroy_instance,
    .process_midi     = process_midi,
    .tick             = tick_fn,              /* Per spec: must be named 'tick' */
    .set_param        = set_param,
    .get_param        = get_param,
};

midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host) {
    (void)host;
    return &g_api;
}

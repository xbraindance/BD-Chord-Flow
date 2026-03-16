/**
 * ui.js — Expressive Chords
 *
 * Thin UI over dsp.so. DSP handles all chord logic, preset storage,
 * pad slots, strum, articulation. UI handles display + input only.
 *
 * Screens:
 *   'browser'  — preset browser (default)
 *   'edit'     — per-pad chord settings editor
 */

import { shouldFilterMessage } from '../../shared/input_filter.mjs';

// ─── CC constants ─────────────────────────────────────────────────────────────
const CC_JOG   = 14;
const CC_CLICK = 3;
const CC_BACK  = 51;
const CC_SHIFT = 49;
const CC_UP    = 46;
const CC_DOWN  = 47;
const CC_LEFT  = 62;
const CC_RIGHT = 63;

// ─── Display ──────────────────────────────────────────────────────────────────
const SCREEN_W = 128;
const HEADER_Y = 1;
const FOOTER_Y = 56;
const CENTER_Y = 32;
const LIST_TOP = 13;
const LIST_BOT = 52;
const ROW_H    = 9;
const VISIBLE  = Math.floor((LIST_BOT - LIST_TOP) / ROW_H); // 4

// ─── Edit rows ────────────────────────────────────────────────────────────────
const EDIT_KEYS   = ['pad','root','type','inversion','strum','strum_dir','articulation','reverse_art'];
const EDIT_LABELS = ['Pad','Root','Chord Type','Inversion','Strum','Strum Dir','Articulation','Reverse Art'];
const EDIT_ENUMS  = {
    root:         ['c','c#','d','d#','e','f','f#','g','g#','a','a#','b'],
    type:         ['maj','min','dom7','maj7','min7','sus2','sus4','add9','min9','maj9','dim','aug','5th','6th','min6','dom9'],
    inversion:    ['root','1st','2nd','3rd'],
    strum_dir:    ['up','down'],
    articulation: ['off','on'],
    reverse_art:  ['off','on'],
};

// ─── State ────────────────────────────────────────────────────────────────────
let screen      = 'browser';
let needsRedraw = true;
let shiftHeld   = false;

// Browser
let presetIndex = 0;
let presetCount = 0;
let presetName  = '';

// Edit
let editRow  = 0;
let editVals = {};

// ─── Helpers ──────────────────────────────────────────────────────────────────
function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function trunc(s, n) { return s.length > n ? s.substring(0, n - 1) + '~' : s; }

// ─── DSP communication ────────────────────────────────────────────────────────
function dspGet(key) {
    return host_module_get_param(key) || '';
}

function dspSet(key, val) {
    host_module_set_param(key, String(val));
}

function refreshPreset() {
    presetCount = parseInt(dspGet('preset_count')) || 0;
    presetIndex = parseInt(dspGet('preset'))       || 0;
    presetName  = dspGet('preset_name');
}

function selectPreset(idx) {
    if (idx < 0) idx = presetCount - 1;
    if (idx >= presetCount) idx = 0;
    dspSet('preset', idx);
    presetIndex = idx;
    presetName  = dspGet('preset_name');
    needsRedraw = true;
}

function readEditVals(knownPad) {
    // Read all pad params from DSP — DSP returns values for current active pad
    editVals = {};
    for (const k of EDIT_KEYS) {
        editVals[k] = dspGet(k);
    }
    // If caller knows the pad number for certain, use it directly
    // (guards against DSP not yet reflecting the switch)
    if (knownPad !== undefined) {
        editVals['pad'] = String(knownPad);
    }
}

function cycleVal(delta) {
    const key = EDIT_KEYS[editRow];
    if (!key) return;
    const cur = editVals[key] || '';

    if (EDIT_ENUMS[key]) {
        // Clamp to ±1: jog sends large relative values (e.g. 2, 4, 16…) which
        // would wrap enums with 12 or 16 options back to the same position.
        const step = delta > 0 ? 1 : -1;
        const opts = EDIT_ENUMS[key];
        let i = opts.indexOf(cur);
        if (i < 0) i = 0;
        i = ((i + step) + opts.length) % opts.length;
        dspSet(key, opts[i]);
        editVals[key] = opts[i];
    } else {
        const n = clamp((parseInt(cur) || 0) + delta,
                        key === 'pad' ? 1 : 0,
                        key === 'pad' ? 32 : 100);
        dspSet(key, n);
        editVals[key] = String(n);
        // Pad change: re-read all values from DSP for the new pad.
        // Keep our optimistic pad value in case DSP state lags.
        if (key === 'pad') {
            readEditVals(n);
        }
    }
    needsRedraw = true;
}

// ─── Drawing ──────────────────────────────────────────────────────────────────
function drawBrowserScreen() {
    clear_screen();
    print(1, HEADER_Y, trunc('EX: Expressive Chords', 22), 1);
    draw_rect(0, 11, SCREEN_W, 1, 1);

    if (presetCount > 0) {
        const num   = `${presetIndex + 1} / ${presetCount}`;
        const numX  = Math.floor((SCREEN_W - num.length * 6) / 2);
        print(numX, CENTER_Y - 8, num, 1);

        const name  = trunc(presetName || '---', 21);
        const nameX = Math.floor((SCREEN_W - name.length * 6) / 2);
        print(nameX, CENTER_Y + 4, name, 1);

        print(4,            CENTER_Y - 2, '<', 1);
        print(SCREEN_W - 8, CENTER_Y - 2, '>', 1);
    } else {
        print(4, CENTER_Y, 'No presets', 1);
    }

    draw_rect(0, 53, SCREEN_W, 1, 1);
    print(1,  FOOTER_Y, 'Clk:load+edit', 1);
    print(84, FOOTER_Y, 'Jog:browse', 1);
    host_flush_display();
    needsRedraw = false;
}

function drawEditScreen() {
    clear_screen();
    const pad = editVals['pad'] || '1';
    print(1, HEADER_Y, trunc(`EX > Pad ${pad}`, 22), 1);
    draw_rect(0, 11, SCREEN_W, 1, 1);

    const scroll = clamp(
        editRow - Math.floor(VISIBLE / 2),
        0, Math.max(0, EDIT_KEYS.length - VISIBLE)
    );

    for (let i = 0; i < VISIBLE; i++) {
        const idx  = scroll + i;
        if (idx >= EDIT_KEYS.length) break;
        const y    = LIST_TOP + i * ROW_H;
        const sel  = (idx === editRow);
        const lbl  = EDIT_LABELS[idx];
        const val  = String(editVals[EDIT_KEYS[idx]] || '?');
        const valX = SCREEN_W - val.length * 6 - 3;

        if (sel) {
            draw_rect(0, y, SCREEN_W, ROW_H - 1, 1);
            print(4,    y + 1, lbl, 0);
            print(valX, y + 1, val, 0);
        } else {
            print(4,    y + 1, lbl, 1);
            print(valX, y + 1, val, 1);
        }
    }

    // Scroll dot
    if (EDIT_KEYS.length > 1) {
        const dotY = LIST_TOP + Math.floor(
            editRow * (LIST_BOT - LIST_TOP - 3) / (EDIT_KEYS.length - 1)
        );
        draw_rect(125, dotY, 2, 3, 1);
    }

    draw_rect(0, 53, SCREEN_W, 1, 1);
    print(1,  FOOTER_Y, 'Jog:val', 1);
    print(48, FOOTER_Y, 'Up/Dn:row', 1);
    print(108, FOOTER_Y, 'Bk', 1);
    host_flush_display();
    needsRedraw = false;
}

function redraw() {
    if (screen === 'edit') { drawEditScreen(); return; }
    drawBrowserScreen();
}

// ─── Input ────────────────────────────────────────────────────────────────────
function handleJogTurn(delta) {
    if (screen === 'browser') {
        selectPreset(presetIndex + delta);
    } else if (screen === 'edit') {
        cycleVal(delta);
    }
}

function handleClick() {
    if (screen === 'browser') {
        // Load this preset into the current active pad, then open edit
        dspSet('preset', presetIndex);
        readEditVals();
        editRow     = 0;
        screen      = 'edit';
        needsRedraw = true;
    }
}

function handleBack() {
    if (screen === 'edit') {
        screen      = 'browser';
        refreshPreset();
        needsRedraw = true;
    } else {
        host_return_to_menu();
    }
}

function handleUpDown(dir) {
    if (screen === 'edit') {
        editRow = clamp(editRow + dir, 0, EDIT_KEYS.length - 1);
        needsRedraw = true;
    } else {
        selectPreset(presetIndex + dir);
    }
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────
globalThis.init = function() {
    refreshPreset();
    screen      = 'browser';
    needsRedraw = true;
};

globalThis.tick = function() {
    // Poll DSP's current_pad while in edit mode. This catches pad switches that
    // come through process_midi (note-on 68-99) regardless of whether
    // onMidiMessageInternal also fires for them.
    if (screen === 'edit') {
        const dspPad = dspGet('pad');
        if (dspPad && dspPad !== editVals['pad']) {
            readEditVals(parseInt(dspPad));
            needsRedraw = true;
        }
    }
    if (needsRedraw) redraw();
};

globalThis.onMidiMessageInternal = function(data) {
    if (shouldFilterMessage(data)) return;
    const status = data[0], d1 = data[1], d2 = data[2];

    if (status === 0xB0 && d1 === CC_SHIFT)            { shiftHeld = d2 > 0;           return; }
    if (status === 0xB0 && d1 === CC_BACK  && d2 > 0)  { handleBack();                  return; }
    if (status === 0xB0 && d1 === CC_CLICK && d2 > 0)  { handleClick();                 return; }
    if (status === 0xB0 && d1 === CC_UP    && d2 > 0)  { handleUpDown(-1);              return; }
    if (status === 0xB0 && d1 === CC_DOWN  && d2 > 0)  { handleUpDown(1);               return; }
    if (status === 0xB0 && d1 === CC_LEFT  && d2 > 0)  { handleJogTurn(-1);             return; }
    if (status === 0xB0 && d1 === CC_RIGHT && d2 > 0)  { handleJogTurn(1);              return; }
    if (status === 0xB0 && d1 === CC_JOG)              { handleJogTurn((d2 <= 64) ? d2 : d2 - 128); return; }

    // Pad press — tell DSP to switch active pad, refresh edit if open
    if ((status & 0xF0) === 0x90 && d2 > 0 && d1 >= 68 && d1 <= 99) {
        const padNum = d1 - 68 + 1;
        dspSet('pad', padNum);        // synchronously update DSP's current_pad
        if (screen === 'edit') {
            readEditVals(padNum);      // pass padNum so header is always correct
            needsRedraw = true;
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {
    if (shouldFilterMessage(data)) return;
    const status = data[0], d1 = data[1], d2 = data[2];
    if ((status & 0xF0) === 0x90 && d2 > 0 && d1 >= 68 && d1 <= 99) {
        const padNum = d1 - 68 + 1;
        dspSet('pad', padNum);
        if (screen === 'edit') { readEditVals(padNum); needsRedraw = true; }
    }
};

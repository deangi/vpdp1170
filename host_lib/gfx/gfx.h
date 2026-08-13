#pragma once
#ifndef HOST_LIB_GFX_H
#define HOST_LIB_GFX_H

// Board-selected 2D graphics backend (approach B: one concrete GfxDisplay type
// chosen at compile time; call sites stay shared, with a few #ifdefs only where
// the two libraries genuinely differ — init, RGB writeback, and the font cell).
//
//   Freenove 2.8" : TFT_eSPI (SPI ILI9341)          -> GfxDisplay == TFT_eSPI
//   CrowPanel 7"  : LovyanGFX (RGB Panel_RGB/GT911) -> GfxDisplay == LGFX
//
// LovyanGFX is a near drop-in for the TFT_eSPI method surface this project uses
// (fillRect, drawString, pushImage, setTextDatum, TFT_* colors, *_DATUM, ...),
// so the shared drawing code compiles against either type unchanged.

#include "config.h"

#if VPDP_DISPLAY_BACKEND == VPDP_DISPLAY_TFT_ESPI

#include <TFT_eSPI.h>
using GfxDisplay = TFT_eSPI;

// SPI panels push straight to the controller; nothing to flush.
static inline void gfx_writeback(GfxDisplay&) {}
static inline void gfx_writeback(GfxDisplay&, int, int, int, int) {}

#elif VPDP_DISPLAY_BACKEND == VPDP_DISPLAY_LOVYANGFX

#include "lgfx_conf.h"
using GfxDisplay = LGFX;

// RGB framebuffer lives in PSRAM and is scanned by LCD DMA. Drawing updates the
// buffer; display() flushes the CPU cache for the dirty region so DMA sees it.
// Keep writebacks region-bounded to stay within PSRAM bandwidth (see bring-up).
static inline void gfx_writeback(GfxDisplay& g) { g.display(); }
static inline void gfx_writeback(GfxDisplay& g, int x, int y, int w, int h) {
  g.display(x, y, w, h);
}

#else
#error "Unknown VPDP_DISPLAY_BACKEND (set via board_*.h)"
#endif

#endif  // HOST_LIB_GFX_H

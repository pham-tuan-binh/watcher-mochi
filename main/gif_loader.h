#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Scan the SD card, preload blank.gif, and start the background loader task.
/// Returns the number of GIF files found (excluding blank.gif).
int gif_loader_init(void);

/// Get the preloaded blank.gif data. Returns NULL if blank.gif was not found.
const uint8_t *gif_loader_get_blank(size_t *out_size);

/// Request the loader to read a random GIF in the background.
/// Calls `cb(data, size)` from the loader task once ready.
/// Ignored if no GIF files were found.
void gif_loader_request(void (*cb)(uint8_t *data, size_t size));

#ifdef __cplusplus
}
#endif

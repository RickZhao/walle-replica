/**
 * AUDIO PLAYER (PCM5102 I2S output)
 *
 * @file    audio_player.hpp
 * @brief   Plays the wav sound effects from LittleFS through the
 *          PCM5102 I2S DAC (third-party BOM audio chain:
 *          PCM5102 -> PAM8406 amp -> speakers). Replaces the
 *          aplay-based playback of the Raspberry Pi version.
 *
 * Requires the ESP32-audioI2S library (schreibfaul1). Playback runs
 * through I2S DMA and does not block the 10ms servo control loop.
 */

#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

/// Initialise the I2S output (pins from web_config.h) and set the
/// default volume. Call once from setup() after LittleFS is mounted
/// (webServerInit mounts it).
void audioPlayerInit();

/// Feed the decoder. Call on every iteration of loop().
void audioPlayerLoop();

/// Play a sound clip from /static/sounds/<clip>.wav
/// @param  clip  Clip base name (letters, digits, '-' and '_' only)
/// @return true when playback started, false when the name is invalid
///         or the file does not exist
bool audioPlayClip(const char *clip);

/// Set playback volume (web slider range 0-10, mapped to the
/// library range 0-21).
void audioSetVolume(int volume);

/// @return true while a clip is playing
bool audioIsPlaying();

#endif /* AUDIO_PLAYER_HPP */

/**
 * AUDIO PLAYER (PCM5102 I2S output)
 *
 * @file    audio_player.cpp
 * @brief   Implementation, see audio_player.hpp
 */

#include <Audio.h>          // ESP32-audioI2S library (schreibfaul1)
#include <LittleFS.h>
#include "web_config.h"     // I2S pin definitions
#include "audio_player.hpp"


static Audio audio;

static int currentVolume = 8;   // Web slider range 0-10, matches the
                                // default slider position in index.html


void audioPlayerInit() {
	// PCM5102: BCK = bit clock, LRC = word select, DIN = data in
	audio.setPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
	audioSetVolume(currentVolume);
	Serial.println(F("Audio player started (I2S)"));
}


void audioPlayerLoop() {
	audio.loop();
}


bool audioPlayClip(const char *clip) {

	// Only allow plain file names - no path traversal
	if (clip == nullptr || clip[0] == '\0') return false;
	for (const char *c = clip; *c != '\0'; c++) {
		char ch = *c;
		bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
		if (!ok) return false;
	}

	char path[64];
	snprintf(path, sizeof(path), "/static/sounds/%s.wav", clip);

	if (!LittleFS.exists(path)) {
		Serial.print(F("Sound clip not found: "));
		Serial.println(path);
		return false;
	}

	return audio.connecttoFS(LittleFS, path);
}


void audioSetVolume(int volume) {
	if (volume < 0) volume = 0;
	if (volume > 10) volume = 10;
	currentVolume = volume;
	audio.setVolume(volume * 2);    // Library range is 0-21
}


bool audioIsPlaying() {
	return audio.isRunning();
}

#pragma once
// Once-a-minute power log for the Kindle (see KindlePowerLog.cpp).

void kindlePowerLogFrame();               // once per main-loop pass
void kindlePowerLogTick();                // from HalGPIO::update(); logs when due
void kindlePowerLogNote(const char* what);  // one line now (e.g. before sleep)

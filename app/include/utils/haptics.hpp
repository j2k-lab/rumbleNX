#pragma once

namespace rumblenx::haptics {

float keyboardIntensity();
bool setKeyboardIntensity(float intensity);

void keyboardPulse();
void stop();

}  // namespace rumblenx::haptics

#pragma once
#include <Arduino.h>

// ================================================================
//  PigWorld - the scrolling-world cartoon on the Pig page (4), on the
//  128x64 two-colour OLED. The blue area is the world (clouds, birds,
//  far hills, trees, rolling ground, butterfly or fireflies, truffle
//  hunts); the yellow band is the sky above it, with the sun or the
//  moon and stars by local solar time, and fireworks.
//
//  GPS drives the sky (solar time from position and GPS time) and the
//  pace: the trot turns into a gallop with speed (dust from 12 km/h,
//  speed lines from 50 km/h).
// ================================================================

// Render the next frame into the display buffer. Call every loop() while
// on the Pig page; it paces itself to 20 fps. Returns true when a frame was
// drawn, and the caller pushes it with display.display().
bool pigWorldTick();

// Fireworks and a happy hop (double press on the Pig page).
void pigWorldCelebrate();

Based on the wonderful insect_box by octavebrand (https://github.com/octavebrand/insect_box)

I hit upon an issue with the Waveshare 2040 and 2350 plus boards - I'm unsure if this affects other RP boards.

Simply, the encoder doesn't work. When the SD card reader is removed, the encoder works perfectly. It seems the microcontroller struggles to keep up with the file IO demands and listening to the encoder pulses. The RP2350 is dual core, so I reworked the original code to make use of the second core. One core is now used for the controls, the other core for playback, allowing non-blocking interfacing. It also opens up scope for an OLED etc for different UI.



DAC Wiring Notes:

I've had a lot of noise and EMC weirdness from the DAC with the wiring. It might be the specific PCB, I don't know, but I found it became almost capacitive sensitive, and would mute when moving any object close to the DAC.

I've since soldered all of the grounded terminals on the board together, and that solved a lot to begin, the FLT pin also needs grounding and solved the capacitance issue. But there was still ringing sounds and noise. Accidentally at one stage, I had the ground disconnected from the DAC, and it just worked so much better. Most of the noise went. Ringing remained.

Secondly, the DAC I used has VCC and 3.3V. I was sending 3.3V into VCC or 3.3V and nothing changed. I then realised it has a regulator, and used the VBUS into the VCC. IT WORKS PERFECTLY. Like, no noise, no ringing, the audio is perfect.

I know there's a ground loop through the Amp so breaking that will improve, but it's confusing to say the least. Now, the DAC is grounded via the audio path and via the amp. If I disconnect the amp and use the DAC for ground, it hates it. Likewise I've changed it around a few ways, including the power supply, and the DAC grounding only via the amp works perfectly.

I suspect things would change if I used an external power supply, but using the RP2040 (a Waveshare Plus specifically), the 3.3V is powering the controls and LED, the VBUS powers the DAC (pre-regulated, but DAC regulates) and VSYS powers the amp (post-regulated cleaner). I'm grounding them via the onboard ground, as now I have enough ground pins spare (after soldering all of the DAC pins together).

It's weird, but it actually works.

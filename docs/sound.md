# Sound

Both Toons can play sounds: WAV files, tones and beeps. The command and the
volume setting are the same on both; what you hear is not, because the
hardware differs.

| | Toon 1 | Toon 2 |
|---|---|---|
| Sound chip | TI TLV320AIC3111 audio codec | none |
| Output | a small speaker on the codec's class-D amplifier | the buzzer, driven by the Cortex-M4 as a DAC |
| Played at | 48 kHz, 16 bit | 16 kHz, 8 bit |
| Volume when not set | 30 % | 70 % (about as loud as the Toon 1 at 30 %) |
| Sounds like | a small speaker | thinner; loudest for high tones, little below 1 kHz |

## Playing sounds

```
toon-sound play file.wav      # PCM WAV, 8 or 16 bit, mono or stereo, any rate
toon-sound tone 440 500       # a tone: frequency in Hz, length in ms
toon-sound beep               # a short beep
toon-sound test               # a three-note chime
toon-sound -v 40 beep         # with its own volume (0..100)
```

`beep` is a soft 2.4 kHz tone on both Toons. On the Toon 2 the test chime is
played two octaves up, where the buzzer is loud; WAV files and `tone` play as
given, so a sound meant for the buzzer is best kept between 1 and 4 kHz.

One sound plays at a time; a second call waits for the first. The volume is
set in the web interface, System, Sound (stored as `volume=` in
`config/sound.conf`), which also has a button to play the test sound.

## How it works

**Toon 1.** The codec gets a 11.77 MHz master clock from the TOUT pin of
the i.MX27's timer 1 (the system timer is timer 2 under ReToon) and makes the audio clocks itself with its PLL. The
SSI is fed by a FIQ handler; the audio mux connects it to the codec's pins.
The drivers are modules (the kernel has to fit its 3 MiB flash partition);
`S03toon-audio` loads them and routes the DAC to the speaker. `toon-sound`
plays through `aplay`.

**Toon 2.** The buzzer hangs on PWM4. The Cortex-M4 runs that PWM at a
32 kHz carrier and sets its duty cycle for every sample: 16000 samples a
second, paced by the PWM's own FIFO. Linux writes 8-bit samples into a
4 KiB ring buffer in the M4's memory; when nothing plays the output slides
to 0 % duty, so no current flows through the buzzer coil between sounds.
`toon-m4d` switches this on whenever Linux does not hold PWM4 (the
`m4-no-sound` file in `config/` turns it off).

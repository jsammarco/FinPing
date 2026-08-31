# FinPing

FinPing is an experimental Flipper Zero DTMF-over-RF transmitter using the internal CC1101. It sends DTMF tones and an optional WAV audio preamble; RF reception and DTMF decoding are not included.

## What it is doing

The CC1101 does not provide a normal microphone/audio input for analog FM. FinPing therefore uses the CC1101's asynchronous **2-FSK, +/-2.38 kHz** mode as a one-bit RF DAC.

The app generates the two standard DTMF sine waves, mixes them, converts the result to a 40 ksample/s pulse-density stream, and feeds that stream into the CC1101 asynchronous modulation input. It can use the same path to transmit a selected PCM WAV file before the DTMF message. WAV audio is downmixed, resampled to 8 kHz with low-pass averaging, smoothed for voice transmission, and peak-normalized separately from DTMF. This avoids nonlinear audio processing that could resemble a DTMF keypress. An FM receiver's IF/demodulator/audio filtering may average the fast +/- frequency switching into an audible waveform.

That means this is experimental. It may work well, poorly, or not at all depending on the receiver, filtering, firmware, signal level, and the CC1101's behavior in asynchronous mode.

## Features

- Main menu: **Send RF**, **Message**, **Frequency**, **Interval**, and **Call Sign Audio**
- Full 4x4 DTMF message keypad: 0-9, A-D, `*`, `#`
- **P** button for a 1-second pause and **LP** for a 3-second pause
- On-screen **OK** button to finish message editing
- Message length up to 32 symbols
- Digit-by-digit frequency editor from 387-464 MHz
- Persistent frequency setting and last-entered message
- Persistent interval selection: One Shot, or every 1, 2, 5, 10, 30, or 60 minutes
- Optional Call Sign Audio WAV preamble sent over RF before the DTMF message
- Persistent WAV-file selection
- Clear on-screen transmit status and error messages
- Cancel transmission with Back
- Regional transmit restrictions are respected
- Internal CC1101 only
- No external hardware required for the experimental mode

## Controls

### Main menu

- Up/Down: select Send RF, Message, Frequency, Interval, or Call Sign Audio
- OK on Send RF: transmit the optional WAV preamble, then the current DTMF message. With an interval selected, this starts the repeating schedule after the first send.
- OK on Message/Frequency: open that editor
- OK on Interval: choose One Shot or a repeat interval
- OK on Call Sign Audio: choose a WAV file from the SD card
- Left on Call Sign Audio: clear the selected WAV file
- Left on Send RF: stop a pending repeating schedule
- Back: exit the app
- During TX, Back cancels the transmission and its repeating schedule

### Message editor

- Arrow keys: select a DTMF symbol
- OK: append the selected symbol
- Select **P** to append a 1-second pause, or **LP** for a 3-second pause
- Select the on-screen **OK** button to return to the main menu
- Back or the **DEL** button: delete the last symbol
- Hold Back: return to the main menu

The message is saved as it is edited and is restored when the app is reopened.

### Frequency editor

- Left/Right: select a frequency digit
- Up/Down: increase or decrease that digit
- OK: save the frequency and return to the menu
- Back: discard changes and return to the menu

### Interval editor

- Up/Down: select One Shot, or every 1, 2, 5, 10, 30, or 60 minutes
- OK: save the selection and return to the menu
- Back: discard changes and return to the menu

The interval is only armed when you select **Send RF**. It repeats the complete configured transmission, including any Call Sign Audio preamble, while FinPing remains open on its main menu. If an editor is open when a send is due, it starts after you return to the menu. Choose **One Shot** for a single transmission.

### Audio preamble

The audio file is transmitted over the same experimental RF signal immediately before the DTMF message. With Call Sign Audio selected, the app sends a 600 ms steady-carrier key-up period before the audio, then holds a steady carrier for another 600 ms afterward (plus the normal 80 ms DTMF gap when a message follows). The app does not use a PDM pattern during these silent periods, preventing an audible idle tone. This gives a repeater time to key up and stays on-air between the call sign and DTMF. It is not played through the Flipper speaker.

Use an uncompressed PCM WAV file with these limits:

- Mono or stereo (stereo is downmixed to mono)
- 8-bit unsigned or 16-bit signed PCM
- 8,000–48,000 Hz source sample rate (files above 8 kHz are downsampled)
- Up to 80,000 post-conversion samples (10 seconds at 8 kHz)

MP3 files are not supported directly. Convert an MP3 with FFmpeg, for example:

```bash
ffmpeg -i callsign.mp3 -ac 1 -ar 8000 -c:a pcm_s16le callsign.wav
```

Copy the WAV to `/ext/apps_data/finping/audio/` on the Flipper SD card, select it under **Call Sign Audio**, then use **Send RF**. The app creates this folder on its first launch.

## Current scope

- Implemented: DTMF tone generation, optional PCM WAV preamble, and experimental RF transmission.
- Not implemented: RF receive mode, audio demodulation, and received-DTMF decoding.

The app is deliberately TX-only; its menu and documentation make that scope explicit.

## Default RF setup

Default frequency: **433.920 MHz**.

The app allows selection within 387-464 MHz. Your firmware/region configuration can still block transmission on frequencies that the Flipper is not allowed to transmit on. The app does not bypass that protection.

## UV-K5 test setup

1. Tune the UV-K5 to the exact same frequency as the Flipper.
2. Use FM/NFM receive mode.
3. Disable CTCSS/DCS squelch for the initial test.
4. Turn on the firmware's DTMF decoder.
5. If your firmware has a live DTMF display option, turn that on as well.
6. Start with a simple sequence such as `123#`.
7. On the Flipper, select **Send RF** and press OK.

If the UV-K5 opens squelch but does not decode digits:

- Try NFM and FM on the UV-K5.
- Keep the radios a few feet apart to avoid receiver overload.
- If very close, reduce RF coupling by increasing distance or removing one antenna for a bench test where appropriate.
- Listen to the demodulated audio first. You should hear recognizable dual tones.

## Building

### uFBT

Install uFBT, then run from this project directory:

```bash
build.bat
```

or:

```bash
py -m ufbt
```

The generated file is normally `dist/finping.fap`.

To upload and launch on a connected Flipper:

```bash
py -m ufbt launch
```

### Full Flipper firmware tree

Copy this folder under:

```text
flipperzero-firmware/applications_user/finping
```

Then from the firmware root:

```bash
./fbt fap_finping
```

or launch directly:

```bash
./fbt launch APPSRC=applications_user/finping
```

## Compatibility note

Flipper external apps are tied to firmware API versions. If the FAP reports an API mismatch, rebuild it against the firmware version currently installed on the Flipper.

This source targets the current-style Sub-GHz device API:

- `subghz_devices_init`
- internal CC1101 device lookup
- `FuriHalSubGhzPreset2FSKDev238Async`
- asynchronous TX callback returning `LevelDuration`

## Why FM238?

The built-in `FuriHalSubGhzPreset2FSKDev238Async` switches the carrier between approximately +/-2.38 kHz deviation according to the asynchronous data input. The PDM stream varies the proportion of time spent at each frequency. A receiving FM discriminator acts as the first stage of reconstruction.

For an input sample `x` in roughly -1 to +1:

```text
P(HIGH) = (1 + x) / 2
average deviation ~= 2.38 kHz * x
```

The current build uses a fixed 80% modulation level and 120 ms tone / 80 ms inter-digit timing.

## Important RF/legal note

Only transmit where you are authorized to transmit and with emissions permitted for that service. A frequency being tunable by the hardware does not itself make transmission legal. Amateur-radio identification and operating rules still apply when transmitting under an amateur license.

For initial development, short local bench tests into a nearby receiver are the best approach.

## If the experimental TX signal is not clean enough

The UI and DTMF engine in this project are reusable. A later hardware-backed mode can route the same generated DTMF waveform to a real analog-FM transmitter or radio microphone input while keeping the same keypad, presets, and timing settings. That would be substantially more predictable than PDM-over-2-FSK.

# FinPing

FinPing is an experimental Sub-GHz transmitter for sending DTMF messages with an optional WAV audio preamble. It uses the Flipper Zero internal CC1101 radio in asynchronous 2-FSK mode.

## Features

- Send DTMF symbols, pauses, and an optional PCM WAV call-sign preamble
- Select a transmit frequency from 387 to 464 MHz
- Repeat a configured transmission every 1, 2, 5, 10, 30, or 60 minutes
- Save the selected frequency, message, audio file, and interval on the device

## Notes

- FinPing is transmit-only and does not receive or decode DTMF.
- Transmissions remain subject to the Flipper region configuration and local radio regulations.
- The asynchronous 2-FSK audio method is experimental and receiver results may vary.

# Third-party software notices

Cloud Gaming Host redistributes the following runtime components. Each remains
subject to its upstream license; inclusion does not change the license of this
application.

| Component | Project | License |
|---|---|---|
| FFmpeg libraries | https://ffmpeg.org/ | LGPL 2.1+ or the terms used by the supplied build |
| Pion WebRTC runtime | https://github.com/pion/webrtc | MIT |
| Opus codec | https://opus-codec.org/ | BSD 3-Clause |
| OpenSSL | https://www.openssl.org/ | Apache License 2.0 |
| Microsoft Visual C++ runtime | https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist | Microsoft Software License Terms |

The release owner must verify the exact FFmpeg build configuration before
distribution. In particular, a GPL-enabled or non-free FFmpeg build can impose
different redistribution obligations. Corresponding license texts and any
required source offer must be added to the release payload before publication.

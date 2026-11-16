# thumb25

A Windows Explorer thumbnail handler that shows the frame from **25% into a video** instead of the first frame.

- Works for practically every video format (mp4, mkv, webm, avi, wmv, ts, flv, mpg, vob, rmvb, ...), decoded with [FFmpeg](https://ffmpeg.org).
- Fast: decodes a single keyframe and scales it straight into the thumbnail.
- Works in file views and in folder previews.
- Optional: removes the film-strip borders and the app icon Explorer draws on video thumbnails.

## Install

1. Download the latest release and extract it into one folder.
2. Run `thumb25-setup.exe` and click **Install** (asks for admin rights once).

Setup copies the files to `C:\Program Files\thumb25`, registers the handler, restarts Explorer and clears the thumbnail cache.
To remove it, run `thumb25-setup.exe` and click **Uninstall**: Windows' own video thumbnails come back.

## Build

Requires Visual Studio Build Tools (C++ workload) and the FFmpeg `lgpl-shared` win64 build from
[FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) extracted into `ffmpeg_dl\`. Then run `build.cmd`.
The CI workflow in `.github/workflows/build.yml` does exactly this.

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org) (application pending).

- Committers and reviewers: [@shad0w-xx](https://github.com/shad0w-xx)
- Approvers: [@shad0w-xx](https://github.com/shad0w-xx)

Only `thumb25.dll` and `thumb25-setup.exe` are built from this repository and signed. The FFmpeg DLLs are
unmodified upstream binaries from FFmpeg-Builds and are shipped unsigned.

## Privacy

This program will not transfer any information to other networked systems unless specifically requested by the
user or the person installing or operating it.

## License

thumb25 is MIT licensed. FFmpeg is licensed under the LGPL; its license is included in releases as `FFMPEG-LICENSE.txt`,
and its source code is available from [ffmpeg.org](https://ffmpeg.org) and [FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds).

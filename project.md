# conveyor

## Current Status

Created a minimal ESP-IDF project skeleton in this folder.

## Files

- `CMakeLists.txt`: Top-level ESP-IDF project file.
- `main/CMakeLists.txt`: Main component build file.
- `main/main.c`: Simple `app_main` with a toggleable heartbeat loop.
- `sdkconfig.defaults`: Default log level config.
- `.gitignore`: Ignores ESP-IDF build outputs and local config files.
- `project.md`: Project status notes for future chats.

## Assumptions

- Project name is `conveyor`.
- No specific ESP32 target was selected yet.
- The first skeleton behavior is a log heartbeat every 1000 ms.

## Next Useful Commands

```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

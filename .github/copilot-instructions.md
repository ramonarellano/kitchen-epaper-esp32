<!-- Use this file to provide workspace-specific custom instructions to Copilot. For more details, visit https://code.visualstudio.com/docs/copilot/copilot-customization#_use-a-githubcopilotinstructionsmd-file -->

This is a PlatformIO project for a Lolin NodeMCU v3 (ESP32). Use Arduino framework conventions and PlatformIO best practices.

Always run all commands yourself so you can see the results; I will only confirm. If a command changes the current folder, always return to the root folder afterward.

If the user says "build and upload", always upload the `/data/.env` file (SPIFFS filesystem) first, then upload the firmware code to the ESP32. If possible, combine both uploads into a single command or sequence, but always ensure the `.env` file is present on the device before the firmware runs.

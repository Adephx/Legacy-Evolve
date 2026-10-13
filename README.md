# Legacy Evolve

This solution replaces dependencies on discontinued online services with a local backend implementation, allowing the original game to run using a Goldberg Steam emulator. The solution also streamlines the gameplay experience by skipping the intro movies and hidding the ingame buttons that no longer do anything due to the discontinued online services.

## Installation

1. Download Evolve Legacy, through a 3rd party magnet link: `magnet:?xt=urn:btih:A2A78151A071635F36A6E749C49AD475BF013ECFe`
3. Download the Goldberg Steam Emulator and place steam_api64.dll from the experimental x64 build alongside Evolve.exe. Make sure that networking and overlay are not disabled: `https://gitlab.com/Mr_Goldberg/goldberg_emulator`
5. Place `Legacy.dll` alongside `Evolve.exe`
6. Run `Patch.bat`, enter the full path to the directory containing `Evolve.exe` when prompted and select `1 - Patch`
7. Launch the game normally using `Evolve.exe`

The new certificate is installed automatically when the game starts. The game directory must therefore be writable. The patch modifies four executable import entries to load `Legacy.dll`. Only the supported executable is accepted, and the patch validates its contents before writing. No additional launcher or background service is required.

## Acknowledgments

This project was made possible by [EvolveLegacyReborn](https://github.com/PaienNate/EvolveLegacyReborn) by PaienNate and its contributors. It builds on their work restoring Evolve Legacy and reuses their backend response resources.
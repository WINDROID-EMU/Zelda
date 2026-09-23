# Zelda - Zelda3 Android Port

![Screenshot 1](screenshots/screenshot1.png)
![Screenshot 2](screenshots/screenshot2.png)
![Screenshot 3](screenshots/screenshot3.png)

A high-quality port of **The Legend of Zelda: A Link to the Past** (Zelda3) to Android devices. This project brings the classic SNES masterpiece to mobile devices with modern enhancements and optimizations.

## Features

- **Full Game Experience**: Complete port of Zelda3 with all original content
- **Controller Support**: Designed for controller gameplay (touch controls coming soon)
- **Customizable Settings**: Extensive configuration options via zelda3.ini
- **Aspect Ratio Support**: Multiple aspect ratios including 18:9 widescreen
- **Sprite Customization**: Support for custom character sprites
- **Turbo Mode**: Built-in turbo button functionality
- **Multiple Languages**: Support for English, Portuguese, and more

## Installation

### Prerequisites

- Android device (Android 10+ recommended)
- Controller (required for current version)
- Original SNES ROM of "The Legend of Zelda: A Link to the Past"

### Setup Instructions

1. **Download the APK**: Get the latest version from the [Releases](https://github.com/WINDROID-EMU/Zenda/releases) section
2. **Install the APK**: Install the app on your Android device
3. **Extract Assets**: Use the instructions below to extract the required assets file from your ROM
4. **Place Assets**: Copy the extracted `zelda3_assets.dat` file to:
   ```
   Android/data/com.dishii.zelda3/files/
   ```
   (This directory will be created automatically when you run the app for the first time)

### Asset Extraction

If you don't have access to a computer, you can extract the assets directly on your device using third-party tools. Follow the instructions from the original repository:

**Original Repository**: https://github.com/snesrev/zelda3

## Configuration

### Settings File

Configuration is done through the `zelda3.ini` file located at:
```
Android/data/com.dishii.zelda3/files/zelda3.ini
```

Use a text editor to modify the settings. The file will be created with default values after the first run.

### Default Settings

- **Turbo Button**: L3 button
- **Aspect Ratio**: 18:9 (widescreen)
- **Display Mode**: Fullscreen (no Android on-screen controls)

### Available Options

- Controller mapping
- Aspect ratio selection
- Graphics enhancements
- Audio settings
- Save state management

## Screenshots

Here are some screenshots of the app in action:

![Gameplay](screenshots/screenshot1.png)
*Main gameplay with widescreen support*

![Configuration](screenshots/screenshot2.png)
*Settings and configuration options*

![Sprite Selection](screenshots/screenshot3.png)
*Custom sprite selection interface*

## Compatibility

### Android Versions

- **Android 10-12**: Standard version available
- **Android 13+**: Check the [Releases](https://github.com/WINDROID-EMU/Zenda/releases) tab for the Android 13-specific version

### Controller Support

- **Required**: Controller is currently required for gameplay
- **Recommended**: Bluetooth controllers with standard gamepad layout
- **Future Updates**: Touch controls are planned for future releases

## Controls

### Default Controller Layout

- **D-Pad**: Movement
- **A/B**: Primary actions
- **X/Y**: Secondary actions
- **L/R**: Shoulder buttons
- **Start/Select**: Menu and pause
- **L3**: Turbo button (configurable)

## Customization

### Sprite Packs

The app supports custom character sprites. You can download and install sprite packs to change Link's appearance. Use the built-in sprite downloader or manually place sprite files in the appropriate directory.

### Widescreen Support

This port includes enhanced widescreen support for modern devices. The game can be configured to use various aspect ratios including:
- 4:3 (original)
- 16:9
- 18:9
- 21:9 (ultrawide)

## Building from Source

### Requirements

- Android Studio
- NDK (Native Development Kit)
- Gradle

### Build Instructions

1. Clone the repository:
   ```bash
   git clone https://github.com/WINDROID-EMU/Zenda.git
   cd Zenda
   ```

2. Open the project in Android Studio

3. Build the project:
   ```bash
   ./gradlew assembleDebug
   ```

4. The APK will be generated in `app/build/outputs/apk/`

## Contributing

Contributions are welcome! Please feel free to submit issues, feature requests, or pull requests.

## Credits

- **Original Project**: https://github.com/snesrev/zelda3
- **Original Game**: Nintendo - The Legend of Zelda: A Link to the Past
- **SDL2**: For cross-platform graphics and input handling

## License

This project maintains the same license as the original zelda3 project. Please refer to the original repository for specific licensing information.

## Disclaimer

This project requires you to own the original game. ROM files are not included. Please support the official release.

## Support

For issues, questions, or suggestions:
- Open an issue on GitHub
- Check the [Wiki](https://github.com/WINDROID-EMU/Zenda/wiki) for documentation
- Join our community discussions

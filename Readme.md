# Universal Gyro Aim

Universal Gyro Aim is a simple utility for Windows that captures gyro data from a physical gamepad and maps it to the right analog stick of a virtual Xbox 360 controller. This allows you to add motion-based aiming to games that support standard XInput controllers.

The application uses the SDL3 library to read input from a wide range of physical controllers and the ViGEmBus driver to create the virtual Xbox 360 controller that games will recognize.

## Disclaimer

Please note that this is a proof-of-concept project. The code is crude, unoptimized, and serves as a basic implementation of the idea. It may contain bugs or performance issues and is not intended for robust, everyday use without further development.

## Requirements

To use this application, you **must** have the ViGEmBus driver installed on your system. The virtual controller cannot be created without it.

-   Download and install the ViGEmBus driver version 1.22.0 from the [official releases page](https://github.com/ViGEm/ViGEmBus/releases).

## How to Use

1.  Ensure you have met the requirements listed above.
2.  Download the latest [release](https://github.com/ab2x3z/UniversalGyroAim/releases).
3.  Connect your physical controller to your computer.
4.  Run `UniversalGyroAim.exe`. A small window will open.
5.  The application will wait for a physical controller to be connected. Once detected, its status will be displayed in the window.
6.  By default, no aim button is set. Follow the on-screen instructions to configure the application.

## License

The code for this project (`UGA.c`) is provided as-is. The included ViGEmClient library is distributed under the MIT License.
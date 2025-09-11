# Universal Gyro Aim

Universal Gyro Aim is a utility for Windows that captures gyro data from a physical gamepad and maps it to either the right analog stick of a virtual Xbox 360 controller or directly to mouse movement. This allows you to add high-precision motion-based aiming to any game that supports standard XInput controllers or mouse input.

The application uses the SDL3 library to read input from a wide range of physical controllers and the ViGEmBus driver to create the virtual Xbox 360 controller that games will recognize. To prevent double input issues, it also uses HidHide to conceal the physical controller from games.

## Disclaimer

Please note that this is a proof-of-concept project. The code is crude, unoptimized, and serves as a basic implementation of the idea. It may contain bugs or performance issues and is not intended for robust, everyday use without further development.

## Requirements

To use this application, you **must** have the following drivers installed and configured on your system:

-   **ViGEmBus:** This driver is required to create the virtual controller.
    -   Download and install ViGEmBus driver version 1.22.0 from the [official releases page](https://github.com/ViGEm/ViGEmBus/releases).
-   **HidHide:** This driver is required to hide your physical controller from games, preventing double input.
    -   Download and install the latest version of HidHide from the [official releases page](https://github.com/nefarius/HidHide/releases).
    -   **IMPORTANT:** After installing, you must whitelist `UniversalGyroAim.exe` so it can access the hidden controller. See instructions below.

## How to Use

1.  **Install the required drivers** from the links in the "Requirements" section.
2.  **Configure HidHide:**
    -   Open the **HidHide Configuration Client** from your Start Menu.
    -   Go to the **"Applications"** tab.
    -   Click the `+` icon to add an application.
    -   Browse to and select `UniversalGyroAim.exe`.
    -   Close the **HidHide Configuration Client**
3.  **Download and Run:**
    -   Download the latest [release](https://github.com/ab2x3z/UniversalGyroAim/releases).
    -   Connect your physical controller to your computer.
    -   Run `UniversalGyroAim.exe`. A small window will open.
4.  **Configure:**
    -   The application will detect your controller and attempt to hide it.
    -   Follow the on-screen instructions and keyboard shortcuts displayed in the application window to configure your settings (e.g., set an aim button, adjust sensitivity).

## License

The code for this project (`UGA.c`) is provided as-is. The included ViGEmClient library is distributed under the MIT License.
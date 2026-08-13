# BeepTEA

![BeepTEA Logo](firmware/data/logo.jpg)

**BeepTEA** is a smartwatch-style smart communicator (based on the LilyGo T-Display-S3 board) specifically designed to help children and individuals with Autism Spectrum Disorder (ASD) or other cognitive needs.

Its main function is to provide visual routines using pictograms and a vibration alert system without relying on a constant internet connection, thereby improving autonomy and reducing anxiety.

---

## 🎯 Main Features
* 📵 **True Independence:** Works 100% offline after initial setup. Keeps time internally via RTC.
* 🖼️ **Visual Routines:** Displays custom pictograms to guide the user through their daily tasks (eating, washing hands, sleeping...).
* 📳 **Friendly Alerts:** The watch vibrates and emits gentle tones when it's time for a new routine, catching attention without causing distress.
* 📶 **On-Demand Connectivity:** The watch hosts its own captive portal (Direct Wi-Fi) for initial setup, and connects via **Bluetooth (BLE)** to sync routines with the mobile App.

## 📦 Project Structure

This repository contains everything needed to build and modify the entire system:

1. `/firmware`: The C++ source code for the ESP32-S3 microcontroller. Ready to be compiled with **PlatformIO**.
2. `/mobile-app`: The source code for the companion mobile application (**BeepTEA Manager**) built with **Capacitor** and vanilla HTML/JS. Allows easy pictogram uploads and routine scheduling via Bluetooth.
3. `/BOM.txt`: Bill of Materials and components used in the project.

## 🛠️ Hardware Requirements

The brain of the project is a **LilyGo T-Display-S3**, a powerful ESP32-S3 with a built-in color IPS display. We added the following to this board:

* **Vibration Motor (Haptic):** Connected to PIN 43.
* **Buzzer:** Connected to PIN 44.
* **Extra Physical Button:** (In addition to the internal buttons on PIN 0 and 14).
* **LiPo Battery:** With smart voltage monitoring (connected to PIN 4).

## 🚀 Installation Guide (Firmware)

1. Install [Visual Studio Code](https://code.visualstudio.com/) and the [PlatformIO](https://platformio.org/) extension.
2. Open the `/firmware` folder in VS Code.
3. Connect your LilyGo T-Display-S3 board via USB.
4. First, compile and upload the filesystem (LittleFS) using the **`Upload File System Image`** option in PlatformIO (this uploads the captive portal and the logo).
5. Second, compile and upload the source code using the **`Upload`** option.

## 📱 Build Guide (Mobile App)

To compile the mobile app, you will need [Node.js](https://nodejs.org/) and Android Studio.

1. Open the `/mobile-app` folder in your terminal.
2. Install dependencies: `npm install`
3. Sync the web code with the native Android project: `npx cap sync android`
4. Open it in Android Studio to build your APK: `npx cap open android`

## 🤝 Credits and Collaboration

A project created with dedication to make life easier and foster autonomy for people with ASD. Feel free to fork, improve, or propose new features by opening a *Pull Request*!

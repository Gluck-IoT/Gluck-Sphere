# Gluck Sphere
Gluck runs on two operating systems: Azure Sphere, which is able to communicate with Azure Sphere, and Azure RTOS, a real time-capable operating system.

This repository contains the code for the Azure Sphere app.

## What is Azure Sphere?
[Azure Sphere](https://azure.microsoft.com/en-gb/services/azure-sphere/ "Azure Sphere") is a comprehensive security package for IoT devices. It consists of a microcontroller running a specialised Linux-based operating system, and a cloud service which handles certificate authentication, updates and device management.

An Azure Sphere microcontroller can run apps in real time or on the Azure Sphere operating system: only those on Azure Sphere are allowed to connect to the internet. The Azure Sphere OS connects to the Azure Sphere Security Service, from which it can receive updates and connect to the cloud. The security service also controls the device's capabilities, protecting it from running rogue software, man-in-the-middle attacks, or communicating with a spoofed host.

More information about Azure Sphere can be found [here](https://docs.microsoft.com/en-us/azure-sphere/product-overview/what-is-azure-sphere "What is Azure Sphere?").

## Functionality
This app performs the following tasks:

- Connect to Azure IoT Hub and handle certificates and authentication
- Poll the ADC (which would be connected to a glucose lancet, allowing it to read blood glucose levels) and transmit readings to IoT Hub
- Receive commands from IoT Hub; namely, when glucose levels become too low, turn on a water pump (which in real life would be an insulin pump) to restore glucose levels to normal

## Capabilities
This app uses the following capabilities:

- **ADC:** Required to read glucose levels
- **Connections:** Required to be able to connect to IoT Hub
- **GPIO:** Used to power certain LEDs/buttons for testing purposes
- **UART:** Used to power the water pump
- **System event notifications:** Used for debugging
- **Mutable storage:** 8KB provided
- **Wi-Fi config:** Used to allow the Azure Sphere board to connect via Wi-Fi

## Contributing
After 17 March 2021, all pull requests are welcome.

# Gluck Sphere
Gluck is a prototype for an automated, internet-connected device which can take glucose levels automatically and inject insulin into the patient when their glucose levels become low. An accompanying repository contains a web application which doctors can use to view their patients' glucose readings and message their patients.

![Gluck security model diagram](images/gluck-photo.png?raw=true "Gluck diagram")

The Gluck device comes equipped with an Azure Sphere microcontroller, a water pump (a stand-in for an insulin pump), and a simple ADC blood glucose measurement tool. Once every 5 minutes, it takes a glucose reading and sends it to IoT Hub. In addition, the IoT Hub cloud service keeps track of all devices, and, when a device is reading abnormally low levels of glucose, the cloud service will send a message to the Gluck device, telling it how much insulin to inject into the patient. Communications between the device and the cloud are secured by Azure Sphere, which provides enhanced message authentication using X.509 certificates. The aim is that this makes Gluck more secure than other IoT-connected insulin delivery devices.

## What is Azure Sphere?
[Azure Sphere](https://azure.microsoft.com/en-gb/services/azure-sphere/ "Azure Sphere") is a comprehensive security package for IoT devices. It consists of a microcontroller running a specialised Linux-based operating system, and a cloud service which handles certificate authentication, updates and device management.

![Gluck security model diagram](images/gluck-diagram.png?raw=true "Gluck diagram")

An Azure Sphere microcontroller can run apps in real time or on the Azure Sphere operating system: only those running on Azure Sphere are allowed to connect to the internet. The Azure Sphere OS connects to the Azure Sphere Security Service, from which it can receive updates and connect to the cloud. The security service also controls the device's capabilities, protecting it from running rogue software, man-in-the-middle attacks, or communicating with a spoofed host.

More information about Azure Sphere can be found [here](https://docs.microsoft.com/en-us/azure-sphere/product-overview/what-is-azure-sphere "What is Azure Sphere?").

## Functionality
This app performs the following tasks:

- Connect to Azure IoT Hub and handle certificates and authentication
- Poll the ADC (which would be connected to a glucose lancet, allowing it to read blood glucose levels) and transmit readings to IoT Hub
- Receive commands from IoT Hub; namely, when glucose levels become too low, turn on a water pump (which in real life would be an insulin pump) to restore glucose levels to normal

## Use
To use the program, the following must be set up (the setup instructions are identical to the [Azure IoT sample](https://github.com/Azure/azure-sphere-samples/blob/master/Samples/AzureIoT/README.md "Azure IoT Sample")):

1. Azure Sphere must be configured as described and connected to the internet
2. An Azure IoT Hub/IoT Central application must be set up
3. The device capabilities must be configured in app_manifest.json using [the AzureIoT sample's configuration program](https://github.com/Azure/azure-sphere-samples/blob/master/Samples/AzureIoT/Tools/win-x64/ShowIoTCentralConfig.exe "ShowIoTCentralConfig.exe")
4. [If necessary, hardware dependencies must be configured](https://github.com/Azure/azure-sphere-samples/blob/master/HardwareDefinitions/README.md "Manage Hardware Dependencies"). By default, this program targets the Avnet MT3620 Starter Kit, unlike the Microsoft samples

The Gluck device has two modes: simulated and non-simulated. The simulated version allows the debugger to be used instead of the hardware if a water pump or the ADC are unavailable, and is enabled by default. It can be disabled by setting the SIMULATED variable to 0 in main.c.

By default, this program connects to the internet via Wi-Fi. Follow the instructions at the Azure IoT sample repository to connect the device via Ethernet instead.

## Capabilities
This app uses the following capabilities:

- **ADC:** Required to read glucose levels
- **Connections:** Required to be able to connect to IoT Hub
- **GPIO:** Used to power certain LEDs/buttons for testing purposes
- **UART:** Used to power the water pump
- **System event notifications:** Used for debugging
- **Mutable storage:** 8KB provided
- **Wi-Fi config:** Used to allow the Azure Sphere board to connect via Wi-Fi

## Licensing
See LICENSE.md for details. This project is based on the Azure IoT sample. For the avoidance of doubt, the following functions are re-used from the sample: *SendEventCallback; DeviceTwinCallback; TwinReportState; ReportedStateCallback; DeviceMethodCallback; GetReasonString; GetAzureSphereProvisioningResultString; SetUpAzureIoTHubClient; ButtonPollTimerEventHandler; IsButtonPressed; ValidateUserConfiguration; ParseCommandLineArguments; SetUpAzureIoTHubClientWithDaa; SetUpAzureIoTHubClientWithDps; IsConnectionReadyToSendTelemetry; ReadIoTEdgeCaCertContent*

## Contributing
After 17 March 2021, all pull requests are welcome.

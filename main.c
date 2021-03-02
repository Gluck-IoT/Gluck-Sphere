/* Copyright (c) Group Romeo 2021. All rights reserved.
   Licensed under the MIT License.
   
   Some functions in this file have been written by Microsoft.
   They are Copyright (c) Microsoft Corporation, and are also
   licensed under the MIT License. This program is based on
   the Azure IoT sample: see README.md for details. */

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applibs_versions.h"
#include <applibs/eventloop.h>
#include <applibs/adc.h>
#include <applibs/gpio.h>
#include <applibs/log.h>
#include <applibs/networking.h>
#include <applibs/storage.h>

// Select the hardware to target for this program. To change the hardware, you'll need to update
// CMakeLists.txt (see https://aka.ms/AzureSphereHardwareDefinitions for more details).
#include <hw/sample_appliance.h>

// Include header utilities
#include "eventloop_timer_utilities.h"
#include "parson.h" // Used to parse Device Twin messages.

// Include the Azure IoT SDK
#include <iothub_client_core_common.h>
#include <iothub_device_client_ll.h>
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>
#include <iothub.h>
#include <azure_sphere_provisioning.h>
#include <iothub_security_factory.h>
#include <shared_util_options.h>

// Exit codes for this application. These are used for the
// application exit code. They must all be between zero and 255,
// where zero is reserved for successful termination.
typedef enum {
    ExitCode_Success = 0,

    ExitCode_TermHandler_SigTerm = 1,

    ExitCode_Main_EventLoopFail = 2,

    ExitCode_ButtonTimer_Consume = 3,

    ExitCode_AzureTimer_Consume = 4,

    ExitCode_Init_EventLoop = 5,
    ExitCode_Init_MessageButton = 6,
    ExitCode_Init_OrientationButton = 7,
    ExitCode_Init_TwinStatusLed = 8,
    ExitCode_Init_ButtonPollTimer = 9,
    ExitCode_Init_AzureTimer = 10,

    ExitCode_IsButtonPressed_GetValue = 11,

    ExitCode_Validate_ConnectionType = 12,
    ExitCode_Validate_ScopeId = 13,
    ExitCode_Validate_Hostname = 14,
    ExitCode_Validate_IoTEdgeCAPath = 15,

    ExitCode_InterfaceConnectionStatus_Failed = 16,

    ExitCode_IoTEdgeRootCa_Open_Failed = 17,
    ExitCode_IoTEdgeRootCa_LSeek_Failed = 18,
    ExitCode_IoTEdgeRootCa_FileSize_Invalid = 19,
    ExitCode_IoTEdgeRootCa_FileSize_TooLarge = 20,
    ExitCode_IoTEdgeRootCa_FileRead_Failed = 21,

    ExitCode_PayloadSize_TooLarge = 22,

    ExitCode_AdcTimerHandler_Consume = 23,
    ExitCode_AdcTimerHandler_Poll = 24,

    ExitCode_Init_AdcOpen = 25,
    ExitCode_Init_GetBitCount = 26,
    ExitCode_Init_UnexpectedBitCount = 27,
    ExitCode_Init_SetRefVoltage = 28
} ExitCode;

static volatile sig_atomic_t exitCode = ExitCode_Success;

// Connection types to use when connecting to the Azure IoT Hub.
typedef enum {
    ConnectionType_NotDefined = 0,
    ConnectionType_DPS = 1,
    ConnectionType_Direct = 2,
    ConnectionType_IoTEdge = 3
} ConnectionType;

// Authentication state of the client with respect to the Azure IoT Hub.
typedef enum {
    IoTHubClientAuthenticationState_NotAuthenticated = 0,           // Not authenticated
    IoTHubClientAuthenticationState_AuthenticationInitiated = 1,    // Started authentication
    IoTHubClientAuthenticationState_Authenticated = 2               // Authenticated
} IoTHubClientAuthenticationState;

// Constants
#define MAX_DEVICE_TWIN_PAYLOAD_SIZE 512
#define TELEMETRY_BUFFER_SIZE 100
#define MAX_ROOT_CA_CERT_CONTENT_SIZE (3 * 1024)

// Azure IoT definitions
static char* scopeId = NULL;  // ScopeId for DPS.
static char* hostName = NULL; // Azure IoT Hub or IoT Edge Hostname.
static ConnectionType connectionType = ConnectionType_NotDefined; // Type of connection to use.
static char* iotEdgeRootCAPath = NULL; // Path (including filename) of the IotEdge cert.
static char iotEdgeRootCACertContent[MAX_ROOT_CA_CERT_CONTENT_SIZE +
1]; // Add 1 to account for null terminator.
static IoTHubClientAuthenticationState iotHubClientAuthenticationState =
IoTHubClientAuthenticationState_NotAuthenticated; // Authentication state with respect to the
                                                  // IoT Hub.

static IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle = NULL;
static const int deviceIdForDaaCertUsage = 1;     // A constant used to direct the IoT SDK to use
                                                  // the DAA cert under the hood.
static const char networkInterface[] = "wlan0";

// Function declarations
static void SendEventCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context);
static void DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload,
    size_t payloadSize, void* userContextCallback);
static void TwinReportState(const char* jsonState);
static void ReportedStateCallback(int result, void* context);
static int DeviceMethodCallback(const char* methodName, const unsigned char* payload,
    size_t payloadSize, unsigned char** response, size_t* responseSize,
    void* userContextCallback);
static const char* GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason);
static const char* GetAzureSphereProvisioningResultString(
    AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult);
static void SendTelemetry(const char* jsonMessage);
static void SetUpAzureIoTHubClient(void);
static void SendSimulatedTelemetry(void);
static void ButtonPollTimerEventHandler(EventLoopTimer* timer);
static bool IsButtonPressed(int fd, GPIO_Value_Type* oldState);
static void AzureTimerEventHandler(EventLoopTimer* timer);
static ExitCode ValidateUserConfiguration(void);
static void ParseCommandLineArguments(int argc, char* argv[]);
static bool SetUpAzureIoTHubClientWithDaa(void);
static bool SetUpAzureIoTHubClientWithDps(void);
static bool IsConnectionReadyToSendTelemetry(void);
static ExitCode ReadIoTEdgeCaCertContent(void);

// Initialization/Cleanup
static ExitCode InitPeripheralsAndHandlers(void);
static void CloseFdAndPrintError(int fd, const char* fdName);
static void ClosePeripheralsAndHandlers(void);

// File descriptors - initialized to invalid value
// Button
static int sendMessageButtonGpioFd = -1;

// LED
static int deviceTwinStatusLedGpioFd = -1;

// Timer / polling
static EventLoop* eventLoop = NULL;
static EventLoopTimer* buttonPollTimer = NULL;
static EventLoopTimer* azureTimer = NULL;

// Azure IoT poll periods
static const int AzureIoTDefaultPollPeriodSeconds = 1;        // poll azure iot every second
static const int AzureIoTPollPeriodsPerTelemetry = 300;       // only send telemetry every 5 minutes
static const int AzureIoTMinReconnectPeriodSeconds = 60;      // back off when reconnecting
static const int AzureIoTMaxReconnectPeriodSeconds = 10 * 60; // back off limit

static int azureIoTPollPeriodSeconds = -1;
static int telemetryCount = 0;

// State variables
static GPIO_Value_Type sendMessageButtonState = GPIO_Value_High;
static bool statusLedOn = false;

static float voltage = 5.0; // Default value for simulation purposes

// The size of a sample in bits
static int sampleBitCount = -1;

// The maximum voltage
static float sampleMaxVoltage = 2.5f;

// Include different functions depending on whether the hardware is simulated
#define SIMULATED 1
#if SIMULATED == 1
#include "hardwarefunctions_simulated.h"
#else
#include "hardwarefunctions.h"
#endif

// Usage text for command line arguments in application manifest.
static const char* cmdLineArgsUsageText =
"DPS connection type: \" CmdArgs \": [\"--ConnectionType\", \"DPS\", \"--ScopeID\", "
"\"<scope_id>\"]\n"
"Direction connection type: \" CmdArgs \": [\"--ConnectionType\", \"Direct\", "
"\"--Hostname\", \"<azureiothub_hostname>\"]\n "
"IoTEdge connection type: \" CmdArgs \": [\"--ConnectionType\", \"IoTEdge\", "
"\"--Hostname\", \"<iotedgedevice_hostname>\", \"--IoTEdgeRootCAPath\", "
"\"certs/<iotedgedevice_cert_name>\"]\n";

// Signal handler for termination requests. This handler must be async-signal-safe.
static void TerminationHandler(int signalNumber) {
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    exitCode = ExitCode_TermHandler_SigTerm;
}

// Button timer event: Check the status of the button.
static void ButtonPollTimerEventHandler(EventLoopTimer* timer) {
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        exitCode = ExitCode_ButtonTimer_Consume;
        return;
    }

    if (IsButtonPressed(sendMessageButtonGpioFd, &sendMessageButtonState)) {
        SendTelemetry("{\"ButtonPress\" : true}");
    }
}

// Azure timer event:  Check connection status and send telemetry
static void AzureTimerEventHandler(EventLoopTimer* timer) {
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        exitCode = ExitCode_AzureTimer_Consume;
        return;
    }

    // Check whether the device is connected to the internet.
    Networking_InterfaceConnectionStatus status;
    if (Networking_GetInterfaceConnectionStatus(networkInterface, &status) == 0) {
        if ((status & Networking_InterfaceConnectionStatus_ConnectedToInternet) &&
            (iotHubClientAuthenticationState == IoTHubClientAuthenticationState_NotAuthenticated)) {
            SetUpAzureIoTHubClient();
        }
    }
    else {
        if (errno != EAGAIN) {
            Log_Debug("ERROR: Networking_GetInterfaceConnectionStatus: %d (%s)\n", errno,
                strerror(errno));
            exitCode = ExitCode_InterfaceConnectionStatus_Failed;
            return;
        }
    }

    if (iotHubClientAuthenticationState == IoTHubClientAuthenticationState_Authenticated) {
        telemetryCount++;
        if (telemetryCount == AzureIoTPollPeriodsPerTelemetry) {
            telemetryCount = 0;
            SendSimulatedTelemetry();
        }
    }

    if (iothubClientHandle != NULL) {
        IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
    }
}

// Parse the command line arguments given in the application manifest.
static void ParseCommandLineArguments(int argc, char* argv[]) {
    int option = 0;
    static const struct option cmdLineOptions[] = {
        {.name = "ConnectionType", .has_arg = required_argument, .flag = NULL, .val = 'c'},
        {.name = "ScopeID", .has_arg = required_argument, .flag = NULL, .val = 's'},
        {.name = "Hostname", .has_arg = required_argument, .flag = NULL, .val = 'h'},
        {.name = "IoTEdgeRootCAPath", .has_arg = required_argument, .flag = NULL, .val = 'i'},
        {.name = NULL, .has_arg = 0, .flag = NULL, .val = 0} };

    // Loop over all of the options.
    while ((option = getopt_long(argc, argv, "c:s:h:i:", cmdLineOptions, NULL)) != -1) {
        // Check if arguments are missing. Every option requires an argument.
        if (optarg != NULL && optarg[0] == '-') {
            Log_Debug("WARNING: Option %c requires an argument\n", option);
            continue;
        }
        switch (option) {
        case 'c':
            Log_Debug("ConnectionType: %s\n", optarg);
            if (strcmp(optarg, "DPS") == 0) {
                connectionType = ConnectionType_DPS;
            }
            else if (strcmp(optarg, "Direct") == 0) {
                connectionType = ConnectionType_Direct;
            }
            else if (strcmp(optarg, "IoTEdge") == 0) {
                connectionType = ConnectionType_IoTEdge;
            }
            break;
        case 's':
            Log_Debug("ScopeID: %s\n", optarg);
            scopeId = optarg;
            break;
        case 'h':
            Log_Debug("Hostname: %s\n", optarg);
            hostName = optarg;
            break;
        case 'i':
            Log_Debug("IoTEdgeRootCAPath: %s\n", optarg);
            iotEdgeRootCAPath = optarg;
            break;
        default:
            // Unknown options are ignored.
            break;
        }
    }
}

// Validate if the values of the Connection type, Scope ID, IoT Hub or IoT Edge Hostname
// were set. Returns ExitCode_Success if the parameters were provided, or another ExitCode
// value which indicates the specific failure.
static ExitCode ValidateUserConfiguration(void) {
    ExitCode validationExitCode = ExitCode_Success;

    if (connectionType < ConnectionType_DPS || connectionType > ConnectionType_IoTEdge) {
        validationExitCode = ExitCode_Validate_ConnectionType;
    }

    if (connectionType == ConnectionType_DPS) {
        if (scopeId == NULL) {
            validationExitCode = ExitCode_Validate_ScopeId;
        }
        else {
            Log_Debug("Using DPS Connection: Azure IoT DPS Scope ID %s\n", scopeId);
        }
    }

    if (connectionType == ConnectionType_Direct) {
        if (hostName == NULL) {
            validationExitCode = ExitCode_Validate_Hostname;
        }

        if (validationExitCode == ExitCode_Success) {
            Log_Debug("Using Direct Connection: Azure IoT Hub Hostname %s\n", hostName);
        }
    }

    if (connectionType == ConnectionType_IoTEdge) {
        if (hostName == NULL) {
            validationExitCode = ExitCode_Validate_Hostname;
        }

        if (iotEdgeRootCAPath == NULL) {
            validationExitCode = ExitCode_Validate_IoTEdgeCAPath;
        }

        if (validationExitCode == ExitCode_Success) {
            Log_Debug("Using IoTEdge Connection: IoT Edge device Hostname %s, IoTEdge CA path %s\n",
                hostName, iotEdgeRootCAPath);
        }
    }

    if (validationExitCode != ExitCode_Success) {
        Log_Debug("Command line arguments for application shoud be set as below\n%s",
            cmdLineArgsUsageText);
    }

    return validationExitCode;
}


// Close a file descriptor and print an error on failure.
static void CloseFdAndPrintError(int fd, const char* fdName) {
    if (fd >= 0) {
        int result = close(fd);
        if (result != 0) {
            Log_Debug("ERROR: Could not close fd %s: %s (%d).\n", fdName, strerror(errno), errno);
        }
    }
}

// Callback when the Azure IoT connection state changes.
// This can indicate that a new connection attempt has succeeded or failed.
// It can also indicate than an existing connection has expired due to SAS token expiry.
static void ConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result,
    IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason,
    void* userContextCallback)
{
    Log_Debug("Azure IoT connection status: %s\n", GetReasonString(reason));

    if (result != IOTHUB_CLIENT_CONNECTION_AUTHENTICATED) {
        iotHubClientAuthenticationState = IoTHubClientAuthenticationState_NotAuthenticated;
        return;
    }

    iotHubClientAuthenticationState = IoTHubClientAuthenticationState_Authenticated;

    // Send static device twin properties when connection is established.
    TwinReportState("{\"manufacturer\":\"Microsoft\",\"model\":\"Azure Sphere Sample Device\"}");
}

// Set up the Azure IoT Hub connection (creates the iothubClientHandle)
// When the SAS Token for a device expires the connection needs to be recreated
// which is why this is not simply a one time call.
static void SetUpAzureIoTHubClient(void) {
    bool isClientSetupSuccessful = false;

    if (iothubClientHandle != NULL) {
        IoTHubDeviceClient_LL_Destroy(iothubClientHandle);
    }

    if ((connectionType == ConnectionType_Direct) || (connectionType == ConnectionType_IoTEdge)) {
        isClientSetupSuccessful = SetUpAzureIoTHubClientWithDaa();
    }
    else if (connectionType == ConnectionType_DPS) {
        isClientSetupSuccessful = SetUpAzureIoTHubClientWithDps();
    }

    if (!isClientSetupSuccessful) {
        // If we fail to connect, reduce the polling frequency, starting at
        // AzureIoTMinReconnectPeriodSeconds and with a backoff up to
        // AzureIoTMaxReconnectPeriodSeconds
        if (azureIoTPollPeriodSeconds == AzureIoTDefaultPollPeriodSeconds) {
            azureIoTPollPeriodSeconds = AzureIoTMinReconnectPeriodSeconds;
        }
        else {
            azureIoTPollPeriodSeconds *= 2;
            if (azureIoTPollPeriodSeconds > AzureIoTMaxReconnectPeriodSeconds) {
                azureIoTPollPeriodSeconds = AzureIoTMaxReconnectPeriodSeconds;
            }
        }

        struct timespec azureTelemetryPeriod = { azureIoTPollPeriodSeconds, 0 };
        SetEventLoopTimerPeriod(azureTimer, &azureTelemetryPeriod);

        Log_Debug("ERROR: Failed to create IoTHub Handle - will retry in %i seconds.\n",
            azureIoTPollPeriodSeconds);
        return;
    }

    // Successfully connected, so make sure the polling frequency is back to the default
    azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
    struct timespec azureTelemetryPeriod = { .tv_sec = azureIoTPollPeriodSeconds, .tv_nsec = 0 };
    SetEventLoopTimerPeriod(azureTimer, &azureTelemetryPeriod);

    // Set client authentication state to initiated. This is done to indicate that
    // SetUpAzureIoTHubClient() has been called (and so should not be called again) while the
    // client is waiting for a response via the ConnectionStatusCallback().
    iotHubClientAuthenticationState = IoTHubClientAuthenticationState_AuthenticationInitiated;

    IoTHubDeviceClient_LL_SetDeviceTwinCallback(iothubClientHandle, DeviceTwinCallback, NULL);
    IoTHubDeviceClient_LL_SetDeviceMethodCallback(iothubClientHandle, DeviceMethodCallback, NULL);
    IoTHubDeviceClient_LL_SetConnectionStatusCallback(iothubClientHandle, ConnectionStatusCallback,
        NULL);
}

// Set up the Azure IoT Hub connection (creating the iothubClientHandle) with DAA.
static bool SetUpAzureIoTHubClientWithDaa(void) {
    bool retVal = true;

    // Set up auth type
    int retError = iothub_security_init(IOTHUB_SECURITY_TYPE_X509);
    if (retError != 0) {
        Log_Debug("ERROR: iothub_security_init failed with error %d.\n", retError);
        return false;
    }

    // Create Azure Iot Hub client handle
    iothubClientHandle =
        IoTHubDeviceClient_LL_CreateWithAzureSphereFromDeviceAuth(hostName, MQTT_Protocol);

    if (iothubClientHandle == NULL) {
        Log_Debug("IoTHubDeviceClient_LL_CreateFromDeviceAuth returned NULL.\n");
        retVal = false;
        goto cleanup;
    }

    // Enable DAA cert usage when X509 is invoked
    if (IoTHubDeviceClient_LL_SetOption(iothubClientHandle, "SetDeviceId",
        &deviceIdForDaaCertUsage) != IOTHUB_CLIENT_OK) {
        Log_Debug("ERROR: Failure setting Azure IoT Hub client option \"SetDeviceId\".\n");
        retVal = false;
        goto cleanup;
    }

    if (connectionType == ConnectionType_IoTEdge) {
        // Provide the Azure IoT device client with the IoT Edge root
        // X509 CA certificate that was used to setup the Edge runtime.
        if (IoTHubDeviceClient_LL_SetOption(iothubClientHandle, OPTION_TRUSTED_CERT,
            iotEdgeRootCACertContent) != IOTHUB_CLIENT_OK) {
            Log_Debug("ERROR: Failure setting Azure IoT Hub client option \"TrustedCerts\".\n");
            retVal = false;
            goto cleanup;
        }

        // Set the auto URL Encoder (recommended for MQTT).
        bool urlEncodeOn = true;
        if (IoTHubDeviceClient_LL_SetOption(iothubClientHandle, OPTION_AUTO_URL_ENCODE_DECODE,
            &urlEncodeOn) != IOTHUB_CLIENT_OK) {
            Log_Debug(
                "ERROR: Failure setting Azure IoT Hub client option "
                "\"OPTION_AUTO_URL_ENCODE_DECODE\".\n");
            retVal = false;
            goto cleanup;
        }
    }

cleanup:
    iothub_security_deinit();

    return retVal;
}

// Set up the Azure IoT Hub connection (creating the iothubClientHandle) with DPS.
static bool SetUpAzureIoTHubClientWithDps(void) {
    AZURE_SPHERE_PROV_RETURN_VALUE provResult =
        IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning(scopeId, 10000,
            &iothubClientHandle);
    Log_Debug("IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning returned '%s'.\n",
        GetAzureSphereProvisioningResultString(provResult));

    if (provResult.result != AZURE_SPHERE_PROV_RESULT_OK) {
        return false;
    }

    return true;
}

// Callback invoked when a Device Twin update is received from Azure IoT Hub.
static void DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload,
    size_t payloadSize, void* userContextCallback)
{
    // Statically allocate this for more predictable memory use patterns
    static char nullTerminatedJsonString[MAX_DEVICE_TWIN_PAYLOAD_SIZE + 1];

    if (payloadSize > MAX_DEVICE_TWIN_PAYLOAD_SIZE) {
        Log_Debug("ERROR: Device twin payload size (%u bytes) exceeds maximum (%u bytes).\n",
            payloadSize, MAX_DEVICE_TWIN_PAYLOAD_SIZE);

        exitCode = ExitCode_PayloadSize_TooLarge;
        return;
    }

    // Copy the payload to local buffer for null-termination.
    memcpy(nullTerminatedJsonString, payload, payloadSize);

    // Add the null terminator at the end.
    nullTerminatedJsonString[payloadSize] = 0;

    JSON_Value* rootProperties = NULL;
    rootProperties = json_parse_string(nullTerminatedJsonString);
    if (rootProperties == NULL) {
        Log_Debug("WARNING: Cannot parse the string as JSON content.\n");
        goto cleanup;
    }

    JSON_Object* rootObject = json_value_get_object(rootProperties);
    JSON_Object* desiredProperties = json_object_dotget_object(rootObject, "desired");
    if (desiredProperties == NULL) {
        desiredProperties = rootObject;
    }

    // The desired properties should have a "StatusLED" object
    int statusLedValue = json_object_dotget_boolean(desiredProperties, "StatusLED");
    if (statusLedValue != -1) {
        statusLedOn = statusLedValue == 1;
        GPIO_SetValue(deviceTwinStatusLedGpioFd, statusLedOn ? GPIO_Value_Low : GPIO_Value_High);
    }

    // Report current status LED state
    if (statusLedOn) {
        TwinReportState("{\"StatusLED\":true}");
    }
    else {
        TwinReportState("{\"StatusLED\":false}");
    }

cleanup:
    // Release the allocated memory.
    json_value_free(rootProperties);
}

// Converts the Azure IoT Hub connection status reason to a string.
static const char* GetReasonString(IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason) {
    static char* reasonString = "unknown reason";
    switch (reason) {
    case IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN:
        reasonString = "IOTHUB_CLIENT_CONNECTION_EXPIRED_SAS_TOKEN";
        break;
    case IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED:
        reasonString = "IOTHUB_CLIENT_CONNECTION_DEVICE_DISABLED";
        break;
    case IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL:
        reasonString = "IOTHUB_CLIENT_CONNECTION_BAD_CREDENTIAL";
        break;
    case IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED:
        reasonString = "IOTHUB_CLIENT_CONNECTION_RETRY_EXPIRED";
        break;
    case IOTHUB_CLIENT_CONNECTION_NO_NETWORK:
        reasonString = "IOTHUB_CLIENT_CONNECTION_NO_NETWORK";
        break;
    case IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR:
        reasonString = "IOTHUB_CLIENT_CONNECTION_COMMUNICATION_ERROR";
        break;
    case IOTHUB_CLIENT_CONNECTION_OK:
        reasonString = "IOTHUB_CLIENT_CONNECTION_OK";
        break;
    case IOTHUB_CLIENT_CONNECTION_NO_PING_RESPONSE:
        reasonString = "IOTHUB_CLIENT_CONNECTION_NO_PING_RESPONSE";
        break;
    }
    return reasonString;
}

// Convert AZURE_SPHERE_PROV_RETURN_VALUE to a string.
static const char* GetAzureSphereProvisioningResultString(
    AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult)
{
    switch (provisioningResult.result) {
    case AZURE_SPHERE_PROV_RESULT_OK:
        return "AZURE_SPHERE_PROV_RESULT_OK";
    case AZURE_SPHERE_PROV_RESULT_INVALID_PARAM:
        return "AZURE_SPHERE_PROV_RESULT_INVALID_PARAM";
    case AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY:
        return "AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY";
    case AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY:
        return "AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY";
    case AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR:
        return "AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR";
    case AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR:
        return "AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR";
    default:
        return "UNKNOWN_RETURN_VALUE";
    }
}

// Check the network status.
static bool IsConnectionReadyToSendTelemetry(void) {
    Networking_InterfaceConnectionStatus status;
    if (Networking_GetInterfaceConnectionStatus(networkInterface, &status) != 0) {
        if (errno != EAGAIN) {
            Log_Debug("ERROR: Networking_GetInterfaceConnectionStatus: %d (%s)\n", errno,
                strerror(errno));
            exitCode = ExitCode_InterfaceConnectionStatus_Failed;
            return false;
        }
        Log_Debug(
            "WARNING: Cannot send Azure IoT Hub telemetry because the networking stack isn't ready "
            "yet.\n");
        return false;
    }

    if ((status & Networking_InterfaceConnectionStatus_ConnectedToInternet) == 0) {
        Log_Debug(
            "WARNING: Cannot send Azure IoT Hub telemetry because the device is not connected to "
            "the internet.\n");
        return false;
    }

    return true;
}

// Send telemetry to Azure IoT Hub.
static void SendTelemetry(const char* jsonMessage) {
    if (iotHubClientAuthenticationState != IoTHubClientAuthenticationState_Authenticated) {
        // AzureIoT client is not authenticated. Log a warning and return.
        Log_Debug("WARNING: Azure IoT Hub is not authenticated. Not sending telemetry.\n");
        return;
    }

    Log_Debug("Sending Azure IoT Hub telemetry: %s.\n", jsonMessage);

    // Check whether the device is connected to the internet.
    if (IsConnectionReadyToSendTelemetry() == false) {
        return;
    }

    IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(jsonMessage);

    if (messageHandle == 0) {
        Log_Debug("ERROR: unable to create a new IoTHubMessage.\n");
        return;
    }

    if (IoTHubDeviceClient_LL_SendEventAsync(iothubClientHandle, messageHandle, SendEventCallback,
        /*&callback_param*/ NULL) != IOTHUB_CLIENT_OK) {
        Log_Debug("ERROR: failure requesting IoTHubClient to send telemetry event.\n");
    }
    else {
        Log_Debug("INFO: IoTHubClient accepted the telemetry event for delivery.\n");
    }

    IoTHubMessage_Destroy(messageHandle);
}


// Callback invoked when the Azure IoT Hub send event request is processed.
static void SendEventCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context) {
    Log_Debug("INFO: Azure IoT Hub send telemetry event callback: status code %d.\n", result);
}

// Enqueue a report containing Device Twin reported properties. The report is not sent
// immediately, but it is sent on the next invocation of IoTHubDeviceClient_LL_DoWork().
static void TwinReportState(const char* jsonState) {
    if (iothubClientHandle == NULL) {
        Log_Debug("ERROR: Azure IoT Hub client not initialized.\n");
    }
    else {
        if (IoTHubDeviceClient_LL_SendReportedState(
            iothubClientHandle, (const unsigned char*)jsonState, strlen(jsonState),
            ReportedStateCallback, NULL) != IOTHUB_CLIENT_OK) {
            Log_Debug("ERROR: Azure IoT Hub client error when reporting state '%s'.\n", jsonState);
        }
        else {
            Log_Debug("INFO: Azure IoT Hub client accepted request to report state '%s'.\n",
                jsonState);
        }
    }
}

// Callback invoked when the Device Twin report state request is processed by Azure IoT Hub
// client.
static void ReportedStateCallback(int result, void* context) {
    Log_Debug("INFO: Azure IoT Hub Device Twin reported state callback: status code %d.\n", result);
}

// Check whether a given button has just been pressed.
static bool IsButtonPressed(int fd, GPIO_Value_Type* oldState) {
    bool isButtonPressed = false;
    GPIO_Value_Type newState;
    int result = GPIO_GetValue(fd, &newState);
    if (result != 0) {
        Log_Debug("ERROR: Could not read button GPIO: %s (%d).\n", strerror(errno), errno);
        exitCode = ExitCode_IsButtonPressed_GetValue;
    }
    else {
        // Button is pressed if it is low and different than last known state.
        isButtonPressed = (newState != *oldState) && (newState == GPIO_Value_Low);
        *oldState = newState;
    }

    return isButtonPressed;
}

// Read the certificate file and provide a null-terminated string containing the certificate.
// The function logs an error and returns an error code if it cannot allocate enough memory to
// hold the certificate content. Returns ExitCode_Success on success, otherwise returns another
// ExitCode indicating the specific error.
static ExitCode ReadIoTEdgeCaCertContent(void) {
    int certFd = -1;
    off_t fileSize = 0;

    certFd = Storage_OpenFileInImagePackage(iotEdgeRootCAPath);
    if (certFd == -1) {
        Log_Debug("ERROR: Storage_OpenFileInImagePackage failed with error code: %d (%s).\n", errno,
            strerror(errno));
        return ExitCode_IoTEdgeRootCa_Open_Failed;
    }

    // Get the file size.
    fileSize = lseek(certFd, 0, SEEK_END);
    if (fileSize == -1) {
        Log_Debug("ERROR: lseek SEEK_END: %d (%s)\n", errno, strerror(errno));
        close(certFd);
        return ExitCode_IoTEdgeRootCa_LSeek_Failed;
    }

    // Reset the pointer to start of the file.
    if (lseek(certFd, 0, SEEK_SET) < 0) {
        Log_Debug("ERROR: lseek SEEK_SET: %d (%s)\n", errno, strerror(errno));
        close(certFd);
        return ExitCode_IoTEdgeRootCa_LSeek_Failed;
    }

    if (fileSize == 0) {
        Log_Debug("File size invalid for %s\r\n", iotEdgeRootCAPath);
        close(certFd);
        return ExitCode_IoTEdgeRootCa_FileSize_Invalid;
    }

    if (fileSize > MAX_ROOT_CA_CERT_CONTENT_SIZE) {
        Log_Debug("File size for %s is %lld bytes. Max file size supported is %d bytes.\r\n",
            iotEdgeRootCAPath, fileSize, MAX_ROOT_CA_CERT_CONTENT_SIZE);
        close(certFd);
        return ExitCode_IoTEdgeRootCa_FileSize_TooLarge;
    }

    // Copy the file into the buffer.
    ssize_t read_size = read(certFd, &iotEdgeRootCACertContent, (size_t)fileSize);
    if (read_size != (size_t)fileSize) {
        Log_Debug("Error reading file %s\r\n", iotEdgeRootCAPath);
        close(certFd);
        return ExitCode_IoTEdgeRootCa_FileRead_Failed;
    }

    // Add the null terminator at the end.
    iotEdgeRootCACertContent[fileSize] = '\0';

    close(certFd);
    return ExitCode_Success;
}

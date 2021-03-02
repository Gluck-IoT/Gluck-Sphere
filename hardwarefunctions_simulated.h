/* Copyright (c) Group Romeo 2021. All rights reserved.
   Licensed under the MIT License. */

static void TerminationHandler(int signalNumber);

int main(int argc, char* argv[]) {
    Log_Debug("Azure IoT Application starting.\n");

    bool isNetworkingReady = false;
    if ((Networking_IsNetworkingReady(&isNetworkingReady) == -1) || !isNetworkingReady) {
        Log_Debug("WARNING: Network is not ready. Device cannot connect until network is ready.\n");
    }

    ParseCommandLineArguments(argc, argv);

    exitCode = ValidateUserConfiguration();
    if (exitCode != ExitCode_Success) {
        return exitCode;
    }

    if (connectionType == ConnectionType_IoTEdge) {
        exitCode = ReadIoTEdgeCaCertContent();
        if (exitCode != ExitCode_Success) {
            return exitCode;
        }
    }

    exitCode = InitPeripheralsAndHandlers();

    // Main loop
    while (exitCode == ExitCode_Success) {
        EventLoop_Run_Result result = EventLoop_Run(eventLoop, -1, true);
        // Continue if interrupted by signal, e.g. due to breakpoint being set.
        if (result == EventLoop_Run_Failed && errno != EINTR) {
            exitCode = ExitCode_Main_EventLoopFail;
        }
    }

    ClosePeripheralsAndHandlers();

    Log_Debug("Application exiting.\n");

    return exitCode;
}

// Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
// Return ExitCode_Success if all resources were allocated successfully; otherwise return
// another ExitCode value to indicate the specific failure.
static ExitCode InitPeripheralsAndHandlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

    eventLoop = EventLoop_Create();
    if (eventLoop == NULL) {
        Log_Debug("Could not create event loop.\n");
        return ExitCode_Init_EventLoop;
    }

    // Open SAMPLE_BUTTON_1 GPIO as input
    Log_Debug("Opening SAMPLE_BUTTON_1 as input.\n");
    sendMessageButtonGpioFd = GPIO_OpenAsInput(SAMPLE_BUTTON_1);
    if (sendMessageButtonGpioFd == -1) {
        Log_Debug("ERROR: Could not open SAMPLE_BUTTON_1: %s (%d).\n", strerror(errno), errno);
        return ExitCode_Init_MessageButton;
    }

    // SAMPLE_LED is used to show Device Twin settings state
    Log_Debug("Opening SAMPLE_LED as output.\n");
    deviceTwinStatusLedGpioFd =
        GPIO_OpenAsOutput(SAMPLE_LED, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (deviceTwinStatusLedGpioFd == -1) {
        Log_Debug("ERROR: Could not open SAMPLE_LED: %s (%d).\n", strerror(errno), errno);
        return ExitCode_Init_TwinStatusLed;
    }

    // Set up a timer to poll for button events.
    static const struct timespec buttonPressCheckPeriod = { .tv_sec = 0, .tv_nsec = 1000 * 1000 };
    buttonPollTimer = CreateEventLoopPeriodicTimer(eventLoop, &ButtonPollTimerEventHandler,
        &buttonPressCheckPeriod);
    if (buttonPollTimer == NULL) {
        return ExitCode_Init_ButtonPollTimer;
    }

    azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
    struct timespec azureTelemetryPeriod = { .tv_sec = azureIoTPollPeriodSeconds, .tv_nsec = 0 };
    azureTimer =
        CreateEventLoopPeriodicTimer(eventLoop, &AzureTimerEventHandler, &azureTelemetryPeriod);
    if (azureTimer == NULL) {
        return ExitCode_Init_AzureTimer;
    }

    return ExitCode_Success;
}

// Close peripherals and event handlers.
static void ClosePeripheralsAndHandlers(void) {
    DisposeEventLoopTimer(buttonPollTimer);
    DisposeEventLoopTimer(azureTimer);
    EventLoop_Close(eventLoop);

    Log_Debug("Closing file descriptors\n");

    // Leave the LEDs off
    if (deviceTwinStatusLedGpioFd >= 0) {
        GPIO_SetValue(deviceTwinStatusLedGpioFd, GPIO_Value_High);
    }

    CloseFdAndPrintError(sendMessageButtonGpioFd, "SendMessageButton");
    CloseFdAndPrintError(deviceTwinStatusLedGpioFd, "StatusLed");
}

// Send telemetry to Azure IoT Hub.
void SendSimulatedTelemetry(void) {
    static char telemetryBuffer[TELEMETRY_BUFFER_SIZE];

    // Generate a simulated voltage.
    float delta = ((float)(rand() % 41)) / 40.0f - 0.5f; // between -1.0 and +1.0
    voltage += delta;

    int len =
        snprintf(telemetryBuffer, TELEMETRY_BUFFER_SIZE, "{\"Glucose\":%3.2f}", voltage);
    if (len < 0 || len >= TELEMETRY_BUFFER_SIZE) {
        Log_Debug("ERROR: Cannot write telemetry to buffer.\n");
        return;
    }
    SendTelemetry(telemetryBuffer);
}

// Callback invoked when a Direct Method is received from Azure IoT Hub.
static int DeviceMethodCallback(const char* methodName, const unsigned char* payload,
    size_t payloadSize, unsigned char** response, size_t* responseSize,
    void* userContextCallback)
{
    int result;
    char* responseString;

    Log_Debug("Received Device Method callback: Method name %s.\n", methodName);

    if (strcmp("TriggerAlarm", methodName) == 0) {
        // Output alarm using Log_Debug
        Log_Debug("Alarm triggered!\n");
        responseString = "\"Alarm Triggered\""; // must be a JSON string (in quotes)
        result = 200;
    }
    else if (strcmp("InjectInsulin", methodName) == 0) {
        // Output insulin injection using log debug
        int intPayload = atoi(payload);
        Log_Debug("Injecting %d mg insulin\n", intPayload);
        responseString = "\"Injecting insulin\""; // must be a JSON string (in quotes)
        result = 100;
    }
    else {
        // All other method names are ignored
        responseString = "{}";
        result = -1;
    }

    // if 'response' is non-NULL, the Azure IoT library frees it after use, so copy it to heap
    *responseSize = strlen(responseString);
    *response = malloc(*responseSize);
    memcpy(*response, responseString, *responseSize);
    return result;
}
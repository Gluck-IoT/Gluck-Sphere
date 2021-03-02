/* Copyright (c) Group Romeo 2021. All rights reserved.
   Licensed under the MIT License. */

#define PumpOutputPin SAMPLE_NRF52_UART

static void TerminationHandler(int signalNumber);
static int adcControllerFd = -1;
static EventLoopTimer* insulinToInject = NULL;

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

    // Open the ADC controller
    adcControllerFd = ADC_Open(SAMPLE_POTENTIOMETER_ADC_CONTROLLER);
    if (adcControllerFd == -1) {
        Log_Debug("ADC_Open failed with error: %s (%d)\n", strerror(errno), errno);
        return ExitCode_Init_AdcOpen;
    }

    // Get the sample bit count for the ADC controller
    sampleBitCount = ADC_GetSampleBitCount(adcControllerFd, SAMPLE_POTENTIOMETER_ADC_CHANNEL);
    if (sampleBitCount == -1) {
        Log_Debug("ADC_GetSampleBitCount failed with error : %s (%d)\n", strerror(errno), errno);
        return ExitCode_Init_GetBitCount;
    }
    if (sampleBitCount == 0) {
        Log_Debug("ADC_GetSampleBitCount returned sample size of 0 bits.\n");
        return ExitCode_Init_UnexpectedBitCount;
    }

    int result = ADC_SetReferenceVoltage(adcControllerFd, SAMPLE_POTENTIOMETER_ADC_CHANNEL,
        sampleMaxVoltage);
    if (result == -1) {
        Log_Debug("ADC_SetReferenceVoltage failed with error : %s (%d)\n", strerror(errno), errno);
        return ExitCode_Init_SetRefVoltage;
    }

    // Open the pins which will be used for the insulin pump
    Log_Debug("Opening pin for insulin pump as output.\n");
    deviceStatusPumpGpioFd =
        GPIO_OpenAsOutput(PumpOutputPin, GPIO_OutputMode_OpenSource, GPIO_Value_Low);
    if (deviceStatusPumpGpioFd == -1) {
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
    DisposeEventLoopTimer(insulinToInject);
    EventLoop_Close(eventLoop);

    Log_Debug("Closing file descriptors\n");

    // Leave the LEDs off
    if (deviceTwinStatusLedGpioFd >= 0) {
        GPIO_SetValue(deviceTwinStatusLedGpioFd, GPIO_Value_High);
    }

    CloseFdAndPrintError(sendMessageButtonGpioFd, "SendMessageButton");
    CloseFdAndPrintError(deviceTwinStatusLedGpioFd, "StatusLed");
    CloseFdAndPrintError(adcControllerFd, "ADC");
    CloseFdAndPrintError(deviceStatusPumpGpioFd, "Pump");
}

// Send telemetry to Azure IoT Hub.
void SendSimulatedTelemetry(void) {
    static char telemetryBuffer[TELEMETRY_BUFFER_SIZE];

    uint32_t value;
    int result = ADC_Poll(adcControllerFd, SAMPLE_POTENTIOMETER_ADC_CHANNEL, &value);
    if (result == -1) {
        Log_Debug("ADC_Poll failed with error: %s (%d)\n", strerror(errno), errno);
        exitCode = ExitCode_AdcTimerHandler_Poll;
        return;
    }

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
        responseString = "\"Injecting insulin\""; // must be a JSON string (in quotes

        // Inject the specified amount of insulin
        GPIO_SetValue(deviceStatusPumpGpioFd, GPIO_Value_High);
        CreateEventLoopDisarmedTimer(insulinToInject, InsulinTimerEventHandler);
        SetEventLoopTimerOneShot(insulinToInject, intPayload);

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

// Insulin timer event: stop injecting insulin after enough has been injected.
static void InsulinTimerEventHandler(EventLoopTimer* timer) {
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        exitCode = ExitCode_AzureTimer_Consume;
        return;
    }

    // Switch off the pump after it has injected enough insulin
    GPIO_SetValue(deviceStatusPumpGpioFd, GPIO_Value_Low);
    return;
}
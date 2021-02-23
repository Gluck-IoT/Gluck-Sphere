#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <applibs/log.h>
#include <applibs/gpio.h>

// The following #include imports a "template appliance" definition. This app comes with multiple
// implementations of the template appliance, each in a separate directory, which allow the code
// to run unchanged on different hardware.
//
// By default, this app targets hardware that follows the MT3620 Reference Development Board (RDB)
// specification, such as the MT3620 Dev Kit from Seeed Studio.
//
// To target different hardware, you'll need to update CMakeLists.txt.  For example, to target the
// Avnet MT3620 Starter Kit, make this update: azsphere_target_hardware_definition(${PROJECT_NAME}
// TARGET_DIRECTORY "HardwareDefinitions/avnet_mt3620_sk" TARGET_DEFINITION
// "template_appliance.json")
//
// See https://aka.ms/AzureSphereHardwareDefinitions for more details.
#include <hw/template_appliance.h>

// Event codes for the application. These are used for the
// application exit code, and must be between 0 and 255 inclusive,
// where zero is reserved for successful termination.
typedef enum {
    ExitCode_Success = 0,
    ExitCode_TermHandler_SigTerm = 1,
    ExitCode_TimerHandler_Consume = 2,
    ExitCode_SendMsg_Send = 3,
    ExitCode_SocketHandler_Recv = 4,
    ExitCode_Init_EventLoop = 5,
    ExitCode_Init_SendTimer = 6,
    ExitCode_Init_Connection = 7,
    ExitCode_Init_SetSockOpt = 8,
    ExitCode_Init_RegisterIo = 9,
    ExitCode_Main_EventLoopFail = 10
} ExitCode;

static int rtSockFd = -1;

static EventLoop* eventLoop = NULL;
static EventLoopTimer* sendTimer = NULL;
static EventRegistration* socketEventReg = NULL;
static volatile sig_atomic_t exitCode = ExitCode_Success;

static const char rtAppComponentId[] = "e8b60e54-a71a-4bbd-93c0-0a500a1224f5";

static void TerminationHandler(int signalNumber);
static void SendTimerEventHandler(EventLoopTimer* timer);
static void SendMessageToRTApp(void);
static void SocketEventHandler(EventLoop* el, int fd, EventLoop_IoEvents events, void* context);
static ExitCode InitHandlers(void);
static void CloseHandlers(void);

/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    exitCode = ExitCode_TermHandler_SigTerm;
}

/// <summary>
///     Handle send timer event by writing data to the real-time capable application.
/// </summary>
static void SendTimerEventHandler(EventLoopTimer* timer)
{
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        exitCode = ExitCode_TimerHandler_Consume;
        return;
    }

    SendMessageToRTApp();
}

// Send message to real-time capable application
static void SendMessageToRTApp(int amount_insulin) {
    // In this prototype, we'll instead tell the RT app to power a water pump for that many milliseconds.
    static int iter = 0;

    static char txMessage[40];
    snprintf(txMessage, sizeof(txMessage), "hl-app-to-rt-app-%04d", amount_insulin % 10000);

    Log_Debug("Sphere -> RTOS: %s\n", txMessage);

    int bytesSent = send(rtSockFd, txMessage, strlen(txMessage), 0);
    if (bytesSent == -1) {
        Log_Debug("ERROR: Unable to send message: %d (%s)\n", errno, strerror(errno));
        exitCode = ExitCode_SendMsg_Send;
        return;
    }
}

// Handle socket event by reading incoming data from real-time capable application
static void SocketEventHandler(EventLoop* el, int fd, EventLoop_IoEvents events, void* context)
{
    // Read response from real-time capable application.
    // If the RTApp has sent more than 32 bytes, then truncate.
    char rxBuf[32];
    int bytesReceived = recv(fd, rxBuf, sizeof(rxBuf), 0);

    if (bytesReceived == -1) {
        Log_Debug("ERROR: Unable to receive message: %d (%s)\n", errno, strerror(errno));
        exitCode = ExitCode_SocketHandler_Recv;
        return;
    }

    Log_Debug("Sphere received %d bytes: ", bytesReceived);
    for (int i = 0; i < bytesReceived; ++i) {
        Log_Debug("%c", isprint(rxBuf[i]) ? rxBuf[i] : '.');
    }
    Log_Debug("\n");
}

// Set up SIGTERM termination handler for send timer and to receive data from
// real-time capable application
// Returns ExitCode_Success if all resources were allocated successfully,
// otherwise returns another ExitCode value
static ExitCode InitHandlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

    eventLoop = EventLoop_Create();
    if (eventLoop == NULL) {
        Log_Debug("Could not create event loop.\n");
        return ExitCode_Init_EventLoop;
    }

    // Register a one second timer to send a message to the RTApp.
    static const struct timespec sendPeriod = { .tv_sec = 1, .tv_nsec = 0 };
    sendTimer = CreateEventLoopPeriodicTimer(eventLoop, &SendTimerEventHandler, &sendPeriod);
    if (sendTimer == NULL) {
        return ExitCode_Init_SendTimer;
    }

    // Open a connection to the RTApp.
    sockFd = Application_Connect(rtAppComponentId);
    if (sockFd == -1) {
        Log_Debug("ERROR: Unable to create socket: %d (%s)\n", errno, strerror(errno));
        return ExitCode_Init_Connection;
    }

    // Set timeout, to handle case where real-time capable application does not respond.
    static const struct timeval recvTimeout = { .tv_sec = 5, .tv_usec = 0 };
    int result = setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
    if (result == -1) {
        Log_Debug("ERROR: Unable to set socket timeout: %d (%s)\n", errno, strerror(errno));
        return ExitCode_Init_SetSockOpt;
    }

    // Register handler for incoming messages from real-time capable application.
    socketEventReg = EventLoop_RegisterIo(eventLoop, sockFd, EventLoop_Input, SocketEventHandler,
        /* context */ NULL);
    if (socketEventReg == NULL) {
        Log_Debug("ERROR: Unable to register socket event: %d (%s)\n", errno, strerror(errno));
        return ExitCode_Init_RegisterIo;
    }

    return ExitCode_Success;
}

int main(void)
{
    Log_Debug(
        "\nVisit https://github.com/Azure/azure-sphere-samples for extensible samples to use as a "
        "starting point for full applications.\n");

    int fd = GPIO_OpenAsOutput(TEMPLATE_LED, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (fd < 0) {
        Log_Debug(
            "Error opening GPIO: %s (%d). Check that app_manifest.json includes the GPIO used.\n",
            strerror(errno), errno);
        return ExitCode_Main_Led;
    }

    const struct timespec sleepTime = {.tv_sec = 1, .tv_nsec = 0};
    while (true) {
        GPIO_SetValue(fd, GPIO_Value_Low);
        nanosleep(&sleepTime, NULL);
        GPIO_SetValue(fd, GPIO_Value_High);
        nanosleep(&sleepTime, NULL);
    }
}

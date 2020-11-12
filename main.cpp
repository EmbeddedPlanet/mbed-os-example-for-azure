/*
 * Copyright (c) 2020 Arm Limited
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mbed.h"
#include "rtos/ThisThread.h"
#include "NTPClient.h"

#include "certs.h"
#include "iothub.h"
#include "iothub_client_options.h"
#include "iothub_device_client.h"
#include "iothub_message.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/tickcounter.h"
#include "azure_c_shared_utility/xlogging.h"

#include "iothubtransportmqtt.h"
#include "azure_cloud_credentials.h"

/**
 * Provisioning client includes
 */
#include "azure_prov_client/prov_transport_mqtt_client.h"
#include "azure_prov_client/prov_device_client.h"
#include "azure_prov_client/prov_security_factory.h"

#define PROVISIONING_DEMO 0
#define SAMPLE_MQTT

MU_DEFINE_ENUM_STRINGS_WITHOUT_INVALID(PROV_DEVICE_RESULT, PROV_DEVICE_RESULT_VALUE);
MU_DEFINE_ENUM_STRINGS_WITHOUT_INVALID(PROV_DEVICE_REG_STATUS, PROV_DEVICE_REG_STATUS_VALUES);

static const char* global_prov_uri = "global.azure-devices-provisioning.net";
static const char* id_scope = "0ne000FFA42";

volatile static bool g_registration_complete = false;
static bool g_use_proxy = false;
static const char* PROXY_ADDRESS = "127.0.0.1";

#define PROXY_PORT                  8888
#define MESSAGES_TO_SEND            2
#define TIME_BETWEEN_MESSAGES       2

static void registration_status_callback(PROV_DEVICE_REG_STATUS reg_status, void* user_context)
{
    (void)user_context;
    (void)printf("Provisioning Status: %s\r\n", MU_ENUM_TO_STRING(PROV_DEVICE_REG_STATUS, reg_status));
}

static void register_device_callback(PROV_DEVICE_RESULT register_result, const char* iothub_uri, const char* device_id, void* user_context)
{
    (void)user_context;
    if (register_result == PROV_DEVICE_RESULT_OK)
    {
        (void)printf("\r\nRegistration Information received from service: %s, deviceId: %s\r\n", iothub_uri, device_id);
    }
    else
    {
        (void)printf("\r\nFailure registering device: %s\r\n", MU_ENUM_TO_STRING(PROV_DEVICE_RESULT, register_result));
    }
    g_registration_complete = true;
}

/**
 * This example sends and receives messages to and from Azure IoT Hub.
 * The API usages are based on Azure SDK's official iothub_convenience_sample.
 */

// Global symbol referenced by the Azure SDK's port for Mbed OS, via "extern"
NetworkInterface *_defaultSystemNetwork;

static bool message_received = false;

static void on_connection_status(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* user_context)
{
    if (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED) {
        LogInfo("Connected to IoT Hub");
    } else {
        LogError("Connection failed, reason: %s", MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, reason));
    }
}

static IOTHUBMESSAGE_DISPOSITION_RESULT on_message_received(IOTHUB_MESSAGE_HANDLE message, void* user_context)
{
    LogInfo("Message received from IoT Hub");

    const unsigned char *data_ptr;
    size_t len;
    if (IoTHubMessage_GetByteArray(message, &data_ptr, &len) != IOTHUB_MESSAGE_OK) {
        LogError("Failed to extract message data, please try again on IoT Hub");
        return IOTHUBMESSAGE_ABANDONED;
    }

    message_received = true;
    LogInfo("Message body: %.*s", len, data_ptr);
    return IOTHUBMESSAGE_ACCEPTED;
}

static void on_message_sent(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
    if (result == IOTHUB_CLIENT_CONFIRMATION_OK) {
        LogInfo("Message sent successfully");
    } else {
        LogInfo("Failed to send message, error: %s",
            MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
    }
}

void demo() {
    bool trace_on = MBED_CONF_APP_IOTHUB_CLIENT_TRACE;
    tickcounter_ms_t interval = 100;
    IOTHUB_CLIENT_RESULT res;

    LogInfo("Initializing IoT Hub client");
    IoTHub_Init();

    IOTHUB_DEVICE_CLIENT_HANDLE client_handle = IoTHubDeviceClient_CreateFromConnectionString(
        azure_cloud::credentials::iothub_connection_string,
        MQTT_Protocol
    );
    if (client_handle == nullptr) {
        LogError("Failed to create IoT Hub client handle");
        goto cleanup;
    }

    // Enable SDK tracing
    res = IoTHubDeviceClient_SetOption(client_handle, OPTION_LOG_TRACE, &trace_on);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to enable IoT Hub client tracing, error: %d", res);
        goto cleanup;
    }

    // Enable static CA Certificates defined in the SDK
    res = IoTHubDeviceClient_SetOption(client_handle, OPTION_TRUSTED_CERT, certificates);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set trusted certificates, error: %d", res);
        goto cleanup;
    }

    // Process communication every 100ms
    res = IoTHubDeviceClient_SetOption(client_handle, OPTION_DO_WORK_FREQUENCY_IN_MS, &interval);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set communication process frequency, error: %d", res);
        goto cleanup;
    }

    // set incoming message callback
    res = IoTHubDeviceClient_SetMessageCallback(client_handle, on_message_received, nullptr);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set message callback, error: %d", res);
        goto cleanup;
    }

    // Set connection/disconnection callback
    res = IoTHubDeviceClient_SetConnectionStatusCallback(client_handle, on_connection_status, nullptr);
    if (res != IOTHUB_CLIENT_OK) {
        LogError("Failed to set connection status callback, error: %d", res);
        goto cleanup;
    }

    // Send ten message to the cloud (one per second)
    // or until we receive a message from the cloud
    IOTHUB_MESSAGE_HANDLE message_handle;
    char message[80];
    for (int i = 0; i < 10; ++i) {
        if (message_received) {
            // If we have received a message from the cloud, don't send more messeges
            break;
        }

        sprintf(message, "%d messages left to send, or until we receive a reply", 10 - i);
        LogInfo("Sending: \"%s\"", message);

        message_handle = IoTHubMessage_CreateFromString(message);
        if (message_handle == nullptr) {
            LogError("Failed to create message");
            goto cleanup;
        }

        res = IoTHubDeviceClient_SendEventAsync(client_handle, message_handle, on_message_sent, nullptr);
        IoTHubMessage_Destroy(message_handle); // message already copied into the SDK

        if (res != IOTHUB_CLIENT_OK) {
            LogError("Failed to send message event, error: %d", res);
            goto cleanup;
        }

        ThisThread::sleep_for(1s);
    }

    // If the user didn't manage to send a cloud-to-device message earlier,
    // let's wait until we receive one
    while (!message_received) {
        // Continue to receive messages in the communication thread
        // which is internally created and maintained by the Azure SDK.
        sleep();
    }

cleanup:
    IoTHubDeviceClient_Destroy(client_handle);
    IoTHub_Deinit();
}

int provisioning_demo(void) {
    SECURE_DEVICE_TYPE hsm_type;
    //hsm_type = SECURE_DEVICE_TYPE_TPM;
    //hsm_type = SECURE_DEVICE_TYPE_X509;
    hsm_type = SECURE_DEVICE_TYPE_SYMMETRIC_KEY;

    // Used to initialize IoTHub SDK subsystem
    (void)IoTHub_Init();
    (void)prov_dev_security_init(hsm_type);

    // Set the symmetric key if using they auth type
    prov_dev_set_symmetric_key_info("69ccd566-cdf2-4608-8bee-561054d744f1",
            "VV/wq59ZDfHpf4CmOiP+dTVlhASxF3/aoKR2b4S21HA=");

    HTTP_PROXY_OPTIONS http_proxy;
    PROV_DEVICE_TRANSPORT_PROVIDER_FUNCTION prov_transport;

    memset(&http_proxy, 0, sizeof(HTTP_PROXY_OPTIONS));

    // Protocol to USE - HTTP, AMQP, AMQP_WS, MQTT, MQTT_WS
#ifdef SAMPLE_MQTT
    prov_transport = Prov_Device_MQTT_Protocol;
#endif // SAMPLE_MQTT
#ifdef SAMPLE_MQTT_OVER_WEBSOCKETS
    prov_transport = Prov_Device_MQTT_WS_Protocol;
#endif // SAMPLE_MQTT_OVER_WEBSOCKETS
#ifdef SAMPLE_AMQP
    prov_transport = Prov_Device_AMQP_Protocol;
#endif // SAMPLE_AMQP
#ifdef SAMPLE_AMQP_OVER_WEBSOCKETS
    prov_transport = Prov_Device_AMQP_WS_Protocol;
#endif // SAMPLE_AMQP_OVER_WEBSOCKETS
#ifdef SAMPLE_HTTP
    prov_transport = Prov_Device_HTTP_Protocol;
#endif // SAMPLE_HTTP

    printf("Provisioning API Version: %s\r\n", Prov_Device_GetVersionString());

    if (g_use_proxy)
    {
        http_proxy.host_address = PROXY_ADDRESS;
        http_proxy.port = PROXY_PORT;
    }

    PROV_DEVICE_RESULT prov_device_result = PROV_DEVICE_RESULT_ERROR;
    PROV_DEVICE_HANDLE prov_device_handle;
    if ((prov_device_handle = Prov_Device_Create(global_prov_uri, id_scope, prov_transport)) == NULL)
    {
        (void)printf("failed calling Prov_Device_Create\r\n");
    }
    else
    {
        if (http_proxy.host_address != NULL)
        {
            Prov_Device_SetOption(prov_device_handle, OPTION_HTTP_PROXY, &http_proxy);
        }

        //bool traceOn = true;
        //Prov_Device_SetOption(prov_device_handle, PROV_OPTION_LOG_TRACE, &traceOn);
#ifdef SET_TRUSTED_CERT_IN_SAMPLES
        // Setting the Trusted Certificate. This is only necessary on systems without
        // built in certificate stores.
        Prov_Device_SetOption(prov_device_handle, OPTION_TRUSTED_CERT, certificates);
#endif // SET_TRUSTED_CERT_IN_SAMPLES

        // This option sets the registration ID it overrides the registration ID that is
        // set within the HSM so be cautious if setting this value
        //Prov_Device_SetOption(prov_device_handle, PROV_REGISTRATION_ID, "[REGISTRATION ID]");

        prov_device_result = Prov_Device_Register_Device(prov_device_handle, register_device_callback, NULL, registration_status_callback, NULL);

        (void)printf("\r\nRegistering Device\r\n\r\n");
        do
        {
            ThreadAPI_Sleep(1000);
        } while (!g_registration_complete);

        Prov_Device_Destroy(prov_device_handle);
    }
    prov_dev_security_deinit();

    // Free all the sdk subsystem
    IoTHub_Deinit();

    (void)printf("Press enter key to exit:\r\n");
    (void)getchar();

    return 0;
}

int main() {
    LogInfo("Connecting to the network");

    _defaultSystemNetwork = NetworkInterface::get_default_instance();
    if (_defaultSystemNetwork == nullptr) {
        LogError("No network interface found");
        return -1;
    }

    int ret = _defaultSystemNetwork->connect();
    if (ret != 0) {
        LogError("Connection error: %d", ret);
        return -1;
    }
    LogInfo("Connection success, MAC: %s", _defaultSystemNetwork->get_mac_address());

    LogInfo("Getting time from the NTP server");

    NTPClient ntp(_defaultSystemNetwork);
    ntp.set_server("time.google.com", 123);
    time_t timestamp = ntp.get_timestamp();
    if (timestamp < 0) {
        LogError("Failed to get the current time, error: %ld", timestamp);
        return -1;
    }
    LogInfo("Time: %s", ctime(&timestamp));
    set_time(timestamp);

#if PROVISIONING_DEMO
    LogInfo("Starting provisioning demo");
    provisioning_demo();
    LogInfo("Provisioning has ended");
#else
    LogInfo("Starting the Demo");
    demo();
    LogInfo("The demo has ended");
#endif

    return 0;
}

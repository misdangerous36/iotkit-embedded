#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "mqtt_api.h"
#include "dev_sign_api.h"
#include "mqtt_wrapper.h"
#include "infra_defs.h"
#include "infra_list.h"
#include "mqtt_internal.h"

#ifdef INFRA_MEM_STATS
    #define mqtt_api_malloc(size)            LITE_malloc(size, MEM_MAGIC, "mqtt-api")
    #define mqtt_api_free(ptr)               LITE_free(ptr)
#else
    #define mqtt_api_malloc(size)            HAL_Malloc(size)
    #define mqtt_api_free(ptr)               {HAL_Free((void *)ptr);ptr = NULL;}
#endif


#define MQTT_DEFAULT_MSG_LEN 1024

static void *g_mqtt_client = NULL;

typedef struct {
    struct list_head offline_sub_list;
    void *mutex;
} offline_sub_list_t;

/* Handle structure of subscribed topic */
typedef struct  {
    char *topic_filter;
    iotx_mqtt_event_handle_func_fpt handle;
    void *user_data;
    iotx_mqtt_qos_t qos;
    struct list_head linked_list;
} iotx_mc_offline_subs_t;

static offline_sub_list_t *_mqtt_offline_subs_list = NULL;

static int _offline_subs_list_init()
{
    if (_mqtt_offline_subs_list != NULL) {
        return 0;
    }

    _mqtt_offline_subs_list = mqtt_api_malloc(sizeof(offline_sub_list_t));

    if (_mqtt_offline_subs_list == NULL) {
        return ERROR_MALLOC;
    }

    memset(_mqtt_offline_subs_list, 0, sizeof(offline_sub_list_t));
    INIT_LIST_HEAD(&_mqtt_offline_subs_list->offline_sub_list);

    _mqtt_offline_subs_list->mutex = HAL_MutexCreate();

    if (_mqtt_offline_subs_list->mutex == NULL) {
        mqtt_api_free(_mqtt_offline_subs_list);
        _mqtt_offline_subs_list = NULL;
        return ERROR_MALLOC;
    }

    return 0;
}

static int _offline_subs_list_deinit()
{
    if (_mqtt_offline_subs_list == NULL || _mqtt_offline_subs_list->mutex == NULL) {
        return NULL_VALUE_ERROR;
    }

    iotx_mc_offline_subs_t *node = NULL, *next_node = NULL;
    list_for_each_entry_safe(node, next_node, &_mqtt_offline_subs_list->offline_sub_list, linked_list,
                             iotx_mc_offline_subs_t) {
        list_del(&node->linked_list);
        mqtt_api_free(node->topic_filter);
        mqtt_api_free(node);
    }

    HAL_MutexDestroy(_mqtt_offline_subs_list->mutex);
    mqtt_api_free(_mqtt_offline_subs_list);
    _mqtt_offline_subs_list = NULL;
    return 0;

}

static int iotx_mqtt_offline_subscribe(const char *topic_filter,
                                       iotx_mqtt_qos_t qos,
                                       iotx_mqtt_event_handle_func_fpt topic_handle_func,
                                       void *pcontext)
{
    int ret;
    if (topic_filter == NULL || topic_handle_func == NULL) {
        return NULL_VALUE_ERROR;
    }

    ret = _offline_subs_list_init();

    if (ret != 0) {
        return ret;
    }
    iotx_mc_offline_subs_t *sub_info = mqtt_api_malloc(sizeof(iotx_mc_offline_subs_t));
    if (sub_info == NULL) {
        return ERROR_MALLOC;
    }

    memset(sub_info, 0, sizeof(iotx_mc_offline_subs_t));
    sub_info->topic_filter = mqtt_api_malloc(strlen(topic_filter) + 1);
    if (sub_info->topic_filter == NULL) {
        mqtt_api_free(sub_info);
        return ERROR_MALLOC;
    }
    memset(sub_info->topic_filter, 0, strlen(topic_filter) + 1);
    strncpy(sub_info->topic_filter, topic_filter, strlen(topic_filter));
    sub_info->qos = qos;
    sub_info->handle = topic_handle_func;
    sub_info->user_data = pcontext;
    INIT_LIST_HEAD(&sub_info->linked_list);

    HAL_MutexLock(_mqtt_offline_subs_list->mutex);
    list_add_tail(&sub_info->linked_list, &_mqtt_offline_subs_list->offline_sub_list);
    HAL_MutexUnlock(_mqtt_offline_subs_list->mutex);

    return 0;
}


static int iotx_mqtt_deal_offline_subs(void *client)
{
    iotx_mc_offline_subs_t *node = NULL, *next_node = NULL;

    if (_mqtt_offline_subs_list == NULL) {
        return SUCCESS_RETURN;
    }

    HAL_MutexLock(_mqtt_offline_subs_list->mutex);
    list_for_each_entry_safe(node, next_node, &_mqtt_offline_subs_list->offline_sub_list, linked_list,
                             iotx_mc_offline_subs_t) {
        list_del(&node->linked_list);
        mqtt_subscribe_wrapper(client, node->topic_filter, node->qos, node->handle, node->user_data);
        mqtt_api_free(node->topic_filter);
        mqtt_api_free(node);
    }
    HAL_MutexUnlock(_mqtt_offline_subs_list->mutex);

    _offline_subs_list_deinit();
    return SUCCESS_RETURN;
}

static int _get_report_devinfo(char *pk, char *dn, char **url, char **payload)
{
    int url_len = 0, payload_len = 0;
    const char *url_fmt = "/sys/%s/%s/thing/deviceinfo/update";
    const char *payload_fmt = "{\"id\":\"11111111112\",\"version\":\"1.0\",\"params\":["
                              "{\"attrKey\":\"SYS_LP_SDK_VERSION\",\"attrValue\":\"%s\",\"domain\":\"SYSTEM\"},"
                              "],\"method\":\"thing.deviceinfo.update\"}";

    url_len = strlen(url_fmt) + strlen(pk) + strlen(pk) + 1;
    *url = mqtt_api_malloc(url_len);
    if (url == NULL) {
        return FAIL_RETURN;
    }
    memset(*url, 0, url_len);
    HAL_Snprintf(*url, url_len, url_fmt, pk, dn);

    payload_len = strlen(payload_fmt) + strlen(IOTX_SDK_VERSION) + 1;
    *payload = mqtt_api_malloc(payload_len);
    if (payload == NULL) {
        mqtt_api_free(*url);
        return FAIL_RETURN;
    }
    memset(*payload, 0, payload_len);
    HAL_Snprintf(*payload, payload_len, payload_fmt, IOTX_SDK_VERSION);

    return SUCCESS_RETURN;
}

static int _get_report_version(char *pk, char *dn, char **url, char **payload)
{
    int url_len = 0, payload_len = 0;
    char firmware_version[] = {0};
    const char *url_fmt = "/ota/device/inform/%s/%s";
    const char *payload_fmt = "{\"id\":\"%d\",\"params\":{\"version\":\"%s\"}}";

    HAL_GetFirmwareVersion(firmware_version);

    url_len = strlen(url_fmt) + strlen(pk) + strlen(pk) + 1;
    *url = mqtt_api_malloc(url_len);
    if (url == NULL) {
        return FAIL_RETURN;
    }
    memset(*url, 0, url_len);
    HAL_Snprintf(*url, url_len, url_fmt, pk, dn);

    payload_len = strlen(payload_fmt) + strlen(firmware_version) + 1;
    *payload = mqtt_api_malloc(payload_len);
    if (payload == NULL) {
        mqtt_api_free(*url);
        return FAIL_RETURN;
    }
    memset(*payload, 0, payload_len);
    HAL_Snprintf(*payload, payload_len, payload_fmt, firmware_version);

    return SUCCESS_RETURN;
}

static void _report_device_info(void *pclient)
{
    int res = 0;
    char *url = NULL, *payload = NULL;
    char product_key[IOTX_PRODUCT_KEY_LEN] = {0};
    char device_name[IOTX_DEVICE_NAME_LEN] = {0};

    HAL_GetProductKey(product_key);
    HAL_GetDeviceName(device_name);

    res = _get_report_devinfo(product_key, device_name, &url, &payload);
    if (res > 0) {
        IOT_MQTT_Publish_Simple(pclient, url, IOTX_MQTT_QOS0, payload, strlen(payload));
        mqtt_api_free(url);
        mqtt_api_free(payload);
    }


    res = _get_report_version(product_key, device_name, &url, &payload);
    if (res >= 0) {
        IOT_MQTT_Publish_Simple(pclient, url, IOTX_MQTT_QOS0, payload, strlen(payload));
        mqtt_api_free(url);
        mqtt_api_free(payload);
    }
}

/************************  Public Interface ************************/
void *IOT_MQTT_Construct(iotx_mqtt_param_t *pInitParams)
{
    int                 err;
    void   *pclient;
    iotx_mqtt_param_t *mqtt_params = NULL;

    if (pInitParams == NULL) {
        iotx_dev_meta_info_t meta;
        iotx_sign_mqtt_t sign_mqtt;

        if (g_mqtt_client != NULL) {
            return NULL;
        }

        mqtt_params = (iotx_mqtt_param_t *)mqtt_api_malloc(sizeof(iotx_mqtt_param_t));
        if (mqtt_params == NULL) {
            return NULL;
        }

        memset(&meta, 0, sizeof(iotx_dev_meta_info_t));
        memset(&sign_mqtt, 0, sizeof(iotx_sign_mqtt_t));

        HAL_GetProductKey(meta.product_key);
        HAL_GetDeviceName(meta.device_name);
        HAL_GetDeviceSecret(meta.device_secret);
        memset(&sign_mqtt, 0, sizeof(iotx_sign_mqtt_t));

        int ret = IOT_Sign_MQTT(IOTX_CLOUD_REGION_SHANGHAI, &meta, &sign_mqtt);
        if (ret != SUCCESS_RETURN) {
            mqtt_api_free(mqtt_params);
            return NULL;
        }
        /* Initialize MQTT parameter */
        memset(mqtt_params, 0x0, sizeof(iotx_mqtt_param_t));

        mqtt_params->port = sign_mqtt.port;
        mqtt_params->host = sign_mqtt.hostname;
        mqtt_params->client_id = sign_mqtt.clientid;
        mqtt_params->username = sign_mqtt.username;
        mqtt_params->password = sign_mqtt.password;
#ifdef SUPPORT_TLS
        extern const char *iotx_ca_crt;
        mqtt_params->pub_key = iotx_ca_crt;
#endif
        mqtt_params->request_timeout_ms    = 2000;
        mqtt_params->clean_session         = 0;
        mqtt_params->keepalive_interval_ms = 60000;
        mqtt_params->read_buf_size         = MQTT_DEFAULT_MSG_LEN;
        mqtt_params->write_buf_size        = MQTT_DEFAULT_MSG_LEN;
        mqtt_params->handle_event.h_fp     = NULL;
        mqtt_params->handle_event.pcontext = NULL;
        pInitParams = mqtt_params;
    }

    if (pInitParams->host == NULL || pInitParams->client_id == NULL ||
        pInitParams->username == NULL || pInitParams->password == NULL ||
        pInitParams->port == 0 || !strlen(pInitParams->host)) {
        mqtt_err("init params is not complete");
        if (mqtt_params != NULL) {
            mqtt_api_free(mqtt_params);

        }
        return NULL;
    }

    pclient = mqtt_init_wrapper(pInitParams);
    if (pclient == NULL) {
        if (mqtt_params != NULL) {
            mqtt_api_free(mqtt_params);
        }
    }

    err = mqtt_connect_wrapper(pclient);
    if (SUCCESS_RETURN != err) {
        mqtt_err("mqtt_connect_wrapper failed");
        mqtt_release_wrapper(pclient);
        mqtt_api_free(pclient);
        return NULL;
    }

    iotx_mqtt_deal_offline_subs(pclient);
    _report_device_info(pclient);

    g_mqtt_client = pclient;

#if defined(DEVICE_MODEL_ENABLED) && !(DEPRECATED_LINKKIT)
    void *callback = NULL;
    /* MQTT Connected Callback */
    callback = iotx_event_callback(ITE_MQTT_CONNECT_SUCC);
    if (callback) {
        ((int (*)(void))callback)();
    }
#endif

    return pclient;
}

int IOT_MQTT_Destroy(void **phandler)
{
    void *client;
    if (phandler != NULL) {
        client = *phandler;
        *phandler = NULL;
    } else {
        client = g_mqtt_client;
    }

    if (client == NULL) {
        mqtt_err("handler is null");
        return NULL_VALUE_ERROR;
    }

    mqtt_release_wrapper(&client);
    g_mqtt_client = NULL;

    return SUCCESS_RETURN;
}

int IOT_MQTT_Yield(void *handle, int timeout_ms)
{
    void *pClient = (handle ? handle : g_mqtt_client);
    return mqtt_yield_wrapper(pClient, timeout_ms);
}

/* check whether MQTT connection is established or not */
int IOT_MQTT_CheckStateNormal(void *handle)
{
    void *pClient = (handle ? handle : g_mqtt_client);
    if (pClient == NULL) {
        mqtt_err("handler is null");
        return NULL_VALUE_ERROR;
    }

    return mqtt_check_state_wrapper(pClient);
}

int IOT_MQTT_Subscribe(void *handle,
                       const char *topic_filter,
                       iotx_mqtt_qos_t qos,
                       iotx_mqtt_event_handle_func_fpt topic_handle_func,
                       void *pcontext)
{
    void *client = handle ? handle : g_mqtt_client;

    if (client == NULL) { /* do offline subscribe */
        return iotx_mqtt_offline_subscribe(topic_filter, qos, topic_handle_func, pcontext);
    }

    if (topic_filter == NULL || strlen(topic_filter) == 0 || topic_handle_func == NULL) {
        mqtt_err("params err");
        return NULL_VALUE_ERROR;
    }

    if (qos > IOTX_MQTT_QOS2) {
        mqtt_warning("Invalid qos(%d) out of [%d, %d], using %d",
                     qos,
                     IOTX_MQTT_QOS0, IOTX_MQTT_QOS2, IOTX_MQTT_QOS0);
        qos = IOTX_MQTT_QOS0;
    }

    return mqtt_subscribe_wrapper(client, topic_filter, qos, topic_handle_func, pcontext);
}

#define SUBSCRIBE_SYNC_TIMEOUT_MAX 10000
int IOT_MQTT_Subscribe_Sync(void *handle,
                            const char *topic_filter,
                            iotx_mqtt_qos_t qos,
                            iotx_mqtt_event_handle_func_fpt topic_handle_func,
                            void *pcontext,
                            int timeout_ms)
{
    void *client = handle ? handle : g_mqtt_client;

    if (client == NULL) { /* do offline subscribe */
        return iotx_mqtt_offline_subscribe(topic_filter, qos, topic_handle_func, pcontext);
    }
    if (timeout_ms > SUBSCRIBE_SYNC_TIMEOUT_MAX) {
        timeout_ms = SUBSCRIBE_SYNC_TIMEOUT_MAX;
    }

    if (topic_filter == NULL || strlen(topic_filter) == 0) {
        mqtt_err("params err");
        return NULL_VALUE_ERROR;
    }
    return mqtt_subscribe_sync_wrapper(client, topic_filter, qos, topic_handle_func, pcontext, timeout_ms);
}


int IOT_MQTT_Unsubscribe(void *handle, const char *topic_filter)
{
    void *client = handle ? handle : g_mqtt_client;


    if (client == NULL || topic_filter == NULL || strlen(topic_filter) == 0) {
        mqtt_err("params err");
        return NULL_VALUE_ERROR;
    }

    return mqtt_unsubscribe_wrapper(client, topic_filter);
}

int IOT_MQTT_Publish(void *handle, const char *topic_name, iotx_mqtt_topic_info_pt topic_msg)
{
    void *client = handle ? handle : g_mqtt_client;
    int                 rc = -1;

    if (client == NULL || topic_name == NULL || strlen(topic_name) == 0) {
        mqtt_err("params err");
        return NULL_VALUE_ERROR;
    }

    rc = mqtt_publish_wrapper(client, topic_name, topic_msg);
    return rc;
}

int IOT_MQTT_Publish_Simple(void *handle, const char *topic_name, int qos, void *data, int len)
{
    iotx_mqtt_topic_info_t mqtt_msg;
    void *client = handle ? handle : g_mqtt_client;
    int rc = -1;

    if (client == NULL || topic_name == NULL || strlen(topic_name) == 0) {
        mqtt_err("params err");
        return NULL_VALUE_ERROR;
    }

    memset(&mqtt_msg, 0x0, sizeof(iotx_mqtt_topic_info_t));

    mqtt_msg.qos         = qos;
    mqtt_msg.retain      = 0;
    mqtt_msg.dup         = 0;
    mqtt_msg.payload     = (void *)data;
    mqtt_msg.payload_len = len;

    rc = mqtt_publish_wrapper(client, topic_name, &mqtt_msg);

    if (rc < 0) {
        mqtt_err("IOT_MQTT_Publish failed\n");
        return -1;
    }

    return rc;
}
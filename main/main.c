/* Simple HTTP Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "esp_tls_crypto.h"
#include <esp_http_server.h>
#include "driver/gpio.h"
// #include "driver/adc.h"
#include "esp_adc/adc_continuous.h"
// #include "esp_adc/adc_oneshot.h"
// #include "esp_adc_cal.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "math.h"

/* A simple example that demonstrates how to create GET and POST
 * handlers for the web server.x
 */
static uint8_t s_led_state = 0;
// uint32_t reading;
// uint32_t voltage;
char voltage_txt[5];

#define NUMBER_OF_SAMPLES 2048
uint16_t voltage[NUMBER_OF_SAMPLES];
// uint16_t voltage_raw[NUMBER_OF_SAMPLES];
uint16_t voltage_copy[NUMBER_OF_SAMPLES];
int32_t timestamp[NUMBER_OF_SAMPLES];

char Vstr[NUMBER_OF_SAMPLES*10]; //leave lots of room - 10 characters for each entry
// char Vstr_raw[NUMBER_OF_SAMPLES];

static const char *TAG = "example";

#if CONFIG_EXAMPLE_BASIC_AUTH

typedef struct {
    char    *username;
    char    *password;
} basic_auth_info_t;

#define HTTPD_401      "401 UNAUTHORIZED"           /*!< HTTP Response 401 */

static char *http_auth_basic(const char *username, const char *password)
{
    int out;
    char *user_info = NULL;
    char *digest = NULL;
    size_t n = 0;
    asprintf(&user_info, "%s:%s", username, password);
    if (!user_info) {
        ESP_LOGE(TAG, "No enough memory for user information");
        return NULL;
    }
    esp_crypto_base64_encode(NULL, 0, &n, (const unsigned char *)user_info, strlen(user_info));

    /* 6: The length of the "Basic " string
     * n: Number of bytes for a base64 encode format
     * 1: Number of bytes for a reserved which be used to fill zero
    */
    digest = calloc(1, 6 + n + 1);
    if (digest) {
        strcpy(digest, "Basic ");
        esp_crypto_base64_encode((unsigned char *)digest + 6, n, (size_t *)&out, (const unsigned char *)user_info, strlen(user_info));
    }
    free(user_info);
    return digest;
}

/* An HTTP GET handler */
static esp_err_t basic_auth_get_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_len = 0;
    basic_auth_info_t *basic_auth_info = req->user_ctx;

    buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len > 1) {
        buf = calloc(1, buf_len);
        if (!buf) {
            ESP_LOGE(TAG, "No enough memory for basic authorization");
            return ESP_ERR_NO_MEM;
        }

        if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Authorization: %s", buf);
        } else {
            ESP_LOGE(TAG, "No auth value received");
        }

        char *auth_credentials = http_auth_basic(basic_auth_info->username, basic_auth_info->password);
        if (!auth_credentials) {
            ESP_LOGE(TAG, "No enough memory for basic authorization credentials");
            free(buf);
            return ESP_ERR_NO_MEM;
        }

        if (strncmp(auth_credentials, buf, buf_len)) {
            ESP_LOGE(TAG, "Not authenticated");
            httpd_resp_set_status(req, HTTPD_401);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "keep-alive");
            httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Hello\"");
            httpd_resp_send(req, NULL, 0);
        } else {
            ESP_LOGI(TAG, "Authenticated!");
            char *basic_auth_resp = NULL;
            httpd_resp_set_status(req, HTTPD_200);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "keep-alive");
            asprintf(&basic_auth_resp, "{\"authenticated\": true,\"user\": \"%s\"}", basic_auth_info->username);
            if (!basic_auth_resp) {
                ESP_LOGE(TAG, "No enough memory for basic authorization response");
                free(auth_credentials);
                free(buf);
                return ESP_ERR_NO_MEM;
            }
            httpd_resp_send(req, basic_auth_resp, strlen(basic_auth_resp));
            free(basic_auth_resp);
        }
        free(auth_credentials);
        free(buf);
    } else {
        ESP_LOGE(TAG, "No auth header received");
        httpd_resp_set_status(req, HTTPD_401);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Hello\"");
        httpd_resp_send(req, NULL, 0);
    }

    return ESP_OK;
}

static httpd_uri_t basic_auth = {
    .uri       = "/basic_auth",
    .method    = HTTP_GET,
    .handler   = basic_auth_get_handler,
};

static void httpd_register_basic_auth(httpd_handle_t server)
{
    basic_auth_info_t *basic_auth_info = calloc(1, sizeof(basic_auth_info_t));
    if (basic_auth_info) {
        basic_auth_info->username = CONFIG_EXAMPLE_BASIC_AUTH_USERNAME;
        basic_auth_info->password = CONFIG_EXAMPLE_BASIC_AUTH_PASSWORD;

        basic_auth.user_ctx = basic_auth_info;
        httpd_register_uri_handler(server, &basic_auth);
    }
}
#endif

/* An HTTP GET handler */
static esp_err_t hello_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;

    ESP_LOGI(TAG, "Turning the LED %s!", s_led_state == true ? "ON" : "OFF");
    s_led_state = !s_led_state;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-2") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-2", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-2: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-1") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Test-Header-1", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-1: %s", buf);
        }
        free(buf);
    }

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[32];
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "query1", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query1=%s", param);
            }
            if (httpd_query_key_value(buf, "query3", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query3=%s", param);
            }
            if (httpd_query_key_value(buf, "query2", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query2=%s", param);
            }
        }
        free(buf);
    }

    char* led_state_string="OFF";

    led_state_string = (s_led_state == true ? "ON" : "OFF");
    ESP_LOGI(TAG, "s_led_state is: %i", s_led_state);
    ESP_LOGI(TAG, "led state string is: %s",led_state_string);

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    char foo[100];
    strcpy(foo,req->user_ctx);
    strcat(foo,led_state_string);

    // const char* resp_str = (const char*) req->user_ctx;
    const char* resp_str = (const char*) foo;

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0) {
        ESP_LOGI(TAG, "Request headers lost");
    }
    return ESP_OK;
}

static const httpd_uri_t hello = {
    .uri       = "/hello",
    .method    = HTTP_GET,
    .handler   = hello_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = "EV Charger is: "
};
/* An HTTP GET handler for reading ADC */
// static esp_err_t hello_adc_handler(httpd_req_t *req)
// {
//     /* Set some custom headers */
//     httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
//     httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

//     /* Send response with custom headers and body set as the
//      * string passed in user context*/
//     char foo[100];
//     strcpy(foo,req->user_ctx);
//     sprintf(voltage_txt,"%lu",voltage);
//     strcat(foo,voltage_txt);

//     // const char* resp_str = (const char*) req->user_ctx;
//     const char* resp_str = (const char*) foo;

//     httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

//     /* After sending the HTTP response the old HTTP request
//      * headers are lost. Check if HTTP request headers can be read now. */
//     // if (httpd_req_adc_hdr_value_len(req, "Host") == 0) {
//     //     ESP_LOGI(TAG, "Request headers lost");
//     // }
//     return ESP_OK;
// }

// static const httpd_uri_t adc = {
//     .uri       = "/adc",
//     .method    = HTTP_GET,
//     .handler   = hello_adc_handler,
//     /* Let's pass response string in user
//      * context to demonstrate it's usage */
//     .user_ctx  = "ADC value is: "
// };

/* An HTTP POST handler */
static esp_err_t echo_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret, remaining = req->content_len;

    while (remaining > 0) {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                        MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Send back the same data */
        httpd_resp_send_chunk(req, buf, ret);
        remaining -= ret;

        /* Log data received */
        ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "====================================");
    }

    // End response
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t echo = {
    .uri       = "/echo",
    .method    = HTTP_POST,
    .handler   = echo_post_handler,
    .user_ctx  = NULL
};

/* This handler allows the custom error handling functionality to be
 * tested from client side. For that, when a PUT request 0 is sent to
 * URI /ctrl, the /hello and /echo URIs are unregistered and following
 * custom error handler http_404_error_handler() is registered.
 * Afterwards, when /hello or /echo is requested, this custom error
 * handler is invoked which, after sending an error message to client,
 * either closes the underlying socket (when requested URI is /echo)
 * or keeps it open (when requested URI is /hello). This allows the
 * client to infer if the custom error handler is functioning as expected
 * by observing the socket state.
 */
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/hello", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        /* Return ESP_OK to keep underlying socket open */
        return ESP_OK;
    } else if (strcmp("/echo", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/echo URI is not available");
        /* Return ESP_FAIL to close underlying socket */
        return ESP_FAIL;
    }
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}

/* An HTTP PUT handler. This demonstrates realtime
 * registration and deregistration of URI handlers
 */
static esp_err_t ctrl_put_handler(httpd_req_t *req)
{
    char buf;
    int ret;

    if ((ret = httpd_req_recv(req, &buf, 1)) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }

    if (buf == '0') {
        /* URI handlers can be unregistered using the uri string */
        ESP_LOGI(TAG, "Unregistering /hello and /echo URIs");
        httpd_unregister_uri(req->handle, "/hello");
        httpd_unregister_uri(req->handle, "/echo");
        /* Register the custom error handler */
        httpd_register_err_handler(req->handle, HTTPD_404_NOT_FOUND, http_404_error_handler);
    }
    else {
        ESP_LOGI(TAG, "Registering /hello and /echo URIs");
        httpd_register_uri_handler(req->handle, &hello);
        httpd_register_uri_handler(req->handle, &echo);
        /* Unregister custom error handler */
        httpd_register_err_handler(req->handle, HTTPD_404_NOT_FOUND, NULL);
    }

    /* Respond with empty body */
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
};
/////////////
static const httpd_uri_t ctrl = {
    .uri       = "/ctrl",
    .method    = HTTP_PUT,
    .handler   = ctrl_put_handler,
    .user_ctx  = NULL
};
//////////////////
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &hello);
        // httpd_register_uri_handler(server, &adc);
        httpd_register_uri_handler(server, &echo);
        httpd_register_uri_handler(server, &ctrl);
        #if CONFIG_EXAMPLE_BASIC_AUTH
        httpd_register_basic_auth(server);
        #endif
        return server;
    };
    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}
////////////////
static void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}
////////////////
static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}
////////////////
static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}
// ***** function calls ******
int compare (const void * a, const void * b) {
    // ESP_LOGI(TAG,"a:%i   b:%i  Compare returns:%i", *(int*)a&0xfff,*(int*)b&0xfff,(*((int*)a)&0xfff) - (*((int*)b)&0xfff));
    return ( *(uint16_t*)a - *(uint16_t*)b );
}
////////////////
/*---------------------------------------------------------------
        ADC Calibration
---------------------------------------------------------------*/
adc_cali_handle_t adc1_cali_handle = NULL;

static bool example_adc_calibration_init(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

//
// ** start setup for adc_continuous
//
//
#define EXAMPLE_READ_LEN   2048
#define GET_UNIT(x)        ((x>>3) & 0x1)
#define ADC_CONV_MODE       ADC_CONV_SINGLE_UNIT_1  //ESP32 only supports ADC1 DMA mode
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE1


static adc_channel_t channel[1] = {ADC_CHANNEL_7};
static TaskHandle_t s_task_handle;
static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    //Notify that ADC continuous driver has done enough number of conversions
    vTaskNotifyGiveFromISR(s_task_handle, &mustYield);

    return (mustYield == pdTRUE);
}
static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle,adc_cali_handle_t *cali_handle)
{
    adc_continuous_handle_t handle = NULL;
    adc_cali_handle_t c_handle = NULL;

    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 4096,
        .conv_frame_size = EXAMPLE_READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = 20 * 1000,  //read at 20KHz to sample   
        .conv_mode = ADC_CONV_MODE,
        .format = ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    // ESP_LOGI(TAG, "channel_num is:%x", channel_num);

    for (int i = 0; i < channel_num; i++) {
        uint8_t unit = GET_UNIT(channel[i]);
        uint8_t ch = channel[i] & 0x7;
        adc_pattern[i].atten = ADC_ATTEN_DB_0;
        adc_pattern[i].channel = ch;
        adc_pattern[i].unit = unit;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

        // ESP_LOGI(TAG, "adc_pattern[%d].atten is :%x", i, adc_pattern[i].atten);
        // ESP_LOGI(TAG, "adc_pattern[%d].channel is :%x", i, adc_pattern[i].channel);
        // ESP_LOGI(TAG, "adc_pattern[%d].unit is :%x", i, adc_pattern[i].unit);

        // ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = ADC_ATTEN_DB_0,
            .bitwidth = SOC_ADC_DIGI_MAX_BITWIDTH,
            };
        ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_config, &c_handle));
        }  
    dig_cfg.adc_pattern = adc_pattern;
    // ESP_LOGI(TAG, "dig_cfg.adc_pattern[0].atten is :%x", dig_cfg.adc_pattern[0].atten);
    // ESP_LOGI(TAG, "dig_cfg.adc_pattern[0].channel is :%x", dig_cfg.adc_pattern[0].channel);
    // ESP_LOGI(TAG, "dig_cfg.adc_pattern[0].unit is :%x", dig_cfg.adc_pattern[0].unit);

    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));

    *out_handle = handle;
    *cali_handle = c_handle;

    bool do_calibration1 = example_adc_calibration_init(ADC_UNIT_1, ADC_ATTEN_DB_0, &adc1_cali_handle);
}

// ** finish setup for adc_continuous
//
//
// ************
int get_Vrms(adc_continuous_handle_t handle, adc_cali_handle_t cali_handle) {

    esp_err_t ret;
    uint32_t ret_num = 0;
    uint8_t result[EXAMPLE_READ_LEN];
    memset(result, 0xcc, EXAMPLE_READ_LEN);
    int i=0,j=0,V=0;
    uint16_t Vsample=0;
    // uint16_t Vrawsample=0;
    uint16_t Vout;

    uint16_t least_voltage,peak_voltage,median_voltage;
    // ESP_LOGI(TAG, "Starting get_Vrms...");

    int samples=0;

    memset(voltage,0,NUMBER_OF_SAMPLES*sizeof(voltage[0]));
    memset(voltage_copy,0,NUMBER_OF_SAMPLES*sizeof(voltage_copy[0]));
    // memset(voltage_raw,0,NUMBER_OF_SAMPLES*sizeof(voltage_raw[0]));

    s_task_handle = xTaskGetCurrentTaskHandle();

    // adc_continuous_handle_t handle = NULL;
    // adc_cali_handle_t cali_handle = NULL;

    // ESP_LOGI(TAG, "about to call continuous_adc_initadc_init()...");
    // continuous_adc_init(channel, sizeof(channel) / sizeof(adc_channel_t), &handle, &cali_handle);
    // ESP_LOGI(TAG, "called continuous_adc_init()...");

    // adc_continuous_evt_cbs_t cbs = {
    //     .on_conv_done = s_conv_done_cb,
    // };
    // ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
    // ESP_ERROR_CHECK(adc_continuous_start(handle));

    // ESP_LOGI(TAG, "Starting sample loop...");

    // while(j<NUMBER_OF_SAMPLES) {
        /**
         * This is to show you the way to use the ADC continuous mode driver event callback.
         * This `ulTaskNotifyTake` will block when the data processing in the task is fast.
         * However in this example, the data processing (print) is slow, so you barely block here.
         *
         * Without using this event callback (to notify this task), you can still just call
         * `adc_continuous_read()` here in a loop, with/without a certain block timeout.
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // ret = adc_continuous_start(handle);
       
        ret = adc_continuous_read(handle, result, EXAMPLE_READ_LEN, &ret_num, 0);
        if (ret == ESP_OK) {
            // ESP_LOGI("TASK", "ret is %x, ret_num is %"PRIu32, ret, ret_num);
            V=0;
            int Vr=0;
            Vstr[0]='\0';
            // Vstr_raw[0]='\0';

            // for (i=0;i<0xfff;i++) {
            //     ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, i, &Vr));
            //     ESP_LOGI(TAG, "adc_cali_raw_to_voltage:  input:%x  output %x",i,Vr);
            // };


            for (int i = 0;i < ret_num;i += SOC_ADC_DIGI_RESULT_BYTES) {
                adc_digi_output_data_t *p = (void*)&result[i];
                // ESP_LOGI(TAG, "Unit: %d, Channel: %d, Value: %x", 1, p->type1.channel, p->type1.data);
                // ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, (int) p->type1.data, &Vout));
                Vout=p->type1.data;
                ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, Vout, &Vr));
                // ESP_LOGI(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT_1 + 1, EXAMPLE_ADC1_CHAN0, voltage[0][0]);
                // ESP_LOGI(TAG, "%i: p->type1.data:%i V:%i  Vr:%i",i,(int) p->type1.data, V,Vr);
                voltage[samples]=Vr;
                voltage_copy[samples]=Vr;

                // Vr+= Vout;
                if (p->type1.data > 0xfff) ESP_LOGI(TAG,"p->type1.data too big: %i",p->type1.data);
                // if ((samples % 1000)==0) ESP_LOGI(TAG, "p->type1.data:%i Vout:%i",(int) p->type1.data, Vout);

                samples++;
            }
            // ESP_LOGI(TAG,"number of samples:%i  V:%i",samples,V); 

            // if (samples>0) {
            //     Vsample=(int)(V/samples);  // Average value of the sampled voltage (sampled at 20KHz (or more!)
                // Vrawsample=(int)(Vr/samples);
                samples=0;
                // ESP_LOGI(TAG,"%i: Vrawsample:%i",j,Vrawsample);
            // }
            /**
             * Because printing is slow, so every time you call `ulTaskNotifyTake`, it will immediately return.
             * To avoid a task watchdog timeout, add a delay here. When you replace the way you process the data,
             * usually you don't need this delay (as this task will block for a while).
             */
            // vTaskDelay(1);
        } else if (ret == ESP_ERR_TIMEOUT) {
            // j=NUMBER_OF_SAMPLES;
            ESP_LOGI(TAG,"ESP_ERR_TIMEOUT");
            // break;
        }
    // }
        // if (j>NUMBER_OF_SAMPLES) ESP_LOGI(TAG,"j:%i\n",j);
        // if (Vsample>0xfff) ESP_LOGI(TAG,"Vsample too big: %i",Vsample);

        // voltage[j]= (uint16_t) Vsample&0xfff;
        // voltage_copy[j]= (uint16_t) Vsample&0xfff;
        // timestamp[j]=xTaskGetTickCount();  
        // ESP_LOGI(TAG,"in loop: voltage[%i]:%x    voltage_copy[%i]:%x",j,voltage[j],j,voltage_copy[j]);
        // voltage_raw[j]=sample&0xfff;
    //     j++;
    // }

    int number_of_samples=EXAMPLE_READ_LEN;

    // ESP_ERROR_CHECK(adc_continuous_stop(handle));
    // ESP_ERROR_CHECK(adc_continuous_deinit(handle));

    // ESP_LOGI(TAG,"Length of voltage[] is: %i\n",sizeof(voltage)/sizeof(voltage[0]));
    // ESP_LOGI(TAG,"Length of voltage_copy[] is: %i\n",sizeof(voltage_copy)/sizeof(voltage_copy[0]));

    // ESP_LOGI(TAG,"calculating Vstr ...");
    
    // if (j>NUMBER_OF_SAMPLES) ESP_LOGI(TAG,"j > NUMBER_OF_SAMPLES:%i\n",j);

    memset(Vstr,'\0',sizeof(Vstr)*sizeof(Vstr[0]));
    // Vstr[0]='\0';

    // for (i=0;i<NUMBER_OF_SAMPLES;i++) {
    //     ESP_LOGI(TAG,"before sprintf: voltage[%i]:%x    voltage_copy[%i]:%x",i,voltage[i],i,voltage_copy[i]);
    //     }

    for (i=0;i<number_of_samples;i++) {
        sprintf(Vstr+strlen(Vstr),"%x,",(unsigned int)voltage[i]);
        // ESP_LOGI(TAG,"voltage[%i]:%x",i,(int)voltage[i]);
    };

    // for (i=0;i<NUMBER_OF_SAMPLES;i++) {
    //     ESP_LOGI(TAG,"after sprintf: voltage[%i]:%x    voltage_copy[%i]:%x",i,voltage[i],i,voltage_copy[i]);
    //     };

    int n=0;
    // for (i=0;i<strlen(Vstr);i++) {if(Vstr[i]==',') n++;}
    // ESP_LOGI(TAG,"elements:%i voltage:%s\n",n,Vstr);

    // memset(Vstr,'\0',sizeof(Vstr)*sizeof(Vstr[0]));

    // for (i=0;i<number_of_samples;i++) {
    //     sprintf(Vstr+strlen(Vstr),"%x,",voltage_copy[i]);
        // ESP_LOGI(TAG,"voltage_copy[%i]:%x",i,(int)voltage_copy[i]);
    // };
    // n=0;
    // for (i=0;i<strlen(Vstr);i++) {if(Vstr[i]==',') n++;}
    // ESP_LOGI(TAG,"elements:%i voltage_copy:%s\n",n,Vstr);


    n=0;
    // Vstr_raw[0]='\0';
    // for (i=0;i<number_of_samples;i++) {
    //     sprintf(Vstr_raw+strlen(Vstr_raw),"%x,",0xfff&voltage_raw[i]);
    // };
    // for (i=0;i<strlen(Vstr_raw);i++) {if(Vstr_raw[i]==',') n++;}
    // ESP_LOGI(TAG,"elements:%i voltage_raw:%s\n",n,Vstr_raw);

    // ESP_LOGI(TAG, "About to qsort...");

    // ESP_LOGI(TAG,"voltage_copy:%i, (voltage_copy, sizeof(voltage_copy)/sizeof(voltage_copy[0]):%i, sizeof(voltage_copy[0]):%i",sizeof(voltage_copy), sizeof(voltage_copy)/sizeof(voltage_copy[0]), sizeof(voltage_copy[0]));  

    Vstr[0]='\0';
    sprintf(Vstr+strlen(Vstr),"\nmS,mV\n");
    for (i=0;i<number_of_samples;i++) {
        sprintf(Vstr+strlen(Vstr),"%i,%i\n",i,voltage_copy[i]);
    };
    n=0;
    for (i=0;i<strlen(Vstr);i++) {if(Vstr[i]==',') n++;}
    ESP_LOGI(TAG,"elements:%i in not yet qsorted voltage_copy:%s\n",n,Vstr);

    // ESP_LOGI(TAG,"Start qsort");
    qsort(voltage_copy, sizeof(voltage_copy)/sizeof(voltage_copy[0]), sizeof(voltage_copy[0]), compare);  // sort the voltage array to get voltage bounds
    // ESP_LOGI(TAG,"Finish qsort");
    least_voltage=voltage_copy[0];
    peak_voltage=voltage_copy[number_of_samples-1];
    median_voltage=voltage_copy[number_of_samples>>1];  // counter/2
    ESP_LOGW(TAG, "median_voltage:%i  least_voltage:%i  peak_voltage:%i",median_voltage, least_voltage, peak_voltage);
    Vstr[0]='\0';for (i=0;i<number_of_samples;i++) {sprintf(Vstr+strlen(Vstr),"%x,",voltage_copy[i]);}; ESP_LOGI(TAG,"qsorted voltage_copy :%s",Vstr);

    // calculate RMS
    int Vac=0; //,bitflip=2;
    long long int Vrms=0;
    int Vave=0;
    for (i=0;i<number_of_samples;i++) {
        Vac=((int)voltage[i]&0xfff)-median_voltage;
        Vrms+= Vac*Vac;
        // ESP_LOGI(TAG,"Vac[%i]:%i    sum(Vrms):%lli  voltage[i]:%i    median_voltage:%i",i,Vac,Vrms,voltage[i]&0xfff,median_voltage);
        Vave+=(Vac+median_voltage);
        // if (Vac*(voltage[i-1]-median_voltage)<0) {bitflip++;}
        // ESP_LOGI("Voltage,  ","Bitflip:%i",bitflip);
    }
    // ESP_LOGI(TAG,"Sum of Vac squared:%lli   number_of_samples: %i",Vrms,number_of_samples);
    // ESP_LOGI(TAG,"Sum of Vac squared/number_of_samples: %lli",(long long int)(Vrms/number_of_samples));
    // ESP_LOGI(TAG,"sqrt(Vac squared/number_of_samples): %f",sqrt(Vrms/number_of_samples));
    Vrms=(int)sqrt(Vrms/number_of_samples); // root of mean of voltages squared

    Vave=Vave/number_of_samples;
    ESP_LOGW(TAG,"\033[1;35m Vave:%i (%x) Vave:%.2f",Vave,Vave,(double)Vave/0xfff*4.1);
    return Vrms;
}
// ************
httpd_handle_t start_wifi_and_webserver() {
    static httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
     // This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     // * Read "Establishing Wi-Fi or Ethernet Connection" section in
     // * examples/protocols/README.md for more information about this function.
    ESP_ERROR_CHECK(example_connect());

     // Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     // * and re-start it upon connection.
     
    #ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
    #endif // CONFIG_EXAMPLE_CONNECT_WIFI
    #ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
    #endif // CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_LOGI(TAG, "Started WiFi");

    server=start_webserver();  // Start the server for the first time 
    ESP_LOGI(TAG, "Started webserver!!!");
    return server;
}
// ************
void app_main(void)
{
    // start_wifi_and_webserver();

    // initialize relay control GPIO
    #define GPIO_PIN 18
    gpio_reset_pin(GPIO_PIN);
     // Set the GPIO as a push/pull output
    gpio_set_direction(GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_PIN, false);
    static uint8_t s_led_state = 0;

    int Vrms = 0;

    // start loop 
    // const TickType_t xDelay = 500;// / portTICK_PERIOD_MS;

    adc_continuous_handle_t handle = NULL;
    adc_cali_handle_t cali_handle = NULL;
    continuous_adc_init(channel, sizeof(channel) / sizeof(adc_channel_t), &handle, &cali_handle);

    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = s_conv_done_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
    ESP_ERROR_CHECK(adc_continuous_start(handle));


    while (1) {
        Vrms=get_Vrms(handle, cali_handle);
        ESP_LOGW(TAG,"\033[1;34m Vrms:%d (%x)  Vrms:%f\n",Vrms,Vrms,((double)Vrms/0xfff)*4.1);
        // Toggle the LED state 
        // ESP_LOGI(TAG, "Turning the LED %s!", s_led_state == true ? "ON" : "OFF");
        gpio_set_level(GPIO_PIN, s_led_state); 
        // vTaskDelay(xDelay);
    }
    // ESP_ERROR_CHECK(adc_continuous_stop(handle));
    // ESP_ERROR_CHECK(adc_continuous_deinit(handle));

}

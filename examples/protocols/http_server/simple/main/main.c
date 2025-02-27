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
#include "esp_mac.h"
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

#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"

#include "esp_log.h"

/* Based on a simple example in esp-idf download bundle that demonstrates how to create GET and POST
 * handlers for the web server.x
 */
static uint8_t s_led_state = 0;
// uint32_t reading;
// uint32_t voltage;
char voltage_txt[5];

char command_buffer[16];
int command_buffer_len=0;

char call_juicier_return_buffer[16];

const int grid_voltage=240;
int64_t last_timestamp=0;

#define NUMBER_OF_SAMPLES 100
#define EXAMPLE_READ_LEN   256

#define EXAMPLE_ADC_CONV_MODE           ADC_CONV_SINGLE_UNIT_1

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define EXAMPLE_ADC_USE_OUTPUT_TYPE1    1
#define EXAMPLE_ADC_OUTPUT_TYPE         ADC_DIGI_OUTPUT_FORMAT_TYPE1
#else
#define EXAMPLE_ADC_OUTPUT_TYPE         ADC_DIGI_OUTPUT_FORMAT_TYPE2
#endif

char UID[22];

#if CONFIG_IDF_TARGET_ESP32
// static adc_channel_t channel[2] = {ADC_CHANNEL_6, ADC_CHANNEL_7};
static adc_channel_t channel[1] = {ADC_CHANNEL_7};  //ADC1_7  GPI035
#else
static adc_channel_t channel[2] = {ADC_CHANNEL_2, ADC_CHANNEL_3};
#endif

static TaskHandle_t s_task_handle;
static const char *TAG = "EXAMPLE";

uint16_t voltage[NUMBER_OF_SAMPLES];
// uint16_t voltage_raw[NUMBER_OF_SAMPLES];
uint16_t voltage_copy[NUMBER_OF_SAMPLES];

char Vstr[NUMBER_OF_SAMPLES*10]; //leave lots of room - 10 characters for each entry
// char Vstr_raw[NUMBER_OF_SAMPLES];

#if CONFIG_EXAMPLE_BASIC_AUTH

typedef struct {
    char    *username;
    char    *password;
} basic_auth_info_t;

#define HTTPD_401      "401 UNAUTHORIZED"           /*!< HTTP Response 401 */
// ************
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

    ESP_LOGI(TAG, "hello_get_handler: Turning the LED %s!", s_led_state == true ? "ON" : "OFF");
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
//****
static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    //Notify that ADC continuous driver has done enough number of conversions
    vTaskNotifyGiveFromISR(s_task_handle, &mustYield);

    return (mustYield == pdTRUE);
}
// *****
static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle)
{
    adc_continuous_handle_t handle = NULL;

    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 1024,
        .conv_frame_size = EXAMPLE_READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = 20 * 1000,
        .conv_mode = EXAMPLE_ADC_CONV_MODE,
        .format = EXAMPLE_ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        uint8_t unit = ADC_UNIT_1;
        uint8_t ch = channel[i] & 0x7;
        adc_pattern[i].atten = ADC_ATTEN_DB_11;
        adc_pattern[i].channel = ch;
        adc_pattern[i].unit = unit;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

        // ESP_LOGI(TAG, "adc_pattern[%d].atten is :%x", i, adc_pattern[i].atten);
        // ESP_LOGI(TAG, "adc_pattern[%d].channel is :%x", i, adc_pattern[i].channel);
        // ESP_LOGI(TAG, "adc_pattern[%d].unit is :%x", i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));

    *out_handle = handle;
}

static bool check_valid_data(const adc_digi_output_data_t *data)
{
#if EXAMPLE_ADC_USE_OUTPUT_TYPE1
    if (data->type1.channel >= SOC_ADC_CHANNEL_NUM(ADC_UNIT_1)) {
        return false;
    }
#else
    if (data->type2.channel >= SOC_ADC_CHANNEL_NUM(ADC_UNIT_1)) {
        return false;
    }
#endif

    return true;
}
// ************
int get_Vrms() {
    esp_err_t ret;
    uint32_t ret_num = 0;
    uint8_t result[EXAMPLE_READ_LEN] = {0};
    memset(result, 0xcc, EXAMPLE_READ_LEN);

    // ESP_LOGW(TAG,"Entering get_Vrms()");

    int num_samples=0,i=0,V[128*10]={0,0,0,0,0,0,0,0,0,0,0};
    int Vrms=0,Vave=0;

    s_task_handle = xTaskGetCurrentTaskHandle();

    adc_continuous_handle_t handle = NULL;
    // ESP_LOGW(TAG,"p5");
    continuous_adc_init(channel, sizeof(channel) / sizeof(adc_channel_t), &handle);
    // ESP_LOGW(TAG,"p6");

    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = s_conv_done_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
    ESP_ERROR_CHECK(adc_continuous_start(handle));

    // while(1) {

        /**
         * This is to show you the way to use the ADC continuous mode driver event callback.
         * This `ulTaskNotifyTake` will block when the data processing in the task is fast.
         * However in this example, the data processing (print) is slow, so you barely block here.
         *
         * Without using this event callback (to notify this task), you can still just call
         * `adc_continuous_read()` here in a loop, with/without a certain block timeout.
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (1) {
            ret = adc_continuous_read(handle, result, EXAMPLE_READ_LEN, &ret_num, 0);
            if (ret == ESP_OK) {
                // ESP_LOGI("TASK", "ret is %x, ret_num is %"PRIu32, ret, ret_num); 
                for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
                    adc_digi_output_data_t *p = (void*)&result[i];
                    if (check_valid_data(p)) {
                #if EXAMPLE_ADC_USE_OUTPUT_TYPE1
                        // ESP_LOGI(TAG, "Unit: %d, Channel: %d, Value: %x", 1, p->type1.channel, p->type1.data);
                        Vave+=p->type1.data;
                        V[num_samples]=p->type1.data;  // collect 128 samples in inner loop
                        // ESP_LOGI(TAG, "Value: %i", p->type1.data);

                        num_samples++;
                // #else
                //         ESP_LOGI(TAG, "Unit: %d,_Channel: %d, Value: %x", 1, p->type2.channel, p->type2.data);
                       
                #endif
                    } else {
                        ESP_LOGI(TAG, "Invalid data");
                    }
                }
                /**
                 * Because printing is slow, so every time you call `ulTaskNotifyTake`, it will immediately return.
                 * To avoid a task watchdog timeout, add a delay here. When you replace the way you process the data,
                 * usually you don't need this delay (as this task will block for a while).
                 */
                vTaskDelay(2);

            } else if (ret == ESP_ERR_TIMEOUT) {
                //We try to read `EXAMPLE_READ_LEN` until API returns timeout, which means there's no available data
                ESP_LOGI(TAG,"ESP_ERR_TIMEOUT");
                Vrms=0;
                break;
            }
        if (num_samples>=128*10) break;
        }
    // }

    if(num_samples!=0) {    // calculate VMS even if not a sign wave 
        Vave=Vave/num_samples;  // calculate DC offset

        // find zero crossing at start of waveform
        int rising_zero_crossing_waveform_first=0,rising_zero_crossing_waveform_last=0;
        rising_zero_crossing_waveform_last=num_samples;
        int j=0,Vtmp0=0,Vtmp1=0;
        #define SAMPLE_SIZE 5

        for (i=0;i<num_samples-(SAMPLE_SIZE*2);i++){    // get first zero crossing rising x intercept
            Vtmp0=Vtmp1=0;
            for(j=0;j<SAMPLE_SIZE;j++) {
                Vtmp0+=V[i];
                Vtmp1+=V[i+SAMPLE_SIZE];
            }
            Vtmp0=(Vtmp0/SAMPLE_SIZE)-Vave;
            Vtmp1=(Vtmp1/SAMPLE_SIZE)-Vave;
            if (Vtmp0<0 && Vtmp1>0) {
                rising_zero_crossing_waveform_first=i+SAMPLE_SIZE;
                break;
            }
        }
        for (i=num_samples-(SAMPLE_SIZE*2);i>0;i--){      // get last zero crossing rising x intercept
            Vtmp0=Vtmp1=0;
            for(j=0;j<SAMPLE_SIZE;j++) {
                Vtmp0+=V[i];
                Vtmp1+=V[i+SAMPLE_SIZE];
            }
            Vtmp0=(Vtmp0/5)-Vave;
            Vtmp1=(Vtmp1/5)-Vave;
            if (Vtmp0<0 && Vtmp1>0) {
                rising_zero_crossing_waveform_last=i-SAMPLE_SIZE;
                break;
            }
        }
        Vrms=0;
        for(i=rising_zero_crossing_waveform_first;i<rising_zero_crossing_waveform_last;i++){
            Vrms+=(V[i]-Vave)*(V[i]-Vave);
        }
        Vrms=sqrt(Vrms/(rising_zero_crossing_waveform_last-rising_zero_crossing_waveform_first));
        ESP_LOGI(TAG, "num_samples: %i Vrms: %i  Amps: %f   Vave: %i",num_samples,Vrms,(float)Vrms/37,Vave);  // divide by 37 determined empircally for amps
    }
    ESP_ERROR_CHECK(adc_continuous_stop(handle));
    ESP_ERROR_CHECK(adc_continuous_deinit(handle));

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
esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            printf(" HTTP_EVENT_ERROR ");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            // printf("HTTP_EVENT_ON_CONNECTED ");
            break;
        case HTTP_EVENT_HEADER_SENT:
            // printf("HTTP_EVENT_HEADER_SENT ");
            break;
        case HTTP_EVENT_ON_HEADER:
            // printf("HTTP_EVENT_ON_HEADER ");
            // printf("%.*s\n", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_DATA:
            // printf("HTTP_EVENT_ON_DATA:\n ");
            // printf("evt:%.*s\n", evt->data_len, (char*)evt->data);
            // printf("evt->len:%i  evt->data:%s\n", evt->data_len, (char*)evt->data);

            command_buffer_len=evt->data_len;
            strncpy(command_buffer,evt->data,command_buffer_len);
            // command_buffer[command_buffer_len]='\0';
            // printf("command_buffer_len:%i  command_buffer:%s\n", command_buffer_len, command_buffer);
            // printf("HTTP_EVENT_ON_DATA: done\n ");
            break;
        case HTTP_EVENT_ON_FINISH:
            // printf("HTTP_EVENT_ON_FINISH ");
            break;
        case HTTP_EVENT_DISCONNECTED:
            // printf("HTTP_EVENT_DISCONNECTED ");
            break;
        case HTTP_EVENT_REDIRECT:
            // printf("HTTP_EVENT_REDIRECT ");
            break;
    }
    return ESP_OK;
}
// ************

char* call_juicier(char* cmd,char *key, char *value) {
    char url[256];
    sprintf(url,"https://www.juicier.net/control?devid=%s&cmd=%s&key=%s&value=%s",UID,cmd,key,value);
    ESP_LOGI(TAG,"url: %s",url);

    // ESP_LOGI(TAG,"UID: %s",UID);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err= esp_http_client_perform(client);
    if (err == ESP_OK) {
        // printf("command_buffer_len:%i\n",command_buffer_len);
        // printf("command_buffer:%.*s\n",command_buffer_len,command_buffer);

        // int j=esp_http_client_read(client,buffer,sizeof(buffer)/sizeof(buffer[0]));
        // int remaining=0;

        // printf("return value of esp_http_client_read:%d    remaining:%d\n",j,remaining);
        // for (int i=0;i<sizeof(buffer)/sizeof(buffer[0]);i++) {
        //     printf("buffer[%d]:%x\n",i,buffer[i]);
        // }
        // ESP_LOGI(TAG,"HTTP GET Status = %d, content_length = %lld",
        //     esp_http_client_get_status_code(client),
        //     esp_http_client_get_content_length(client));
        }
     else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);

    for (int i=0;i<sizeof(call_juicier_return_buffer);i++) {call_juicier_return_buffer[i]=0;};
    strncpy(call_juicier_return_buffer,command_buffer,command_buffer_len);
    return call_juicier_return_buffer;
}
// ************
void app_main(void)
{
    start_wifi_and_webserver();

    esp_log_level_set("gpio", ESP_LOG_NONE);

    uint8_t mac[8]; // get a unique ID for this chip
    esp_read_mac(mac,ESP_MAC_ETH);
    sprintf(UID,"%x:%x:%x:%x:%x:%x",mac[5],mac[4],mac[3],mac[2],mac[1],mac[0]);
    ESP_LOGI(TAG,"UID string is:%s\n",UID);

    // initialize relay control GPIO
    #define RELAY_GPIO_PIN 18
    gpio_reset_pin(RELAY_GPIO_PIN);
     // Set the GPIO as a push/pull output
    gpio_set_direction(RELAY_GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_GPIO_PIN, false);
    int s_led_state_previous=false;

    // start loop 
    // const TickType_t xDelay = 500;// / portTICK_PERIOD_MS;
    while (1) {
        int Vrms=get_Vrms();
        // ESP_LOGW(TAG,"\033[1;34m Vrms:%d (%x)  Vrms:%f",Vrms,Vrms,((double)Vrms/0xfff)*4.1);
        // Toggle the LED state 
        if (s_led_state != s_led_state_previous) {
            ESP_LOGI(TAG, "app_main:Turning the LED %s!", s_led_state == true ? "ON" : "OFF");
        };
        char* call_juicier_return_buffer;

        //turn charger on or off
        call_juicier_return_buffer=call_juicier("get","devicestatus","");
        printf("call_juicier_return_buffer:%s\n",call_juicier_return_buffer);
        if (strcmp(command_buffer,"CHARGING") == 0) gpio_set_level(RELAY_GPIO_PIN, true);
        else gpio_set_level(RELAY_GPIO_PIN, false);  

        // gpio_set_level(RELAY_GPIO_PIN, s_led_state); 
        s_led_state_previous=s_led_state;
        
        // send kWh used for the last second (or so)
        char kWh_and_timestamp[32];
        float kWh;

        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        int64_t time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;

        kWh=((Vrms/0xfff)*4.1*240)*grid_voltage*(time_us-last_timestamp)/1000;
        sprintf(kWh_and_timestamp,"kWh:%f_epochtime:%lli",kWh,time_us);   //kwH each millisecond
        call_juicier_return_buffer=call_juicier("set","kWh",kWh_and_timestamp);
        last_timestamp=time_us;

        vTaskDelay(1);
    }

}


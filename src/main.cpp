#include <Arduino.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

#define XSTR(x) #x
#define STR(x) XSTR(x)

uint8_t PINOUT_LED_WHITE = 1;
uint8_t PINOUT_LED_YELLOW = 3;

const char *topic = "bedroom/rl";
WiFiClientSecure espClient;
PubSubClient client(espClient);

#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27

#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static camera_config_t camera_config = {
    .pin_pwdn = CAM_PIN_PWDN,
    .pin_reset = CAM_PIN_RESET,
    .pin_xclk = CAM_PIN_XCLK,
    .pin_sscb_sda = CAM_PIN_SIOD,
    .pin_sscb_scl = CAM_PIN_SIOC,

    .pin_d7 = CAM_PIN_D7,
    .pin_d6 = CAM_PIN_D6,
    .pin_d5 = CAM_PIN_D5,
    .pin_d4 = CAM_PIN_D4,
    .pin_d3 = CAM_PIN_D3,
    .pin_d2 = CAM_PIN_D2,
    .pin_d1 = CAM_PIN_D1,
    .pin_d0 = CAM_PIN_D0,
    .pin_vsync = CAM_PIN_VSYNC,
    .pin_href = CAM_PIN_HREF,
    .pin_pclk = CAM_PIN_PCLK,

    
    .xclk_freq_hz = 20000000, //XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_RGB565, //YUV422,GRAYSCALE,RGB565,JPEG
    .frame_size = FRAMESIZE_QVGA, //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

    .jpeg_quality = 10, //0-63 lower number means higher quality
    .fb_count = 1, //if more than one, i2s runs in continuous mode. Use only with JPEG
};

static esp_err_t init_camera()
{
    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Camera Init Failed");
        return err;
    }

    return ESP_OK;
}

esp_err_t jpg_stream_httpd_handler(httpd_req_t *req){
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len;
    uint8_t * _jpg_buf;
    char * part_buf[64];
    static int64_t last_frame = 0;
    if(!last_frame) {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    while(true){
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        if(fb->format != PIXFORMAT_JPEG){
            bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
            if(!jpeg_converted){
                Serial.println("JPEG compression failed");
                esp_camera_fb_return(fb);
                res = ESP_FAIL;
            }
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);

            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if(fb->format != PIXFORMAT_JPEG){
            free(_jpg_buf);
        }
        esp_camera_fb_return(fb);
        if(res != ESP_OK){
            break;
        }
    }

    last_frame = 0;
    return res;
}

httpd_uri_t uri_get_mjpeg = {    
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = jpg_stream_httpd_handler,
    .user_ctx = NULL
};

httpd_handle_t start_webserver(void)
{
    /* Generate default configuration */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    
    /* Empty handle to esp_http_server */
    httpd_handle_t server = NULL;

    /* Start the httpd server */
    if (httpd_start(&server, &config) == ESP_OK) {
    
        /* Register URI handlers */
        httpd_register_uri_handler(server, &uri_get_mjpeg);
    }
    /* If server failed to start, handle will be NULL */
    return server;
}
void reconnectToBroker()
{
  while (!client.connected())
  {
    Serial.println("Attempting MQTT connection...");

    if (client.connect(STR(CLIENT_ID), STR(BROKER_USERNAME), STR(BROKER_PASSWORD)))
    {
      Serial.print("connected ");
      Serial.println(STR(CLIENT_ID));

      client.subscribe(topic);
    }
    else
    {
      Serial.println("connection failed, try again in 5 seconds");
      delay(5000);
    }
  }
}

char state;

std::function<void(char *, uint8_t *, unsigned int)> onMessageReceived = [](char *topic, byte *payload, unsigned int length)
{
  if ((char)payload[0] == '+')
  {
    // ON
    digitalWrite(PINOUT_LED_WHITE, LOW);
    digitalWrite(PINOUT_LED_YELLOW, HIGH);
    state = 'y';
  }
  else if ((char)payload[0] == '-')
  {
    // OFF
    digitalWrite(PINOUT_LED_WHITE, LOW);
    digitalWrite(PINOUT_LED_YELLOW, LOW);
    state = 'y';
  }
  else if ((char)payload[0] == 'y')
  {
    // YELLOW
    digitalWrite(PINOUT_LED_WHITE, LOW);
    digitalWrite(PINOUT_LED_YELLOW, HIGH);
    state = 'y';
  }
  else if ((char)payload[0] == 'w')
  {
    // WHITE
    digitalWrite(PINOUT_LED_WHITE, HIGH);
    digitalWrite(PINOUT_LED_YELLOW, LOW);
    state = 'w';
  }
};

void setup() {
  Serial.begin(115200);

  pinMode(PINOUT_LED_YELLOW, OUTPUT);
  pinMode(PINOUT_LED_WHITE, OUTPUT);

  init_camera();

  Serial.print("Connecting...");
  WiFi.begin(STR(WIFI_SSID), STR(WIFI_PASS));
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  start_webserver();

  espClient.setInsecure();
  client.setClient(espClient);
  client.setServer(STR(BROKER_HOST), 8883);
  client.setCallback(onMessageReceived);

  digitalWrite(PINOUT_LED_WHITE, HIGH);
  digitalWrite(PINOUT_LED_YELLOW, HIGH);
}

void loop() {
  if (!client.connected())
  {
    reconnectToBroker();
  }
  client.loop();
}
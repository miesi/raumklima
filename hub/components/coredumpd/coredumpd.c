// Core dump download over HTTP. The server runs only while a valid core dump is in flash:
// port 80 is closed on a hub without a dump. GET /coredump?token=<token> returns the raw
// coredump partition data (as esptool read-flash gives it); analyse it with
//   esp-coredump info_corefile -c core.bin -t raw build/raumklima-hub.elf
// The token comes from health (random for each boot) and is in the syslog line
// event=coredump_http, in the console command "coredump" and on the system page.
#include "coredumpd.h"

#include <stdlib.h>
#include <string.h>
#include "app_events.h"
#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "health.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "coredumpd";

#define CHUNK 4096

static httpd_handle_t s_server;

static void peer_name(httpd_req_t *req, char *buf, size_t len)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    buf[0] = '\0';
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&ss, &sl) != 0) {
        strlcpy(buf, "-", len);
    } else if (ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, buf, len);
    } else {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, buf, len);
    }
}

// Same time for every wrong token: no early exit on the first different character.
static bool token_ok(const char *got, const char *want)
{
    size_t n = strlen(want);
    if (strlen(got) != n) {
        return false;
    }
    unsigned d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= (unsigned)(got[i] ^ want[i]);
    }
    return d == 0;
}

static esp_err_t get_coredump(httpd_req_t *req)
{
    char peer[48];
    peer_name(req, peer, sizeof(peer));
    char want[HEALTH_TOKEN_LEN + 1];
    if (!health_coredump_token(want, sizeof(want))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump");
        return ESP_OK;
    }
    char query[64], got[HEALTH_TOKEN_LEN + 8] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "token", got, sizeof(got)) != ESP_OK || !token_ok(got, want)) {
        ESP_LOGW(TAG, "event=denied peer=%s reason=bad_token", peer);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad token");
        return ESP_OK;
    }
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    size_t addr = 0, size = 0;
    if (!part || esp_core_dump_image_get(&addr, &size) != ESP_OK || size > part->size) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "core dump unreadable");
        return ESP_OK;
    }
    char *buf = malloc(CHUNK);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    int64_t t0 = esp_timer_get_time();
    size_t off = 0;
    esp_err_t err = ESP_OK;
    while (off < size && err == ESP_OK) {
        size_t n = size - off < CHUNK ? size - off : CHUNK;
        err = esp_partition_read(part, off, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
        }
        off += n;
    }
    free(buf);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGI(TAG, "event=download peer=%s bytes=%u ms=%lld", peer, (unsigned)size,
                 (esp_timer_get_time() - t0) / 1000);
    } else {
        ESP_LOGW(TAG, "event=download_failed peer=%s bytes=%u err=%s", peer, (unsigned)off, esp_err_to_name(err));
    }
    return ESP_OK;
}

static void on_erased(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "event=stopped reason=coredump_erased port=80");
    }
}

esp_err_t coredumpd_start(void)
{
    char token[HEALTH_TOKEN_LEN + 1];
    if (!health_coredump_token(token, sizeof(token))) {
        return ESP_OK;   // no core dump: port 80 stays closed
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event=start_failed err=%s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uri = {.uri = "/coredump", .method = HTTP_GET, .handler = get_coredump};
    httpd_register_uri_handler(s_server, &uri);
    esp_event_handler_register(APP_EVENT, APP_EVENT_COREDUMP_ERASED, on_erased, NULL);
    ESP_LOGI(TAG, "event=started port=80 path=/coredump");
    return ESP_OK;
}

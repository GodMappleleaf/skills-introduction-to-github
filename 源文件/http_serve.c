/**
 * @file    httpd_post.c
 * @brief   lwIP httpd POST 回调函数实现
 */
#include "http_serve.h"
#include "lwip/apps/httpd.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdio.h>

/* SSI 标签索引，跟在 ssi_tags 数组中的顺序一一对应 */
enum {
    SSI_IP = 0,
    SSI_MASK,
    SSI_GW,
    SSI_MAC,
    SSI_TAG_COUNT
};

static const char *ssi_tags[SSI_TAG_COUNT] = {
    "ip", "mask", "gw", "mac"
};

static const char *cgi_net_cfg(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);

/* 定义 CGI 结构体数组，每个元素 = { URI路径, 处理函数 } */
static const tCGI cgi_handlers[] = {
    { "/api/netcfg", cgi_net_cfg },   /* 下标 0 */
};

#define NUM_CGI_HANDLERS  (sizeof(cgi_handlers) / sizeof(cgi_handlers[0]))


/* ------------------------------------------------------------------ */
/*  自定义数据结构：用于在 POST 处理过程中保存状态                      */
/* ------------------------------------------------------------------ */
#define POST_BUF_MAX  1024
#define POST_MAX_CONNECTIONS  4    /* 同时处理的最大 POST 连接数 */

typedef struct {
    char    uri[64];
    char    data[POST_BUF_MAX];
    uint16_t data_len;
} post_state_t;

/* 每个连接最多同时处理的 POST 数量（与 LWIP_HTTPD_MAX_REQ_LENGTH 对应） */
static post_state_t post_states[POST_MAX_CONNECTIONS];

/* ------------------------------------------------------------------ */
/*  httpd_post_begin                                                   */
/*  POST 请求开始时调用，决定是否接受该 POST                            */
/* ------------------------------------------------------------------ */
err_t httpd_post_begin(void *connection,
                       const char *uri,
                       const char *http_request,
                       u16_t http_request_len,
                       int content_len,
                       char *response_uri,
                       u16_t response_uri_len,
                       u8_t *post_auto_wnd)
{
    LWIP_UNUSED_ARG(http_request);
    LWIP_UNUSED_ARG(http_request_len);

    /* 默认不自动窗口（手动接收数据） */
    if (post_auto_wnd) {
        *post_auto_wnd = 0;
    }

    /*
     * 判断是否是我们要处理的 POST URI
     * 根据实际项目需求修改
     */
    if (strncmp(uri, "/post", 5) == 0 ||
        strncmp(uri, "/config", 7) == 0 ||
        strncmp(uri, "/api", 4) == 0)
    {
        /*
         * 找一个空闲的 slot 存储状态
         * connection 指针可以作为 key，这里简化处理
         */
        int idx = ((uintptr_t)connection) % POST_MAX_CONNECTIONS;

        memset(&post_states[idx], 0, sizeof(post_state_t));
        strncpy(post_states[idx].uri, uri, sizeof(post_states[idx].uri) - 1);
        post_states[idx].data_len = 0;

        /*
         * 设置响应 URI —— POST 完成后 httpd 会返回这个页面
         * 可以返回同一个页面或专门的结果页面
         */
        snprintf(response_uri, response_uri_len, "%s", uri);

        return ERR_OK;
    }

    /* 不认识的 URI，拒绝 POST */
    snprintf(response_uri, response_uri_len, "/404.html");
    return ERR_ARG;
}

/* ------------------------------------------------------------------ */
/*  httpd_post_receive_data                                             */
/*  每收到一段 POST 数据时调用                                          */
/* ------------------------------------------------------------------ */
err_t httpd_post_receive_data(void *connection,
                              struct pbuf *p)
{
    int idx = ((uintptr_t)connection) % POST_MAX_CONNECTIONS;
    post_state_t *ps = &post_states[idx];

    if (ps->data_len == 0 && ps->uri[0] == '\0') {
        /* 未初始化的 slot，丢弃 */
        pbuf_free(p);
        return ERR_ARG;
    }

    /* 将 pbuf 数据拷贝到缓冲区 */
    u16_t to_copy = p->tot_len;
    if (ps->data_len + to_copy > POST_BUF_MAX - 1) {
        to_copy = POST_BUF_MAX - 1 - ps->data_len;
    }

    if (to_copy > 0) {
        pbuf_copy_partial(p,
                          &ps->data[ps->data_len],
                          to_copy, 0);
        ps->data_len += to_copy;
    }

    /* 释放 pbuf */
    pbuf_free(p);

    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  httpd_post_finished                                                 */
/*  POST 数据全部接收完毕后调用                                         */
/* ------------------------------------------------------------------ */
void httpd_post_finished(void *connection,
                         char *response_uri,
                         u16_t response_uri_len)
{
    int idx = ((uintptr_t)connection) % POST_MAX_CONNECTIONS;
    post_state_t *ps = &post_states[idx];

    /* 确保字符串结尾 */
    ps->data[ps->data_len] = '\0';

    /*
     * ========================================
     *  在这里处理接收到的 POST 数据
     * ========================================
     *
     *  ps->uri      : 请求的 URI
     *  ps->data     : POST body（表单 / JSON 等）
     *  ps->data_len : 数据长度
     *
     *  常见处理方式：
     */

    /* ---- 示例 1：解析 application/x-www-form-urlencoded ---- */
    if (strncmp(ps->uri, "/config", 7) == 0) {
        /*
         * 假设收到: name=led&value=on
         * 可以用简单的字符串解析
         */
        char name[32] = {0};
        char value[32] = {0};

        /* 简易解析 key=value&key2=value2 */
        char *token = strtok(ps->data, "&");
        while (token != NULL) {
            char *eq = strchr(token, '=');
            if (eq) {
                *eq = '\0';
                if (strcmp(token, "name") == 0) {
                    strncpy(name, eq + 1, sizeof(name) - 1);
                } else if (strcmp(token, "value") == 0) {
                    strncpy(value, eq + 1, sizeof(value) - 1);
                }
            }
            token = strtok(NULL, "&");
        }

        /* 根据解析结果执行操作 */
        if (strcmp(name, "led") == 0) {
            if (strcmp(value, "on") == 0) {
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
            } else if (strcmp(value, "off") == 0) {
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
            }
        }

        snprintf(response_uri, response_uri_len, "/config.html");
    }
    /* ---- 示例 2：处理 JSON 数据 ---- */
    else if (strncmp(ps->uri, "/api", 4) == 0) {
//        /*
//         * 假设收到: {"cmd":"set_pwm","duty":75}
//         * 这里仅示意，实际可用 cJSON 解析
//         */
//        if (strstr(ps->data, "\"cmd\":\"set_pwm\"")) {
//            char *duty_str = strstr(ps->data, "\"duty\":");
//            /* 待实现 */
//        }

        snprintf(response_uri, response_uri_len, "/api_result.html");
    }
    /* ---- 默认：返回原页面 ---- */
    else {
        snprintf(response_uri, response_uri_len, "/index.html");
    }

    /* 清理 slot */
    memset(ps, 0, sizeof(post_state_t));
}

// 
static const char *cgi_net_cfg(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
/*
     * iIndex  = 你在 tCGI 结构体数组中的下标（0, 1, 2...）
     * POST body 的处理方式：lwIP httpd 会把 POST 数据
     * 以 key=value 形式拆分到 pcParam / pcValue 中
     */
    char ip_str[16] = {0};
    char mask_str[16] = {0};
    char gw_str[16] = {0};

    for (int i = 0; i < iNumParams; i++) {
        if (strcmp(pcParam[i], "ip") == 0) {
            strncpy(ip_str, pcValue[i], sizeof(ip_str) - 1);
        } else if (strcmp(pcParam[i], "mask") == 0) {
            strncpy(mask_str, pcValue[i], sizeof(mask_str) - 1);
        } else if (strcmp(pcParam[i], "gw") == 0) {
            strncpy(gw_str, pcValue[i], sizeof(gw_str) - 1);
        }
    }

    /* 解析并设置网络参数 */
    if (ip_str[0] && mask_str[0] && gw_str[0]) {
        ip4_addr_t ip, mask, gw;
        if (ip4addr_aton(ip_str, &ip) &&
            ip4addr_aton(mask_str, &mask) &&
            ip4addr_aton(gw_str, &gw))
        {
            netif_set_ipaddr(netif_default, &ip);
            netif_set_netmask(netif_default, &mask);
            netif_set_gw(netif_default, &gw);

            /* 可选：写入 Flash 持久化 */
            // flash_save_netcfg(&ip, &mask, &gw);

            return "/api_ok.ssi";
        }
    }

    return "/api_fail.ssi";
}

static u16_t ssi_handler(
#if LWIP_HTTPD_SSI_RAW
                          const char* ssi_tag_name,
#else /* LWIP_HTTPD_SSI_RAW */
                          int iIndex,
#endif /* LWIP_HTTPD_SSI_RAW */
		                      char *pcInsert,
                          int iInsertLen
#if LWIP_HTTPD_SSI_MULTIPART
                          , u16_t current_tag_part, u16_t *next_tag_part
#endif /* LWIP_HTTPD_SSI_MULTIPART */		
#if defined(LWIP_HTTPD_FILE_STATE) && LWIP_HTTPD_FILE_STATE
                          , void *connection_state
#endif /* LWIP_HTTPD_FILE_STATE */		
			                   )
{
    switch (iIndex) {
    case SSI_IP:
        return snprintf(pcInsert, iInsertLen, "%s",
                        ip4addr_ntoa(&netif_default->ip_addr));
    case SSI_MASK:
        return snprintf(pcInsert, iInsertLen, "%s",
                        ip4addr_ntoa(&netif_default->netmask));
    case SSI_GW:
        return snprintf(pcInsert, iInsertLen, "%s",
                        ip4addr_ntoa(&netif_default->gw));
    case SSI_MAC:
        return snprintf(pcInsert, iInsertLen, "%02X:%02X:%02X:%02X:%02X:%02X",
                        netif_default->hwaddr[0], netif_default->hwaddr[1],
                        netif_default->hwaddr[2], netif_default->hwaddr[3],
                        netif_default->hwaddr[4], netif_default->hwaddr[5]);
    default:
        return 0;
    }
}

void http_web_serve_init(void)
{
	// 注册 CGI handlers
	http_set_cgi_handlers(cgi_handlers, NUM_CGI_HANDLERS);
  // 注册 SSI tags
  http_set_ssi_handler(ssi_handler, ssi_tags, SSI_TAG_COUNT);	
}

#include "http_serve.h"
/**
 * @file  httpd_cgi_ssi.c
 * @brief lwIP HTTP server — SSI tag handler, CGI handler, and init entry
 *
 * 配合 httpd.c (lwIP httpd core) 使用。
 * 功能：
 *   1. SSI 标签替换 → IP / Mask / GW / MAC
 *   2. CGI POST      → /api/netcfg.cgi  接收新的网络参数
 *   3. httpd 初始化   → AppHttpdInit()
 */

#include "lwip/apps/httpd.h"
#include "lwip/apps/fs.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>

/* ================================================================== */
/*  0. LWIP_HTTPD_FILE_STATE 需要的用户实现                            */
/*    fs_state_init / fs_state_free 原型来自 lwip/apps/fs.h           */
/* ================================================================== */

typedef struct {
    int placeholder;
} fs_state_t;

/**
 * fs_state_init — 文件打开时 lwIP 调用。
 * 原型: void *fs_state_init(struct fs_file *file, const char *name)
 */
void *fs_state_init(struct fs_file *file, const char *name)
{
    LWIP_UNUSED_ARG(file);
    LWIP_UNUSED_ARG(name);

    fs_state_t *s = (fs_state_t *)mem_malloc(sizeof(fs_state_t));
    if (s != NULL) {
        s->placeholder = 0;
    }
    return s;
}

/**
 * fs_state_free — 文件关闭时 lwIP 调用。
 * 原型: void fs_state_free(struct fs_file *file, void *state)
 */
void fs_state_free(struct fs_file *file, void *state)
{
    LWIP_UNUSED_ARG(file);
    if (state != NULL) {
        mem_free(state);
    }
}

/* ================================================================== */
/*  1. httpd_cgi_handler — CGI_SSI 模式下的通用 CGI 回调               */
/*    原型来自 lwip/apps/httpd.h:                                      */
/*    void httpd_cgi_handler(struct fs_file *, const char *, int,      */
/*                           char **, char **, void *)                 */
/* ================================================================== */

void httpd_cgi_handler(struct fs_file *file, const char *uri, int num_params,
                       char **params, char **values, void *connection)
{
    LWIP_UNUSED_ARG(file);
    LWIP_UNUSED_ARG(connection);

    if (strncmp(uri, "/api/netcfg.cgi", 15) == 0) {
        for (int i = 0; i < num_params; i++) {
            if (strcmp(params[i], "ip") == 0 ||
                strcmp(params[i], "mask") == 0 ||
                strcmp(params[i], "gw") == 0) {
                LWIP_DEBUGF(HTTPD_DEBUG,
                    ("CGI param: %s = %s\n", params[i], values[i]));
            }
        }
    }
}

/* ================================================================== */
/*  2. SSI 标签处理                                                    */
/*    tSSIHandler 原型 (根据你的 lwIP 版本):                           */
/*    u16_t (*)(int iIndex, char *pcInsert, int iInsertLen, void *)    */
/* ================================================================== */

static const char *ssi_tags[] = {
    "ip",       /* <!--#ip-->   */
    "mask",     /* <!--#mask--> */
    "gw",       /* <!--#gw-->   */
    "mac"       /* <!--#mac-->  */
};

#define NUM_SSI_TAGS  (sizeof(ssi_tags) / sizeof(ssi_tags[0]))

static u16_t
ssi_handler(
#if LWIP_HTTPD_SSI_RAW
                             const char* ssi_tag_name,
#else /* LWIP_HTTPD_SSI_RAW */
                             int iIndex,
#endif /* LWIP_HTTPD_SSI_RAW */
                             char *pcInsert, int iInsertLen
#if LWIP_HTTPD_SSI_MULTIPART
                             , u16_t current_tag_part, u16_t *next_tag_part
#endif /* LWIP_HTTPD_SSI_MULTIPART */
#if defined(LWIP_HTTPD_FILE_STATE) && LWIP_HTTPD_FILE_STATE
                             , void *connection_state
#endif /* LWIP_HTTPD_FILE_STATE */
                             )
{

    struct netif *ni = netif_default;
    if (ni == NULL) {
        strncpy(pcInsert, "N/A", iInsertLen);
        return 3;
    }

    switch (iIndex) {
    case 0: /* ip */
        return (u16_t)snprintf(pcInsert, iInsertLen, "%s",
                               ip4addr_ntoa(netif_ip4_addr(ni)));
    case 1: /* mask */
        return (u16_t)snprintf(pcInsert, iInsertLen, "%s",
                               ip4addr_ntoa(netif_ip4_netmask(ni)));
    case 2: /* gw */
        return (u16_t)snprintf(pcInsert, iInsertLen, "%s",
                               ip4addr_ntoa(netif_ip4_gw(ni)));
    case 3: { /* mac */
        const uint8_t *m = ni->hwaddr;
        return (u16_t)snprintf(pcInsert, iInsertLen,
                               "%02X:%02X:%02X:%02X:%02X:%02X",
                               m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    default:
        strncpy(pcInsert, "??", iInsertLen);
        return 2;
    }
}

/* ================================================================== */
/*  3. CGI GET 处理 —— tCGIHandler:                                    */
/*     const char *(*)(int iIndex, int iNumParams,                    */
/*                     char *pcParam[], char *pcValue[])              */
/* ================================================================== */

static const char *
netcfg_cgi_handler(int iIndex, int iNumParams,
                   char *pcParam[], char *pcValue[])
{
    LWIP_UNUSED_ARG(iIndex);

    ip4_addr_t new_ip, new_mask, new_gw;
    int got_ip = 0, got_mask = 0, got_gw = 0;

    for (int i = 0; i < iNumParams; i++) {
        if (strcmp(pcParam[i], "ip") == 0) {
            if (ip4addr_aton(pcValue[i], &new_ip)) got_ip = 1;
        } else if (strcmp(pcParam[i], "mask") == 0) {
            if (ip4addr_aton(pcValue[i], &new_mask)) got_mask = 1;
        } else if (strcmp(pcParam[i], "gw") == 0) {
            if (ip4addr_aton(pcValue[i], &new_gw)) got_gw = 1;
        }
    }

    if (got_ip && got_mask && got_gw) {
        netif_set_addr(netif_default, &new_ip, &new_mask, &new_gw);
    }

    return "/index.ssi";
}

static const tCGI cgi_handlers[] = {
    {
        "/api/netcfg.cgi",
        netcfg_cgi_handler
    }
};

#define NUM_CGI_HANDLERS  (sizeof(cgi_handlers) / sizeof(cgi_handlers[0]))

/* ================================================================== */
/*  4. POST 处理 —— lwIP httpd_post_* 回调                             */
/*    原型来自 lwip/apps/httpd.h:                                      */
/*    err_t httpd_post_begin(void *, const char *, const char *,       */
/*              u16_t, int, char *, u16_t, u8_t *)                    */
/* ================================================================== */

#define POST_BUF_MAX  256
static char post_buf[POST_BUF_MAX];
static int  post_len;

/**
 * httpd_post_begin — 注意最后一个参数是 u8_t* 不是 char*
 */
err_t
httpd_post_begin(void *connection, const char *uri, const char *http_request,
                 u16_t http_request_len, int content_len,
                 char *response_uri, u16_t response_uri_len,
                 u8_t *post_response_file)
{
    LWIP_UNUSED_ARG(connection);
    LWIP_UNUSED_ARG(http_request);
    LWIP_UNUSED_ARG(http_request_len);
    LWIP_UNUSED_ARG(content_len);
    LWIP_UNUSED_ARG(post_response_file);

    if (strncmp(uri, "/api/netcfg.cgi", 15) != 0) {
        return ERR_ARG;
    }

    post_len = 0;
    response_uri[0] = '\0';
    return ERR_OK;
}

err_t
httpd_post_receive_data(void *connection, struct pbuf *p)
{
    LWIP_UNUSED_ARG(connection);

    int copy_len = LWIP_MIN(p->tot_len, (u16_t)(POST_BUF_MAX - 1 - post_len));
    if (copy_len > 0) {
        pbuf_copy_partial(p, post_buf + post_len, copy_len, 0);
        post_len += copy_len;
    }
    pbuf_free(p);
    return ERR_OK;
}

void
httpd_post_finished(void *connection, char *response_uri,
                    u16_t response_uri_len)
{
    LWIP_UNUSED_ARG(connection);

    post_buf[post_len] = '\0';

    char ip_str[16]   = {0};
    char mask_str[16]  = {0};
    char gw_str[16]   = {0};

    char *p = post_buf;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = '\0';

        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            if      (strcmp(p, "ip")   == 0) strncpy(ip_str,   eq + 1, sizeof(ip_str)   - 1);
            else if (strcmp(p, "mask") == 0) strncpy(mask_str,  eq + 1, sizeof(mask_str)  - 1);
            else if (strcmp(p, "gw")   == 0) strncpy(gw_str,   eq + 1, sizeof(gw_str)   - 1);
        }

        p = amp ? amp + 1 : NULL;
    }

    ip4_addr_t new_ip, new_mask, new_gw;
    if (ip_str[0] && mask_str[0] && gw_str[0]) {
        ip4addr_aton(ip_str,   &new_ip);
        ip4addr_aton(mask_str, &new_mask);
        ip4addr_aton(gw_str,   &new_gw);
        netif_set_addr(netif_default, &new_ip, &new_mask, &new_gw);
    }
    snprintf(response_uri, response_uri_len, "/index.ssi");
//    snprintf(response_uri, response_uri_len, "/api/netcfg.cgi?");
//		snprintf(response_uri + strlen(response_uri), response_uri_len, (const char *)&post_buf[0]);
}

/* ================================================================== */
/*  5. 初始化入口                                                      */
/* ================================================================== */

void http_web_serve_init(void)
{
    httpd_init();
    http_set_ssi_handler(ssi_handler, ssi_tags, NUM_SSI_TAGS);
    http_set_cgi_handlers(cgi_handlers, NUM_CGI_HANDLERS);
}

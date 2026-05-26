/**
 * @file    httpd_post.c
 * @brief   lwIP httpd POST �ص�����ʵ��
 */
#include "http_serve.h"
#include "lwip/apps/httpd.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdio.h>

/* SSI ��ǩ���������� ssi_tags �����е�˳��һһ��Ӧ */
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

/* ���� CGI �ṹ�����飬ÿ��Ԫ�� = { URI·��, �������� } */
static const tCGI cgi_handlers[] = {
    { "/api/netcfg.cgi", cgi_net_cfg },
};

#define NUM_CGI_HANDLERS  (sizeof(cgi_handlers) / sizeof(cgi_handlers[0]))


/* ------------------------------------------------------------------ */
/*  �Զ������ݽṹ�������� POST ���������б���״̬                      */
/* ------------------------------------------------------------------ */
#define POST_BUF_MAX  1024
#define POST_MAX_CONNECTIONS  4    /* ͬʱ��������� POST ������ */

typedef struct {
    void   *connection;     /* 用于匹配同一个 POST 会话 */
    char    uri[64];
    char    data[POST_BUF_MAX];
    uint16_t data_len;
} post_state_t;

/* ÿ���������ͬʱ������ POST �������� LWIP_HTTPD_MAX_REQ_LENGTH ��Ӧ�� */
static post_state_t post_states[POST_MAX_CONNECTIONS];

/* ------------------------------------------------------------------ */
/*  httpd_post_begin                                                   */
/*  POST ����ʼʱ���ã������Ƿ���ܸ� POST                            */
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

    /* Ĭ�ϲ��Զ����ڣ��ֶ��������ݣ� */
    if (post_auto_wnd) {
        *post_auto_wnd = 0;
    }

    /*
     * �ж��Ƿ�������Ҫ������ POST URI
     * ����ʵ����Ŀ�����޸�
     */
    if (strncmp(uri, "/post", 5) == 0 ||
        strncmp(uri, "/config", 7) == 0 ||
        strncmp(uri, "/api", 4) == 0)
    {
        /*
         * ��һ�����е� slot �洢״̬
         * connection ָ�������Ϊ key������򻯴���
         */
        int idx = -1;
        for (int i = 0; i < POST_MAX_CONNECTIONS; i++) {
            if (post_states[i].uri[0] == '\0') { idx = i; break; }
        }
        if (idx < 0) {
            snprintf(response_uri, response_uri_len, "/index.ssi");
            return ERR_MEM;
        }

        memset(&post_states[idx], 0, sizeof(post_state_t));
        post_states[idx].connection = connection;
        strncpy(post_states[idx].uri, uri, sizeof(post_states[idx].uri) - 1);
        post_states[idx].data_len = 0;

        /*
         * ������Ӧ URI ���� POST ��ɺ� httpd �᷵�����ҳ��
         * ���Է���ͬһ��ҳ���ר�ŵĽ��ҳ��
         */
        snprintf(response_uri, response_uri_len, "%s", uri);

        return ERR_OK;
    }

    /* ����ʶ�� URI���ܾ� POST */
    snprintf(response_uri, response_uri_len, "/404.html");
    return ERR_ARG;
}

/* ------------------------------------------------------------------ */
/*  httpd_post_receive_data                                             */
/*  ÿ�յ�һ�� POST ����ʱ����                                          */
/* ------------------------------------------------------------------ */
err_t httpd_post_receive_data(void *connection,
                              struct pbuf *p)
{
    /* 与 httpd_post_begin 保持一致：按 connection 指针匹配 slot */
    int idx = -1;
    for (int i = 0; i < POST_MAX_CONNECTIONS; i++) {
        if (post_states[i].connection == connection && post_states[i].uri[0] != '\0') {
            idx = i; break;
        }
    }
    if (idx < 0) {
        pbuf_free(p);
        return ERR_ARG;
    }
    post_state_t *ps = &post_states[idx];

    if (ps->data_len == 0 && ps->uri[0] == '\0') {
        /* δ��ʼ���� slot������ */
        pbuf_free(p);
        return ERR_ARG;
    }

    /* �� pbuf ���ݿ����������� */
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

    /* �ͷ� pbuf */
    pbuf_free(p);

    return ERR_OK;
}

/* ------------------------------------------------------------------ */
/*  httpd_post_finished                                                 */
/*  POST ����ȫ��������Ϻ����                                         */
/* ------------------------------------------------------------------ */
void httpd_post_finished(void *connection,
                         char *response_uri,
                         u16_t response_uri_len)
{
    int idx = -1;
    for (int i = 0; i < POST_MAX_CONNECTIONS; i++) {
        if (post_states[i].connection == connection && post_states[i].uri[0] != '\0') {
            idx = i; break;
        }
    }
    if (idx < 0) {
        snprintf(response_uri, response_uri_len, "/index.ssi");
        return;
    }
    post_state_t *ps = &post_states[idx];

    /* ȷ���ַ�����β */
    ps->data[ps->data_len] = '\0';

    /*
     * ========================================
     *  �����ﴦ�����յ��� POST ����
     * ========================================
     *
     *  ps->uri      : ����� URI
     *  ps->data     : POST body������ / JSON �ȣ�
     *  ps->data_len : ���ݳ���
     *
     *  ����������ʽ��
     */

    /* ---- ʾ�� 1������ application/x-www-form-urlencoded ---- */
    if (strncmp(ps->uri, "/config", 7) == 0) {
        /*
         * �����յ�: name=led&value=on
         * �����ü򵥵��ַ�������
         */
        char name[32] = {0};
        char value[32] = {0};

        /* ���׽��� key=value&key2=value2 */
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

        /* ���ݽ������ִ�в��� */
        if (strcmp(name, "led") == 0) {
            if (strcmp(value, "on") == 0) {
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
            } else if (strcmp(value, "off") == 0) {
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
            }
        }

        snprintf(response_uri, response_uri_len, "/config.html");
    }
    /* ---- Ĭ�ϣ�����ԭҳ�� ---- */
    else {
        snprintf(response_uri, response_uri_len, "/index.ssi");
    }

    /* ���� slot */
    memset(ps, 0, sizeof(post_state_t));
}

// 
static const char *cgi_net_cfg(int iIndex, int iNumParams, char *pcParam[], char *pcValue[])
{
/*
     * iIndex  = ���� tCGI �ṹ�������е��±꣨0, 1, 2...��
     * POST body �Ĵ�����ʽ��lwIP httpd ��� POST ����
     * �� key=value ��ʽ��ֵ� pcParam / pcValue ��
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

    /* ����������������� */
    if (ip_str[0] && mask_str[0] && gw_str[0]) {
        ip4_addr_t ip, mask, gw;
        if (ip4addr_aton(ip_str, &ip) &&
            ip4addr_aton(mask_str, &mask) &&
            ip4addr_aton(gw_str, &gw))
        {
            netif_set_ipaddr(netif_default, &ip);
            netif_set_netmask(netif_default, &mask);
            netif_set_gw(netif_default, &gw);

            /* ��ѡ��д�� Flash �־û� */
            // flash_save_netcfg(&ip, &mask, &gw);

            return "/index.ssi";
        }
    }

    return "/index.ssi";
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
	// ע�� CGI handlers
	http_set_cgi_handlers(cgi_handlers, NUM_CGI_HANDLERS);
  // ע�� SSI tags
  http_set_ssi_handler(ssi_handler, ssi_tags, SSI_TAG_COUNT);	
}

/*
 * Description: 
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdio.h>
# include <stdlib.h>
# include <unistd.h>
# include <sys/time.h>

# include "nw_clt.h"

static int create_socket(int family, int sock_type)
{
    int sockfd = socket(family, sock_type, 0);
    if (sockfd < 0) {
        return -1;
    }
    if (nw_sock_set_nonblock(sockfd) < 0) {
        close(sockfd);
        return -1;
    }
    if (sock_type == SOCK_STREAM && (family == AF_INET || family == AF_INET6)) {
        if (nw_sock_set_no_delay(sockfd) < 0) {
            close(sockfd);
            return -1;
        }
    }

    return sockfd;
}

static int set_socket_option(nw_clt *clt, int sockfd)
{
    if (clt->read_mem > 0) {
        if (nw_sock_set_recv_buf(sockfd, clt->read_mem) < 0) {
            close(sockfd);
            return -1;
        }
    }
    if (clt->write_mem > 0) {
        if (nw_sock_set_send_buf(sockfd, clt->write_mem) < 0) {
            close(sockfd);
            return -1;
        }
    }

    return 0;
}

static void generate_random_path(char *path, size_t size, char *prefix, char *suffix)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    srand(tv.tv_sec * tv.tv_usec);
    char randname[11];
    for (int i = 0; i < 10; ++i) {
        randname[i] = 'a' + rand() % 26;
    }
    randname[10] = '\0';
    snprintf(path, size, "%s/%s%s%s", P_tmpdir, prefix, randname, suffix);
}

static void on_timeout(nw_timer *timer, void *privdata)
{
    nw_clt *clt = (nw_clt *)privdata;
    nw_clt_start(clt);
}

static void reconnect_later(nw_clt *clt)
{
    nw_timer_set(&clt->timer, clt->reconnect_timeout, false, on_timeout, clt);
    nw_timer_start(&clt->timer);
}

static void on_recv_fd(nw_ses *ses, int fd)
{
    close(fd);
}

static void on_connect(nw_ses *ses, bool result)
{
    nw_clt *clt = (nw_clt *)ses;
    if (clt->type.on_connect) {
        clt->type.on_connect(ses, result);
    }
    if (result) {
        clt->ses.connected = true;
        set_socket_option(clt, clt->ses.sockfd);
        nw_sock_host_addr(ses->sockfd, ses->host_addr);
    } else {
        nw_clt_close(clt);
        reconnect_later(clt);
    }
}

static void on_error(nw_ses *ses, const char *msg)
{
    nw_clt *clt = (nw_clt *)ses;
    if (clt->type.on_error_msg) {
        clt->type.on_error_msg(ses, msg);
    }
    nw_clt_close(clt);
    reconnect_later(clt);
}

static void on_close(nw_ses *ses)
{
    nw_clt *clt = (nw_clt *)ses;
    nw_clt_close(clt);
    reconnect_later(clt);
}

nw_clt *nw_clt_create(nw_clt_cfg *cfg, nw_clt_type *type, void *privdata)
{
    nw_loop_init();

    if (type->on_recv_pkg == NULL)
        return NULL;
    if (cfg->sock_type == SOCK_STREAM && type->decode_pkg == NULL)
        return NULL;

    nw_clt *clt = malloc(sizeof(nw_clt));
    memset(clt, 0, sizeof(nw_clt));
    clt->type = *type;
    clt->reconnect_timeout = cfg->reconnect_timeout  == 0 ? 1.0 : cfg->reconnect_timeout;
    clt->buf_pool = nw_buf_pool_create(cfg->max_pkg_size);
    clt->read_mem = cfg->read_mem;
    clt->write_mem = cfg->write_mem;
    if (clt->buf_pool == NULL) {
        nw_clt_release(clt);
        return NULL;
    }

    nw_addr_t *host_addr = malloc(sizeof(nw_addr_t));
    if (host_addr == NULL) {
        nw_clt_release(clt);
        return NULL;
    }
    memset(host_addr, 0, sizeof(nw_addr_t));
    host_addr->family = cfg->addr.family;
    host_addr->addrlen = cfg->addr.addrlen;
    if (nw_ses_init(&clt->ses, nw_default_loop, -1, cfg->sock_type, NW_SES_TYPE_CLIENT, host_addr, clt->buf_pool, privdata) < 0) {
        nw_clt_release(clt);
        return NULL;
    }
    memcpy(&clt->ses.peer_addr, &cfg->addr, sizeof(nw_addr_t));
    clt->ses.decode_pkg  = type->decode_pkg;
    clt->ses.on_recv_pkg = type->on_recv_pkg;
    clt->ses.on_recv_fd  = type->on_recv_fd == NULL ? on_recv_fd : type->on_recv_fd;
    clt->ses.on_connect  = on_connect;
    clt->ses.on_error    = on_error;
    clt->ses.on_close    = on_close;

    return clt;
}

int nw_clt_start(nw_clt *clt)
{
    int sockfd = create_socket(clt->ses.peer_addr.family, clt->ses.sock_type);
    if (sockfd < 0) {
        return -1;
    }
    clt->ses.sockfd = sockfd;
    if (clt->ses.peer_addr.family == AF_UNIX && clt->ses.sock_type == SOCK_DGRAM) {
        clt->ses.host_addr->un.sun_family = AF_UNIX;
        generate_random_path(clt->ses.host_addr->un.sun_path, sizeof(clt->ses.host_addr->un.sun_path), "dgram", ".sock");
        if (nw_ses_bind(&clt->ses, clt->ses.host_addr) < 0) {
            return -1;
        }
    }

    if (clt->ses.sock_type == SOCK_STREAM || clt->ses.sock_type == SOCK_SEQPACKET) {
        clt->ses.connected = false;
        return nw_ses_connect(&clt->ses, &clt->ses.peer_addr);
    } else {
        clt->ses.connected = true;
        set_socket_option(clt, clt->ses.sockfd);
        nw_sock_host_addr(clt->ses.sockfd, clt->ses.host_addr);
        return nw_ses_start(&clt->ses);
    }
}

int nw_clt_close(nw_clt *clt)
{
    if (clt->type.on_close) {
        clt->type.on_close(&clt->ses);
    }
    if (nw_timer_active(&clt->timer)) {
        nw_timer_stop(&clt->timer);
    }
    clt->ses.connected = false;
    return nw_ses_close(&clt->ses);
}

int nw_clt_release(nw_clt *clt)
{
    if (clt->ses.write_buf) {
        nw_ses_release(&clt->ses);
    }
    if (clt->buf_pool) {
        nw_buf_pool_release(clt->buf_pool);
    }
    free(clt);
    return 0;
}

int nw_clt_connected(nw_clt *clt)
{
    return clt->ses.connected;
}

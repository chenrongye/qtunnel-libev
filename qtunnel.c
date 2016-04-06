#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <openssl/rc4.h>
#include <openssl/md5.h>
#include <openssl/evp.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include "qtunnel.h"
#include <ev.h>
#include <netinet/tcp.h>


struct struct_options options;
struct struct_setting setting;

int serv_sock;
struct sockaddr_in serv_adr;

const int BUFSIZE = 4096;


int main(int argc, char *argv[]){
    struct ev_loop *loop = ev_default_loop(0);
    int i;

    struct ev_io socket_accept;

    get_param(argc, argv);
//    options.faddr = "0.0.0.0:8765";
//    options.baddr = "";
//    options.cryptoMethod = "RC4";
//    options.secret = "secret";
//    options.clientMod = 1;

    build_server();

    ev_io_init(&socket_accept, accept_cb, serv_sock, EV_READ);
    ev_io_start(loop, &socket_accept);

    while(1) {
        ev_loop(loop, 0);
    }

    return 0;
}

void free_conn(struct conn *conn) {
    if(conn != NULL) {
        if(conn->another != NULL) {
            conn->another->another = NULL;
        }
        free(conn->buf);
        free(conn->recv_ctx);
        free(conn->send_ctx);
        free(conn);
    }
}

void close_and_free(EV_P_ struct conn *conn) {
//    puts("close");
    if(conn != NULL) {
        ev_io_stop(EV_A_ &conn->send_ctx->io);
        ev_io_stop(EV_A_ &conn->recv_ctx->io);
        printf("close fd %d\n", conn->fd);
        close(conn->fd);
        free_conn(conn);
    }
}

void send_cb(EV_P_ ev_io  *watcher, int revents) {

    if(EV_ERROR & revents)
    {
        printf("error event in read");
        return;
    }

    struct conn_ctx *conn_ctx = (struct conn_ctx *)watcher;
    struct conn *conn = conn_ctx->conn;
    struct conn *another = conn->another;
    char **buf = &conn->buf;

    if(another == NULL) {
        close_and_free(EV_A_ conn);
        return ;
    }

    if(conn->buf_len == 0) {
//        puts("send len == 0");
        close_and_free(EV_A_ conn->another);
        close_and_free(EV_A_ conn);
        return ;
    } else {
        ssize_t s = send(conn->fd, *buf + conn->buf_idx, conn->buf_len, 0);
        if(s < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK) {
                close_and_free(EV_A_ conn->another);
                close_and_free(EV_A_ conn);
            }
            return ;
        } else if( s < conn->buf_len ) {
            conn->buf_len -= s;
            conn->buf_idx += s;
        } else {
            puts("send all");
            conn->buf_len = 0;
            conn->buf_idx = 0;
            ev_io_stop(EV_A_ &conn->send_ctx->io);
            if(another != NULL) {
                ev_io_start(EV_A_ &another->recv_ctx->io);
            } else {
                close_and_free(EV_A_ another);
                close_and_free(EV_A_ conn);
            }
        }
    }

}


void recv_cb(EV_P_ ev_io *watcher, int revents) {

    if(EV_ERROR & revents)
    {
        printf("error event in read");
        return;
    }
    struct conn_ctx *conn_ctx = (struct conn_ctx *)watcher;
    struct conn *conn = conn_ctx->conn;
    struct conn *another = conn->another;
    char **buf = &another->buf;

    ssize_t r = recv(conn->fd, *buf, BUFSIZE, 0);
    printf("fd %d --------> recv %d\n", conn->fd, r);
    if(r == 0) {
        //printf("errno == %d\n", errno);errno
        close_and_free(EV_A_ conn->another);
        close_and_free(EV_A_ conn);
        return ;
    } else if (r < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return ;

        } else {
            close_and_free(EV_A_ conn->another);
            close_and_free(EV_A_ conn);
            return ;
        }
    }

    RC4(&conn->key, r, *buf, *buf);

    int s = send(another->fd, *buf, r, 0);
    printf("send to fd %d --------> %d\n", another->fd, s);
    if(s == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            another->buf_len = r;
            another->buf_idx = 0;
            ev_io_stop(EV_A_ &conn->recv_ctx->io);
            ev_io_start(EV_A_ &another->send_ctx->io);
        } else {
            close_and_free(EV_A_ conn->another);
            close_and_free(EV_A_ conn);

        }
        return ;
    } else if(s < r) {
        another->buf_len = r-s;
        another->buf_idx = s;
        ev_io_stop(EV_A_ &conn->recv_ctx->io);
        ev_io_start(EV_A_ &another->send_ctx->io);
    }
    return ;
}

int setnonblocking(int fd) {
    int flags;
    if (-1 ==(flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void accept_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    int nfd, i, remote_sock, j, o, flags;
    int clnt_adr_size;
    struct sockaddr_in addr, remote_adr;
    nfd = accept(serv_sock, NULL, NULL);

    if(nfd == -1) return ;

    setnonblocking(nfd);

    int opt = 1;
    // disable nagle
    setsockopt(nfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(nfd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif


    memset(&remote_adr, 0, sizeof(remote_adr));
    remote_adr.sin_family = AF_INET;
    remote_adr.sin_port = htons(atoi(setting.baddr_port));
    remote_adr.sin_addr.s_addr = inet_addr(setting.baddr_host);

    remote_sock = socket(PF_INET, SOCK_STREAM, 0);
    printf("new server: local = %d | remote = %d\n", nfd, remote_sock);


    if(remote_sock < 0) {
        perror("socket error");
        close(nfd);
        return ;
    }



    if ( connect(remote_sock, (struct sockaddr *) &remote_adr, sizeof(remote_adr)) < 0) {
        perror("connect remote error");
        close(nfd);
        return ;
        //exit(1);
    }


    int opt2 = 1;
    setsockopt(remote_sock, IPPROTO_TCP, TCP_NODELAY, &opt2, sizeof(opt2));
#ifdef SO_NOSIGPIPE
    setsockopt(remote_sock, SOL_SOCKET, SO_NOSIGPIPE, &opt2, sizeof(opt2));
#endif

    // setup remote socks
    setnonblocking(remote_sock);

    struct conn *local, *remote;
    local = malloc(sizeof(struct conn));
    local->buf = malloc(sizeof(char) * BUFSIZE);
    local->recv_ctx = malloc(sizeof(struct conn_ctx));
    local->send_ctx = malloc(sizeof(struct conn_ctx));
    local->fd = nfd;
    local->buf_idx = 0;
    local->buf_len = 0;
    local->send_ctx->conn = local;
    local->recv_ctx->conn = local;


    remote = malloc(sizeof(struct conn));
    remote = malloc(sizeof(struct conn));
    remote->buf = malloc(sizeof(char) * BUFSIZE);
    remote->recv_ctx = malloc(sizeof(struct conn_ctx));
    remote->send_ctx = malloc(sizeof(struct conn_ctx));
    remote->fd = remote_sock;
    remote->buf_idx = 0;
    remote->buf_len = 0;
    remote->send_ctx->conn = remote;
    remote->recv_ctx->conn = remote;



    RC4_set_key(&local->key, 16, setting.secret);
    RC4_set_key(&remote->key, 16, setting.secret);

    ev_io_init(&local->recv_ctx->io, recv_cb, nfd, EV_READ);
    ev_io_init(&local->send_ctx->io, send_cb, nfd, EV_WRITE);

    ev_io_init(&remote->recv_ctx->io, recv_cb, remote_sock, EV_READ);
    ev_io_init(&remote->send_ctx->io, send_cb, remote_sock, EV_WRITE);

    remote->another = local;
    local->another = remote;
//    puts("start ev");
    ev_io_start(loop, &local->recv_ctx->io);
    ev_io_start(loop, &remote->recv_ctx->io);
}

void get_param(int argc, char *argv[]) {
    char c;
    unsigned long p;
    while((c = getopt_long (argc, argv, short_opts, long_opts, NULL)) != -1) {
        switch(c) {
            case 'h': {
                print_usage();
                exit(0);
            }
            case 'b': {
                options.baddr = optarg;
                p = strchr(optarg, ':') - optarg;
                strncpy(setting.baddr_host, optarg, p);
                strcpy(setting.baddr_port, optarg + p + 1);
                //printf("badd = %s  %s\n", setting.baddr_port, setting.baddr_host);
                break;
            }
            case 'l': {
                options.faddr = optarg;
                p = strchr(optarg, ':') - optarg;
                strcpy(setting.faddr_port, optarg + p + 1);
                //printf("fadd = %s\n", setting.faddr_port);
                break;
            }
            case 'c': {
                options.clientMod = optarg;
                if(strcmp(optarg, "true") == 0) {
                    setting.clientMod = CLIENTMOD;
                } else {
                    setting.clientMod = SERVERMOD;
                }
                break;
            }
            case 's': {
                options.secret = optarg;
                strncpy(setting.secret, secretToKey(optarg, 16), 16);

                break;
            }
            default: {
                printf("unknow option of %c\n", optopt);
                break;
            }
        }
    }
    if(strcmp(setting.baddr_port, "") == 0) {
        perror("missing option --backend");
        exit(1);
    }
    if(strcmp(setting.baddr_host, "") == 0) {
        perror("missing option --backend");
        exit(1);
    }
    if(strcmp(setting.secret, "") == 0) {
        perror("mission option --secret");
        exit(1);
    }

    printf("%s %s %s\n",setting.faddr_port, setting.baddr_port, setting.baddr_host);
}

void print_usage() {
    printf("Options:\n\
  --help\n\
  --backend=remotehost:remoteport    remote\n\
  --listen=localhost:localport   local\n\
  --clientmod=true or false  buffer size\n\
  --secret=secret secret\
\n");
}

byte* secretToKey(char* sec, int size) {

    byte *buf = malloc(sizeof(char) * 16);
    byte *buf2 = malloc(sizeof(char) * 16);
    MD5_CTX h;
    MD5_Init(&h);
    int count = size / 16;
    int i,j;
    for(i = 0; i < count; ++i) {
        MD5_Update(&h, sec, strlen(sec));
        MD5_Final(buf2, &h);
        strncpy(buf, buf2, 15);
    }
    buf[15]=0;


    return buf;
}

int build_server() {
    memset(&serv_adr, 0, sizeof(serv_adr));

    serv_adr.sin_port = htons(atoi(setting.faddr_port));
    serv_adr.sin_family = AF_INET;
    serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);


    if(serv_sock < 0) {
        perror("socket error");
        exit(1);
    }

    int opt = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(serv_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
#ifdef SO_NOSIGPIPE
    setsockopt(serv_sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif


    if ( bind(serv_sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr)) == -1) {
        perror("bind error");
        exit(1);
    }

    if( listen(serv_sock, 128) == -1 ) {
        perror("listen error");
        exit(1);
    }

    setnonblocking(serv_sock);
}

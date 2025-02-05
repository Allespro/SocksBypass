/*
   MicroSocks - multithreaded, small, efficient SOCKS5 server.

   Copyright (C) 2017 rofl0r.

   This is the successor of "rocksocks5", and it was written with
   different goals in mind:

   - prefer usage of standard libc functions over homegrown ones
   - no artificial limits
   - do not aim for minimal binary size, but for minimal source code size,
     and maximal readability, reusability, and extensibility.

   as a result of that, ipv4, dns, and ipv6 is supported out of the box
   and can use the same code, while rocksocks5 has several compile time
   defines to bring down the size of the resulting binary to extreme values
   like 10 KB static linked when only ipv4 support is enabled.

   still, if optimized for size, *this* program when static linked against musl
   libc is not even 50 KB. that's easily usable even on the cheapest routers.

*/

#define _GNU_SOURCE
#include <unistd.h>
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include <limits.h>

#include "sblist.h"
#include "server.h"
#include "sockssrv.h"

extern void custom_log(const char *format, ...);
extern void update_traffic_stats_ui(uint64_t upload, uint64_t download);

/* timeout in microseconds on resource exhaustion to prevent excessive
   cpu usage. */
#ifndef FAILURE_TIMEOUT
#define FAILURE_TIMEOUT 64
#endif

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#ifdef PTHREAD_STACK_MIN
#define THREAD_STACK_SIZE MAX(8*1024, PTHREAD_STACK_MIN)
#else
#define THREAD_STACK_SIZE 64*1024
#endif

#if defined(__APPLE__)
#undef THREAD_STACK_SIZE
#define THREAD_STACK_SIZE 64*1024
#elif defined(__GLIBC__) || defined(__FreeBSD__) || defined(__sun__)
#undef THREAD_STACK_SIZE
#define THREAD_STACK_SIZE 32*1024
#endif

static int quiet;
static const char* auth_user;
static const char* auth_pass;
static sblist* auth_ips;
static pthread_rwlock_t auth_ips_lock = PTHREAD_RWLOCK_INITIALIZER;
static const struct server* server;

struct thread {
    pthread_t pt;
    struct client client;
    enum socksstate state;
    volatile int  done;
};

struct service_addr {
    enum socks5_addr_type type;
    char* host;
    unsigned short port;
};

#ifndef CONFIG_LOG
#define CONFIG_LOG 1
#endif
#if CONFIG_LOG
#define dolog(...) do { \
    char buf[1024]; \
    snprintf(buf, sizeof(buf), __VA_ARGS__); \
    custom_log(buf); \
} while(0)
#else
static void dolog(const char* fmt, ...) { }
#endif

struct socks5_addrport {
    enum socks5_addr_type type;
    char addr[MAX_DNS_LEN + 1];
    unsigned short port;
};


int compareSocks5Addrport(const struct socks5_addrport* addrport1, const struct socks5_addrport* addrport2) {
    if (addrport1->type == addrport2->type && 
        strcmp(addrport1->addr, addrport2->addr) == 0 && 
        addrport1->port == addrport2->port) {
        return 0;
    }
    return -1;
}

int resolveSocks5Addrport(struct socks5_addrport* addrport, enum socks5_socket_type  stype, union sockaddr_union* addr) {
     struct addrinfo* ai;
     if (stype == TCP_SOCKET) {
        /* there's no suitable errorcode in rfc1928 for dns lookup failure */
        if(resolve_tcp(addrport->addr, addrport->port, &ai)) return -EC_GENERAL_FAILURE;
    } else if (stype == UDP_SOCKET) {
        if(resolve_udp(addrport->addr, addrport->port, &ai)) return -EC_GENERAL_FAILURE;
    } else {
        abort();
    }

    memcpy(addr, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    return 0;
}

static int parse_addrport(unsigned char *buf, size_t n, struct socks5_addrport* addrport) {
    assert(addrport != NULL);
    if (n < 2) return -EC_GENERAL_FAILURE;
    int af = AF_INET;
    int minlen = 1 + 4 + 2, l;
    char namebuf[MAX_DNS_LEN + 1];

    enum socks5_addr_type type = buf[0];
    switch(type) {
        case SOCKS5_IPV6: /* ipv6 */
            af = AF_INET6;
            minlen = 1 + 16 + 2;
            /* fall through */
        case SOCKS5_IPV4: /* ipv4 */
            if(n < minlen) return -EC_GENERAL_FAILURE;
            if(namebuf != inet_ntop(af, buf+1, namebuf, sizeof namebuf))
                return -EC_GENERAL_FAILURE; /* malformed or too long addr */
            break;
        case SOCKS5_DNS: /* dns name */
            l = buf[1];
            minlen = 1 + (1 + l) + 2 ;
            if(n < minlen) return -EC_GENERAL_FAILURE;
            memcpy(namebuf, buf+2, l);
            namebuf[l] = 0;
            break;
        default:
            return -EC_ADDRESSTYPE_NOT_SUPPORTED;
    }
    
    addrport->type = type;
    addrport->addr[sizeof addrport->addr - 1] = '\0';
    strncpy(addrport->addr, namebuf, sizeof addrport->addr -1);
    addrport->port = (buf[minlen-2] << 8) | buf[minlen-1];
    return minlen;
}

static int parse_socks_request_header(unsigned char *buf, size_t n, int* cmd, union sockaddr_union* svc_addr) {
    assert(svc_addr != NULL);
    if(n < 3) return -EC_GENERAL_FAILURE;
    if(buf[0] != VERSION) return -EC_GENERAL_FAILURE;
    if(buf[1] != CONNECT && buf[1] != UDP_ASSOCIATE) return -EC_COMMAND_NOT_SUPPORTED; /* we support only CONNECT and UDP ASSOCIATE method */
    *cmd = buf[1];
    if(buf[2] != RSV) return -EC_GENERAL_FAILURE; /* malformed packet */

    struct socks5_addrport addrport;
    int ret = parse_addrport(buf + 3, n - 3, &addrport);
    if (ret < 0) {
        return ret;
    }
    int socktype = *cmd == CONNECT? TCP_SOCKET : UDP_SOCKET;
    ret = resolveSocks5Addrport(&addrport, socktype, svc_addr);
    if (ret < 0) return ret;
    return EC_SUCCESS;
}

static int connect_socks_target(union sockaddr_union* remote_addr, struct client *client) {
    int fd = socket(SOCKADDR_UNION_AF(remote_addr), SOCK_STREAM, 0);
    if(fd == -1) {
        eval_errno:
        if(fd != -1) close(fd);
        switch(errno) {
            case ETIMEDOUT:
                return -EC_TTL_EXPIRED;
            case EPROTOTYPE:
            case EPROTONOSUPPORT:
            case EAFNOSUPPORT:
                return -EC_ADDRESSTYPE_NOT_SUPPORTED;
            case ECONNREFUSED:
                return -EC_CONN_REFUSED;
            case ENETDOWN:
            case ENETUNREACH:
                return -EC_NET_UNREACHABLE;
            case EHOSTUNREACH:
                return -EC_HOST_UNREACHABLE;
            case EBADF:
            default:
            perror("socket/connect");
            return -EC_GENERAL_FAILURE;
        }
    }
    if(connect(fd, (struct sockaddr*)remote_addr, SOCKADDR_UNION_LENGTH(remote_addr)) == -1)
        goto eval_errno;

    if(CONFIG_LOG) {
        char clientname[256], targetname[256];
        int af = SOCKADDR_UNION_AF(&client->addr);
        void *ipdata = SOCKADDR_UNION_ADDRESS(&client->addr);
        inet_ntop(af, ipdata, clientname, sizeof clientname);
        af = SOCKADDR_UNION_AF(remote_addr);
        ipdata = SOCKADDR_UNION_ADDRESS(remote_addr);
        inet_ntop(af, ipdata, targetname, sizeof targetname);
        dolog("SOCKS connection: %s -> %s:%d", 
            clientname, targetname, ntohs(SOCKADDR_UNION_PORT(remote_addr)));
    }
    return fd;
}

static int is_authed(union sockaddr_union *client, union sockaddr_union *authedip) {
    int af = SOCKADDR_UNION_AF(authedip);
    if(af == SOCKADDR_UNION_AF(client)) {
        size_t cmpbytes = af == AF_INET ? 4 : 16;
        void *cmp1 = SOCKADDR_UNION_ADDRESS(client);
        void *cmp2 = SOCKADDR_UNION_ADDRESS(authedip);
        if(!memcmp(cmp1, cmp2, cmpbytes)) return 1;
    }
    return 0;
}

static int is_in_authed_list(union sockaddr_union *caddr) {
    size_t i;
    for(i=0;i<sblist_getsize(auth_ips);i++)
        if(is_authed(caddr, sblist_get(auth_ips, i)))
            return 1;
    return 0;
}

static void add_auth_ip(union sockaddr_union *caddr) {
    sblist_add(auth_ips, caddr);
}

static enum authmethod check_auth_method(unsigned char *buf, size_t n, struct client*client) {
    if(buf[0] != 5) return AM_INVALID;
    size_t idx = 1;
    if(idx >= n ) return AM_INVALID;
    int n_methods = buf[idx];
    idx++;
    while(idx < n && n_methods > 0) {
        if(buf[idx] == AM_NO_AUTH) {
            if(!auth_user) return AM_NO_AUTH;
            else if(auth_ips) {
                int authed = 0;
                if(pthread_rwlock_rdlock(&auth_ips_lock) == 0) {
                    authed = is_in_authed_list(&client->addr);
                    pthread_rwlock_unlock(&auth_ips_lock);
                }
                if(authed) return AM_NO_AUTH;
            }
        } else if(buf[idx] == AM_USERNAME) {
            if(auth_user) return AM_USERNAME;
        }
        idx++;
        n_methods--;
    }
    return AM_INVALID;
}

static void send_auth_response(int fd, int version, enum authmethod meth) {
    unsigned char buf[2];
    buf[0] = version;
    buf[1] = meth;
    write(fd, buf, 2);
}

static ssize_t send_response(int fd, enum errorcode ec, union sockaddr_union* addr) {
    void* addr_ptr = SOCKADDR_UNION_ADDRESS(addr);
    assert(addr_ptr != NULL);
    unsigned short port = ntohs(SOCKADDR_UNION_PORT(addr));
    // IPv6 takes 22 bytes, which is the longest
    unsigned char buf[4 + 16 + 2] = {VERSION, ec, RSV};
    size_t len = 0;
    if (SOCKADDR_UNION_AF(addr) == AF_INET) {
        buf[3] = SOCKS5_IPV4;
        memcpy(buf+4, addr_ptr, 4);
        buf[8] = port >> 8;
        buf[9] = port & 0xFF;
        len = 10;
    } else if (SOCKADDR_UNION_AF(addr) == AF_INET6) {
        buf[3] = SOCKS5_IPV6;
        memcpy(buf+4, addr_ptr, 16);
        buf[20] = port >> 8;
        buf[21] = port & 0xFF;
        len = 22;
    } else {
        abort();
    }
    return write(fd, buf, len);
}

static void send_error(int fd, enum errorcode ec) {
    /* position 4 contains ATYP, the address type, which is the same as used in the connect
       request. we're lazy and return always IPV4 address type in errors. */
    char buf[10] = { 5, ec, 0, 1 /*AT_IPV4*/, 0,0,0,0, 0,0 };
    write(fd, buf, 10);
}

static uint64_t total_upload_bytes = 0;
static uint64_t total_download_bytes = 0;
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

static void update_traffic_stats(size_t uploaded, size_t downloaded) {
    pthread_mutex_lock(&stats_mutex);
    total_upload_bytes += uploaded;
    total_download_bytes += downloaded;
    update_traffic_stats_ui(total_upload_bytes, total_download_bytes);
    pthread_mutex_unlock(&stats_mutex);
}
static void copyloop(int fd1, int fd2) {
    int kq = kqueue();
    if (kq == -1) {
        perror("kqueue");
        return;
    }

    struct kevent events[2];
    struct kevent changes[2];

    EV_SET(&changes[0], fd1, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    EV_SET(&changes[1], fd2, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);

    if (kevent(kq, changes, 2, NULL, 0, NULL) == -1) {
        perror("kevent");
        close(kq);
        return;
    }

    while (1) {
        int nev = kevent(kq, NULL, 0, events, 2, NULL);
        if (nev == -1) {
            if (errno == EINTR || errno == EAGAIN) continue;
            perror("kevent");
            break;
        } else if (nev == 0) {
            break; // Timeout reached (if applicable)
        }

        for (int i = 0; i < nev; i++) {
            int infd = (int)events[i].ident;
            int outfd = (infd == fd1) ? fd2 : fd1;

            if (events[i].filter == EVFILT_READ) {
                char buf[1024];
                ssize_t sent = 0, n = read(infd, buf, sizeof(buf));
                if (n <= 0) {
                    close(kq);
                    return;
                }

                while (sent < n) {
                    ssize_t m = write(outfd, buf + sent, n - sent);
                    if (m < 0) {
                        close(kq);
                        return;
                    }
                    sent += m;
                }

                if (infd == fd1) {
                    update_traffic_stats(n, 0);
                } else {
                    update_traffic_stats(0, n);
                }
            }
        }
    }

    close(kq);
}

// caller must free socks5_addr manually
static ssize_t extract_udp_data(unsigned char* buf, ssize_t n, struct socks5_addrport* addrport) {
    if (n < 3) return -EC_GENERAL_FAILURE;
    if (buf[0] != RSV || buf[1] != RSV) return -EC_GENERAL_FAILURE;
    if (buf[2] != 0) return -EC_GENERAL_FAILURE;  // framentation not supported

    ssize_t offset = 3;
    int ret = parse_addrport(buf + offset, n - offset, addrport);
    if (ret < 0) {
        return ret;
    }
    assert(ret > 0);

    offset += ret;
    return offset;
}

struct fd_socks5addr {
    int fd;
    struct socks5_addrport addrport;
};

int compare_fd_socks5addr_by_fd(char* item1, char* item2) {
    struct fd_socks5addr* i1 = ( struct fd_socks5addr*)item1;
    struct fd_socks5addr* i2 = ( struct fd_socks5addr*)item2;
    if (i1->fd == i2->fd) return 0;
    return 1;
}

int compare_fd_socks5addr_by_addrport(char* item1, char* item2) {
    struct fd_socks5addr* ap1 = ( struct fd_socks5addr*)item1;
    struct fd_socks5addr* ap2 = ( struct fd_socks5addr*)item2;
    return compareSocks5Addrport(&ap1->addrport, &ap2->addrport);
}

static void copy_loop_udp(int tcp_fd, int udp_fd) {
    int kq = kqueue();
    if (kq == -1) {
        perror("kqueue");
        return;
    }

    struct kevent changes[2];
    EV_SET(&changes[0], tcp_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    EV_SET(&changes[1], udp_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);

    if (kevent(kq, changes, 2, NULL, 0, NULL) == -1) {
        perror("kevent");
        close(kq);
        return;
    }

    int udp_is_bound = 1;
    union sockaddr_union client_addr;
    socklen_t socklen = sizeof client_addr;
    if (-1 == getpeername(udp_fd, (struct sockaddr*)&client_addr, &socklen)) {
        if (errno == ENOTCONN) {
            udp_is_bound = 0;
            dprintf(1, "fd %d is not bound yet\n", udp_fd);
        } else {
            abort();
        }
    }

    ssize_t n, ret;
    struct fd_socks5addr item;
    sblist* sock_list = sblist_new(sizeof(struct fd_socks5addr), 1);

    while (1) {
        struct kevent events[1024];
        int nev = kevent(kq, NULL, 0, events, 1024, NULL);
        if (nev == -1) {
            if (errno == EINTR || errno == EAGAIN) continue;
            perror("kevent");
            goto UDP_LOOP_END;
        }

        for (int i = 0; i < nev; i++) {
            int fd = (int)events[i].ident;

            // support up to 1024 bytes of data
            unsigned char buf[MAX_SOCKS5_HEADER_LEN + 1024];

            // TCP socket
            if (fd == tcp_fd) {
                n = read(fd, buf, sizeof(buf) - 1);
                if (n == 0) {
                    // SOCKS5 TCP connection closed
                    goto UDP_LOOP_END;
                }
                if (n == -1) {
                    if (errno == EINTR || errno == EAGAIN) continue;
                    perror("read from tcp socket");
                    goto UDP_LOOP_END;
                }
                buf[n - 1] = '\0';
                dprintf(1, "received unexpectedly from TCP socket in UDP associate: %s", buf);
            }

            // client UDP socket
            if (fd == udp_fd) {
                if (!udp_is_bound) {
                    socklen = sizeof client_addr;
                    n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr*)&client_addr, &socklen);
                } else {
                    n = recv(udp_fd, buf, sizeof(buf), 0);
                }
                if (n == -1) {
                    if (errno == EINTR || errno == EAGAIN) continue;
                    perror("recv from udp socket");
                    goto UDP_LOOP_END;
                }
                if (!udp_is_bound) {
                    if (connect(udp_fd, (const struct sockaddr*)&client_addr, socklen)) {
                        perror("connect");
                        goto UDP_LOOP_END;
                    }
                    udp_is_bound = 1;
                    dprintf(1, "fd %d is bound now\n", udp_fd);
                }

                ssize_t offset = extract_udp_data(buf, n, &item.addrport);
                if (offset < 0) {
                    dprintf(2, "failed to extract from udp packet %ld", offset);
                    goto UDP_LOOP_END;
                }

                int send_fd = 0;
                int idx = sblist_search(sock_list, (char*)&item, compare_fd_socks5addr_by_addrport);
                if (idx != -1) {
                    struct fd_socks5addr* item_found = (struct fd_socks5addr*)sblist_item_from_index(sock_list, idx);
                    send_fd = item_found->fd;
                } else {
                    union sockaddr_union target_addr;
                    ret = resolveSocks5Addrport(&item.addrport, UDP_SOCKET, &target_addr);
                    if (ret < 0) {
                        dprintf(2, "failed to resolve socks5 addrport, %ld", ret);
                        goto UDP_LOOP_END;
                    }

                    // create a new socket
                    int fd = socket(SOCKADDR_UNION_AF(&target_addr), SOCK_DGRAM, 0);
                    if (-1 == connect(fd, (const struct sockaddr*)&target_addr, ((const struct sockaddr*)&target_addr)->sa_len)) {
                        perror("connect");
                        send_error(tcp_fd, EC_GENERAL_FAILURE);
                        goto UDP_LOOP_END;
                    }
                    item.fd = fd;
                    sblist_add(sock_list, &item);

                    // add to kqueue
                    struct kevent new_event;
                    EV_SET(&new_event, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
                    if (kevent(kq, &new_event, 1, NULL, 0, NULL) == -1) {
                        perror("kevent");
                        goto UDP_LOOP_END;
                    }
                    send_fd = fd;
                    if (CONFIG_LOG) {
                        char targetname[256];
                        int af = SOCKADDR_UNION_AF(&target_addr);
                        void *ipdata = SOCKADDR_UNION_ADDRESS(&target_addr);
                        unsigned short port = ntohs(SOCKADDR_UNION_PORT(&target_addr));
                        inet_ntop(af, ipdata, targetname, sizeof targetname);
                        dolog("UDP fd[%d] remote address is %s:%d\n", send_fd, targetname, port);
                    }
                }
                ssize_t ret = send(send_fd, buf + offset, n - offset, 0);
                if (ret < 0) {
                    perror("send");
                    goto UDP_LOOP_END;
                }
            }

            // UDP sockets for target addresses
            if (fd != tcp_fd && fd != udp_fd) {
                item.fd = fd;
                int idx = sblist_search(sock_list, (char *)&item, compare_fd_socks5addr_by_fd);
                if (idx == -1) {
                    dprintf(2, "UDP socket not found");
                    goto UDP_LOOP_END;
                }
                struct fd_socks5addr *item = (struct fd_socks5addr*)sblist_item_from_index(sock_list, idx);
                buf[0] = RSV;
                buf[1] = RSV;
                buf[2] = 0; // FRAG
                struct socks5_addrport* addrport = &item->addrport;
                buf[3] = addrport->type;
                size_t offset = 4;
                if (addrport->type == SOCKS5_DNS) {
                    size_t len = strlen(item->addrport.addr);
                    buf[offset++] = len;
                    memcpy(buf + offset, addrport->addr, len);
                    offset += len;
                } else if (addrport->type == SOCKS5_IPV4) {
                    struct in_addr addr_in4;
                    if (1 != inet_pton(AF_INET, addrport->addr, &addr_in4)) {
                        dprintf(2, "invalid IPv4 address, %s", addrport->addr);
                        goto UDP_LOOP_END;
                    }
                    memcpy(buf + offset, &addr_in4, sizeof addr_in4);
                    offset += sizeof addr_in4;
                } else if (addrport->type == SOCKS5_IPV6) {
                    struct in6_addr addr_in6;
                    if (1 != inet_pton(AF_INET6, addrport->addr, &addr_in6)) {
                        dprintf(2, "invalid IPv6 address, %s", addrport->addr);
                        goto UDP_LOOP_END;
                    }
                    memcpy(buf + offset, &addr_in6, sizeof addr_in6);
                    offset += sizeof addr_in6;
                } else {
                    abort();
                }
                buf[offset++] = addrport->port >> 8;
                buf[offset++] = addrport->port & 0xFF;
                n = recv(fd, buf + offset, sizeof(buf) - offset, 0);
                if(n <= 0) {
                    perror("recv from target address");
                    goto UDP_LOOP_END;
                }
                ret = write(udp_fd, buf, offset + n);
                if (ret < 0) {
                    perror("write to udp_fd");
                    goto UDP_LOOP_END;
                }
            }
        }
    }

UDP_LOOP_END:
    for (int i = 0; i < sblist_getsize(sock_list); i++) {
        struct fd_socks5addr *item = (struct fd_socks5addr*)sblist_item_from_index(sock_list, i);
        close(item->fd);
    }
    sblist_free(sock_list);
    close(kq);
}

static enum errorcode check_credentials(unsigned char* buf, size_t n) {
    if(n < 5) return EC_GENERAL_FAILURE;
    if(buf[0] != 1) return EC_GENERAL_FAILURE;
    unsigned ulen, plen;
    ulen=buf[1];
    if(n < 2 + ulen + 2) return EC_GENERAL_FAILURE;
    plen=buf[2+ulen];
    if(n < 2 + ulen + 1 + plen) return EC_GENERAL_FAILURE;
    char user[256], pass[256];
    memcpy(user, buf+2, ulen);
    memcpy(pass, buf+2+ulen+1, plen);
    user[ulen] = 0;
    pass[plen] = 0;
    if(!strcmp(user, auth_user) && !strcmp(pass, auth_pass)) {
        dolog("Client authentication successful for user: %s\n", user);
        return EC_SUCCESS;
    }
    dolog("Client authentication failed for user: %s\n", user);
    return EC_NOT_ALLOWED;
}

int udp_svc_setup(union sockaddr_union* client_addr) {
    int fd = socket(SOCKADDR_UNION_AF(client_addr), SOCK_DGRAM, 0);
    if(fd == -1) {
        if(fd != -1) close(fd);
        switch(errno) {
            case ETIMEDOUT:
                return -EC_TTL_EXPIRED;
            case EPROTOTYPE:
            case EPROTONOSUPPORT:
            case EAFNOSUPPORT:
                return -EC_ADDRESSTYPE_NOT_SUPPORTED;
            case ECONNREFUSED:
                return -EC_CONN_REFUSED;
            case ENETDOWN:
            case ENETUNREACH:
                return -EC_NET_UNREACHABLE;
            case EHOSTUNREACH:
                return -EC_HOST_UNREACHABLE;
            case EBADF:
            default:
                perror("socket/connect");
                return -EC_GENERAL_FAILURE;
        }
    }

    int af = SOCKADDR_UNION_AF(client_addr);
    if ( (af == AF_INET && client_addr->v4.sin_addr.s_addr != INADDR_ANY) || 
        (af == AF_INET6 && !IN6_ARE_ADDR_EQUAL(&client_addr->v6.sin6_addr, &in6addr_any)) ) {
        if (connect(fd, (const struct sockaddr*)client_addr, sizeof(union sockaddr_union))) {
            perror("udp connect");
            return -1;
        }
        return fd;
    }

    struct addrinfo* addr;
    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_family = af,
        .ai_socktype = SOCK_DGRAM,
    };
    int ret = getaddrinfo(NULL, "0", &hints, &addr);
    if (0 != ret) {
        dprintf(2, "could not resolve to a local UDP address");
        return ret;
    }
    if (0 != bind(fd, addr->ai_addr, addr->ai_addrlen)) {
        perror("udplocal bind");
        freeaddrinfo(addr);
        return -1;
    }
    freeaddrinfo(addr);
    return fd;
}

static void* clientthread(void *data) {
    struct thread *t = data;
    char clientname[256];
    int af = SOCKADDR_UNION_AF(&t->client.addr);
    void *ipdata = SOCKADDR_UNION_ADDRESS(&t->client.addr);
    unsigned short port = ntohs(SOCKADDR_UNION_PORT(&t->client.addr));
    inet_ntop(af, ipdata, clientname, sizeof clientname);
    
    // Log new connection
    dolog("New SOCKS client connected from %s:%d", clientname, port);
    
    t->state = SS_1_CONNECTED;
    unsigned char buf[1024];
    ssize_t n;
    int ret;
    // for CONNECT, this is target TCP address
    // for UDP ASSOCIATE, this is client UDP address
    union sockaddr_union address, local_addr;

    enum authmethod am;
    while((n = recv(t->client.fd, buf, sizeof buf, 0)) > 0) {
        switch(t->state) {
            case SS_1_CONNECTED:
                am = check_auth_method(buf, n, &t->client);
                if(am == AM_NO_AUTH) t->state = SS_3_AUTHED;
                else if (am == AM_USERNAME) t->state = SS_2_NEED_AUTH;
                send_auth_response(t->client.fd, 5, am);
                if(am == AM_INVALID) goto breakloop;
                break;
            case SS_2_NEED_AUTH:
                ret = check_credentials(buf, n);
                send_auth_response(t->client.fd, 1, ret);
                if(ret != EC_SUCCESS)
                    goto breakloop;
                t->state = SS_3_AUTHED;
                if(auth_ips && !pthread_rwlock_wrlock(&auth_ips_lock)) {
                    if(!is_in_authed_list(&t->client.addr))
                        add_auth_ip(&t->client.addr);
                    pthread_rwlock_unlock(&auth_ips_lock);
                }
                break;
            case SS_3_AUTHED:
                (void)0;
                int cmd;
                ret = parse_socks_request_header(buf, n, &cmd, &address);
                if (ret != EC_SUCCESS) {
                    goto breakloop;
                }
                
                if (cmd == CONNECT) {
                    ret = connect_socks_target(&address, &t->client);
                    if(ret < 0) {
                        send_error(t->client.fd, ret*-1);
                        goto breakloop;
                    }
                    int remotefd = ret;
                    socklen_t len = sizeof(union sockaddr_union);
                    if (getsockname(remotefd, (struct sockaddr*)&local_addr, &len)) goto breakloop;
                    if (-1 == send_response(t->client.fd, EC_SUCCESS, &local_addr)) {
                        close(remotefd);
                        goto breakloop;
                    }
                    copyloop(t->client.fd, remotefd);
                    close(remotefd);
                    goto breakloop;
                } else if (cmd == UDP_ASSOCIATE) {
                    int fd = udp_svc_setup(&address);
                    if(fd <= 0) {
                        send_error(t->client.fd, fd*-1);
                        goto breakloop;
                    }

                    socklen_t len = sizeof(union sockaddr_union);
                    if (getsockname(fd, (struct sockaddr*)&local_addr, &len)) goto breakloop;
                    if (-1 == send_response(t->client.fd, EC_SUCCESS, &local_addr)) {
                        close(fd);
                        goto breakloop;
                    }
                    if (CONFIG_LOG) {
                        char clientname[256];
                        int af = SOCKADDR_UNION_AF(&address);
                        void *ipdata = SOCKADDR_UNION_ADDRESS(&address);
                        unsigned short port_c = ntohs(SOCKADDR_UNION_PORT(&address));
                        inet_ntop(af, ipdata, clientname, sizeof clientname);
                        char udp_svc_name[256];
                        ipdata = SOCKADDR_UNION_ADDRESS(&local_addr);
                        unsigned int port_s = ntohs(SOCKADDR_UNION_PORT(&local_addr));
                        inet_ntop(af, ipdata, udp_svc_name, sizeof udp_svc_name);
                        dolog("UDP Associate: client[%d] %s:%d bound to local address %s:%d\n", 
                            t->client.fd, clientname, port_c, udp_svc_name, port_s);
                    }
                    copy_loop_udp(t->client.fd, fd);
                    close(fd);
                    goto breakloop;
                } else {
                    // should not be here
                    abort();
                }
        }
    }
breakloop:
    // Log disconnection
    dolog("SOCKS client disconnected: %s:%d", clientname, port);
    close(t->client.fd);
    t->done = 1;

    return 0;
}

static void collect(sblist *threads) {
    size_t i;
    for(i=0;i<sblist_getsize(threads);) {
        struct thread* thread = *((struct thread**)sblist_get(threads, i));
        if(thread->done) {
            pthread_join(thread->pt, 0);
            sblist_delete(threads, i);
            free(thread);
        } else
            i++;
    }
}

static int usage(void) {
    dprintf(2,
        "MicroSocks SOCKS5 Server\n"
        "------------------------\n"
        "usage: microsocks -1 -q -i listenip -p port -u user -P password -b bindaddr\n"
        "all arguments are optional.\n"
        "by default listenip is 0.0.0.0 and port 1080.\n\n"
        "option -q disables logging.\n"
        "option -b specifies which ip outgoing connections are bound to\n"
        "option -1 activates auth_once mode: once a specific ip address\n"
        "authed successfully with user/pass, it is added to a whitelist\n"
        "and may use the proxy without auth.\n"
        "this is handy for programs like firefox that don't support\n"
        "user/pass auth. for it to work you'd basically make one connection\n"
        "with another program that supports it, and then you can use firefox too.\n"
    );
    return 1;
}

/* prevent username and password from showing up in top. */
static void zero_arg(char *s) {
    size_t i, l = strlen(s);
    for(i=0;i<l;i++) s[i] = 0;
}

int socks_main(int argc, char** argv) {
    int ch;
    const char *listenip = "0.0.0.0";
    unsigned port = 1080;
    while((ch = getopt(argc, argv, ":1qi:p:u:P:")) != -1) {
        switch(ch) {
            case '1':
                auth_ips = sblist_new(sizeof(union sockaddr_union), 8);
                break;
            case 'q':
                quiet = 1;
                break;
            case 'u':
                auth_user = strdup(optarg);
                zero_arg(optarg);
                break;
            case 'P':
                auth_pass = strdup(optarg);
                zero_arg(optarg);
                break;
            case 'i':
                listenip = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case ':':
                dprintf(2, "error: option -%c requires an operand\n", optopt);
                /* fall through */
            case '?':
                return usage();
        }
    }
    if((auth_user && !auth_pass) || (!auth_user && auth_pass)) {
        dprintf(2, "error: user and pass must be used together\n");
        return 1;
    }
    if(auth_ips && !auth_pass) {
        dprintf(2, "error: auth-once option must be used together with user/pass\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    struct server s;
    sblist *threads = sblist_new(sizeof (struct thread*), 8);
    if(server_setup(&s, listenip, port)) {
        perror("server_setup");
        return 1;
    }
    server = &s;

    while(1) {
        collect(threads);
        struct client c;
        struct thread *curr = malloc(sizeof (struct thread));
        if(!curr) goto oom;
        curr->done = 0;
        if(server_waitclient(&s, &c)) {
            dolog("failed to accept connection\n");
            free(curr);
            usleep(FAILURE_TIMEOUT);
            continue;
        }
        curr->client = c;
        if(!sblist_add(threads, &curr)) {
            close(curr->client.fd);
            free(curr);
            oom:
            dolog("rejecting connection due to OOM\n");
            usleep(FAILURE_TIMEOUT); /* prevent 100% CPU usage in OOM situation */
            continue;
        }
        pthread_attr_t *a = 0, attr;
        if(pthread_attr_init(&attr) == 0) {
            a = &attr;
            pthread_attr_setstacksize(a, THREAD_STACK_SIZE);
        }
        if(pthread_create(&curr->pt, a, clientthread, curr) != 0)
            dolog("pthread_create failed. OOM?\n");
        if(a) pthread_attr_destroy(&attr);
    }
}


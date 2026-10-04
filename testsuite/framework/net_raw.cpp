// net_raw.cpp —— 见 net_raw.h
#include "net_raw.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace ts {

int raw_connect(const char* ip, int port, int rcvTimeoutMs, int rcvbuf, int mss)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (rcvbuf > 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    if (mss > 0)
        setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
    if (rcvTimeoutMs > 0) {
        timeval tv{rcvTimeoutMs / 1000, (rcvTimeoutMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

MSGHEAD make_head(int id, const char* qname, const char* itemname)
{
    MSGHEAD h{};
    h.id = id;
    if (qname) {
        strncpy(h.qname, qname, sizeof(h.qname) - 1);
        h.qname[sizeof(h.qname) - 1] = '\0';
    }
    if (itemname) {
        strncpy(h.itemname, itemname, sizeof(h.itemname) - 1);
        h.itemname[sizeof(h.itemname) - 1] = '\0';
    }
    return h;
}

bool raw_send(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

RecvStatus raw_recv(int fd, void* buf, size_t len)
{
    char* p = (char*)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return RecvStatus::CLOSED;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return RecvStatus::TIMEOUT;
            if (errno == ECONNRESET) return RecvStatus::CLOSED;  // 对端关闭时仍有未读数据 → RST，等同被关闭
            return RecvStatus::PROTO_ERR;
        }
        p += n;
        len -= (size_t)n;
    }
    return RecvStatus::OK;
}

bool send_msg(int fd, const MSGHEAD& h, const void* body, int bodyLen)
{
    if (!raw_send(fd, &h, sizeof(MSGHEAD))) return false;
    if (bodyLen > 0 && body) return raw_send(fd, body, (size_t)bodyLen);
    return true;
}

RecvStatus recv_msg(int fd, MSGHEAD& out, std::vector<char>& body, int maxBody)
{
    RecvStatus st = raw_recv(fd, &out, sizeof(MSGHEAD));
    if (st != RecvStatus::OK) return st;
    if (out.bodysize < 0 || out.bodysize > maxBody)
        return RecvStatus::PROTO_ERR;  // 流错位：bodysize 不合理
    body.resize((size_t)out.bodysize);
    if (out.bodysize > 0)
        return raw_recv(fd, body.data(), (size_t)out.bodysize);
    return RecvStatus::OK;
}

}  // namespace ts

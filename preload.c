#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ssize_t (*real_send)(int, const void *, size_t, int) = NULL;
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int) = NULL;

__attribute__((constructor))
static void init_wrappers(void) {
    fprintf(stderr, "[zerocopy] libzerocopy.so loaded!\n");
    real_send = dlsym(RTLD_NEXT, "send");
    real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
    if (!real_send || !real_sendmsg) {
        _exit(1);
    }
}

ssize_t send(int sockfd, const void *buf, size_t len,
               int flags)
{
    // OR in MSG_ZEROCOPY

    if(len > 102400) { // Example threshold
        flags |= MSG_ZEROCOPY;
    }

    // fprintf(stderr, "send!\n");

    return real_send(sockfd, buf, len, flags);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    // Make a modifiable copy so we can OR in the flag
    struct msghdr tmp = *msg;
    if(tmp.msg_iovlen > 0) {
        struct iovec *iov = malloc(tmp.msg_iovlen * sizeof(struct iovec));
        if (!iov) {
            return -1; // Handle memory allocation failure
        }
        memcpy(iov, tmp.msg_iov, tmp.msg_iovlen * sizeof(struct iovec));
        tmp.msg_iov = iov;
    }
    tmp.msg_flags |= MSG_ZEROCOPY;
    return real_sendmsg(sockfd, &tmp, flags);
}
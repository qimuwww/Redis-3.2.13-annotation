/* Linux epoll(2) based ae.c module
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
    epoll内核数据结构由红黑树实现，数据内部维护者两个list：
        1. 进程中声明过的感兴趣的文件描述符列表。
        2. 处于I/O就绪的文件描述符列表。
    
*/

#include <sys/epoll.h>

typedef struct aeApiState
{
    int epfd; // 接收epoll实例的文件描述符

    /*
        typedef union epoll_data
        {
            void *ptr;
            int fd;
            uint32_t u32;
            uint64_t u64;
        } epoll_data_t;

        struct epoll_event
        {
            // 感兴趣的事件掩码
            // AE_READBLE(EPOLLIN)
            // AE_WRITEBLE(EPOLLOUT)
            // ...
            uint32_t events;	
            epoll_data_t data; 
        } __EPOLL_PACKED
    */
    // events以数组形式存在， 存放epoll_wait返回的就绪的文件描述符的信息
    struct epoll_event *events;
} aeApiState;

static int aeApiCreate(aeEventLoop *eventLoop)
{
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state)
        return -1;
    state->events = zmalloc(sizeof(struct epoll_event) * eventLoop->setsize);
    if (!state->events)
    {
        zfree(state);
        return -1;
    }

    // size告诉内核实例化epoll数据结构时的初始化大小。
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */
    if (state->epfd == -1)
    {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int
aeApiResize(aeEventLoop *eventLoop, int setsize)
{
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct epoll_event) * setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop)
{
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    aeApiState *state = eventLoop->apidata;
    /*
    typedef union epoll_data {
               void        *ptr;
               int          fd; // 此处使用fd，为了epoll_wait能够返回就绪事件的文件描述符
               uint32_t     u32;
               uint64_t     u64;
           } epoll_data_t;

           struct epoll_event {
                uint32_t     events; //指定了我们为待检查的描述符 fd 上所感兴趣的事件集合。    
                epoll_data_t data; 
            };
    */
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    // 判断fd是否在兴趣列表中，如果试图操作一个不在兴趣列表中的fd，epoll_ctl将出现
    // EEXIST错误
    int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    // 更新感兴趣的事件掩码
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    if (mask & AE_READABLE)
        ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE)
        ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    /*
    int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    系统调用 epoll_ctl()能够修改由文件描述符 epfd 所代表的 epoll 实例中的兴趣列表.
    参数 fd 指明了要修改兴趣列表中的哪一个文件描述符的设定.fd 不能作为普通文件或目录
    的文件描述符（会出现 EPERM 错误）。

    参数op：
        EPOLL_CTL_ADD，将描述符 fd 添加到 epoll 实例 epfd 中的兴趣列表中去。
        EPOLL_CTL_MOD，修改描述符 fd 上设定的事件，需要用到由 ev 所指向的结构体中
        的信息。如果我们试图修改不在兴趣列表中的文件描述符， epoll_ctl()将出现 ENOENT 错误。
        EPOLL_CTL_DEL，将文件描述符 fd 从 epfd 的兴趣列表中移除。该操作忽略参数 
        ev。如果我们试图移除一个不在 epfd 的兴趣列表中的文件描述符， epoll_ctl()将出
        现 ENOENT 错误。关闭一个文件描述符会自动将其从所有的 epoll 实例的兴趣列表中移除。
    */
    if (epoll_ctl(state->epfd, op, fd, &ee) == -1)
        return -1;
    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask)
{
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    // 按位取反与
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE)
        ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE)
        ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    // 试图移除一个不在兴趣列表中的fd，epoll_ctl将产生ENOENT的错误
    // 如果此时还有对此fd感兴趣的事件类型 则使用EPOLL_CTL_MOD，否则使用
    // EPOLL_CTL_DEL
    if (mask != AE_NONE)
    {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ee);
    }
    else
    {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, &ee);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp)
{
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    /*
    int epoll_wait(int epfd, struct epoll_event *events,
                      int maxevents, int timeout);
    返回 epoll 实例中处于就绪态的文件描述符信息。单个 epoll_wait()调
    用能返回多个就绪态文件描述符的信息。
    参数 evlist 所指向的结构体数组中返回的是有关就绪态文件描述符的信息。
    events 字段返回了在该描述符上已经发生的事件掩码。 Data 字段返回的
    是我们在描述符上使用 cpoll_ctl()注册感兴趣的事件时在 ev.data 中所指定的值。 

    data 字段是唯一可获知同这个事件相关的文件描述符号的途径。因此，当我们调用 
    epoll_ctl()将文件描述符添加到兴趣列表中时，应该要么将 ev.data.fd 设为文
    件描述符号，要么将 ev.data.ptr 设为指向包含文件描述符号的结构体。

    */
    // 如果超时时间大于0，则传入超时时间，否则阻塞等待返回
    retval = epoll_wait(state->epfd, state->events, eventLoop->setsize,
                        tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : -1);
    // 无就绪事件或错误直接返回
    if (retval > 0)
    {
        int j;

        numevents = retval;
        // 将epoll_wait返回的就绪事件列表写入eventLoop的fired字段中
        for (j = 0; j < numevents; j++)
        {
            int mask = 0;
            struct epoll_event *e = state->events + j;

            if (e->events & EPOLLIN)
                mask |= AE_READABLE;
            if (e->events & EPOLLOUT)
                mask |= AE_WRITABLE;
            if (e->events & EPOLLERR)
                mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP)
                mask |= AE_WRITABLE;
            // 获取这个事件相关的文件描述符和就绪事件掩码
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    return numevents;
}

static char *aeApiName(void)
{
    return "epoll";
}

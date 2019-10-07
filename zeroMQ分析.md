# class ctx_t

主要方法：
- terminate。调用`zmq_ctx_term`时调用这个方法。若没有更多的socket打开，这个方法会导致所有的底层都 shotdown。若还有socket开启，那么在最后一个socket关闭后，将会发生内存空间释放。
- shutdown。这个函数会通过不阻止任一阻塞操作的方式终止进程，并且会主动停止socket。该函数是非阻塞的。
- set/get。设置/获取 ctx 的属性。
- create_socket/destroy_socket。socket的创建和销毁
- endpoint相关函数。
- pend_connection/connect_pending。连接相关函数。

包含的数据结构成员：
- tag。检查对象是否是一个ctx
- array_t<socket_base_t> _sockets。隶属于这个ctx的socket列表。
- io_threads_t _io_threads. ctx包含的io线程。
- mutex_t _slot_sync。用于同步访问全局 `slot-related`数据：sockets，empty-slots，terminating标记。同时也用于同步访问僵尸sockets，并提供内存屏障用于确保所有的cpu-core能看到
一致的数据。
- slots。i_mail_box 的数组指针，用于业务和I/O线程的信箱。
- _endpoints。context 中 endpoint 的集合，key 是地址名，value 是 endpoint。
    - endpoint_t 结构体。它是 socket 和其对应选项的集合。所以 endpoint_t 就是一个带有具体选项的 socket_base_t。

## 实现分析
- terminate。
    - 为 `_pending_connections` 中的每一个 `endpoint`创建一个socket，将其bind到这个`endpoint`上，然后关闭
    - 关闭当前所有的 `socket`。调用 socket_base_t::stop()
    - 关闭收割线程（_reaper->stop)
    - 当所有的操作结束后，释放 ctx 对象的内存。（delete this)

- shutdown。
    - 停止所有ctx包含的socket，与terminate相比，没有内存销毁，关闭endpoint的操作。

- start。
    - 确定slots数量：term和收割线程的数据量 = 2；sockets数量 = _max_sockets；_io_thread_count 数量。slots数量 = term和收割线程数量 + sockets数量 + thread数量。
    - 根据 slots数量，为 _slots 分配内存空间。为 empty_slots分配空间。
    - 初始化 slots。term_id(0) = term_mailbox; reaper_id(1) = _reaper mailbox.
    启动 `reaper_->start()`。
    - 创建 socket 空槽位，id为 _slot数组下标。
    - 创建I/O线程，保存在 _io_threads中，每个线程调用 io_thread->start() 启动线程

- create_socket。
    - 创建 socket。
    - 获取 slot id，来自于 empty_slot。
    - 生成 socket 的 sid
    - 调用 socket_base_t::create 创建 socket，并将新的 socket 放在 _sockets 数组中
    - _slots 数组的 slot id 下标，保存新socket的mailbox。



# struct options_t
描述 socket 选项的数据结构。主要方法是 setsockopt/getsockopt 用于设置/获取选项数据。
其成员字段为选项数据。

# class poller_base_t
zmq 多路复用的基础设施类。它有多种实现，例如： zmq::devpoll_t, zmq::epoll_t, zmq::kqueue_t, zmq::poll_t, zmq::pollset_t, zmq::select_t。
包含下述接口：
    - timer。定时器事件控制
    - fd。文件描述符控制
    - pollin/pollout。输入输出事件的控制
当 poll_t start 后，它等待注册的输入、输出、超时事件的发生，发生后调用 `zmq::i_poll_events` 对象上的函数。
主要成员：
    - typedef std::multimap<uint64_t, timer_info_t> timers_t; timers_t _timers; 超时事件记录。 key = 超时到达时间(expiray time)。value = 超时事件和对应的id
    - execute_timers。遍历 _timers，若有超时事件，则调用 i_poll_events 中的 timer_event 方法。

## struct i_poll_events
提供一组虚接口，用于在文件描述符的事件发生时，调用对应的事件处理接口，包括：
    - in_event。当 fd 准备好读时，I/O 线程调用
    - out_event。当 fd 准备好写时，I/O 线程调用
    - timer_event。当超时时间发生时，调用。

## class worker_poller_base_t:public poller_base_t
具有 worker 线程的 poller。主要数据成员：
    - const thread_ctx_t &_ctx; 对应的 ctx
    - thread_t _worker; 执行I/O操作的线程
    - start。启动函数，执行 `_ctx.start_thread (_worker, worker_routine, this, name_);` 启动线程。 线程的参数 this，就是 poller_base_t， `worker_routine`
    函数的实现，就是 poller->loop()。在 loop 主循环中，不断 update 驱动事件更新，以获取最新的就绪的读/写事件（例如使用 epoll_wait）。

## poller_base_t 的实现
在 zmq 中，存在下述实现：
    - `class select_t`。位于 select.hpp
    - `class epoll_t`。位于 epoll.hpp
    - `class devpoll_t`。位于 devpoll.hpp
通过指定的宏定义（ ZMQ_IOTHREAD_POLLER_USE_DEVPOLL/ZMQ_IOTHREAD_POLLER_USE_EPOLL 等）决定使用哪种 poller 实现。
在上述三个实现的末尾，都有 `typedef xxx poller_t;` 的定义。以此实现跨平台运行。

## class epoll_t
基于 epoll 实现 poller_t。可了解 epoll 的使用方法：
    - epoll_t。构造对象时，创建 epoll_fd。`_epoll_fd = epoll_create (1);`
epoll 事件描述数据结构：
```c++
struct poll_entry_t
{
    fd_t fd;
    epoll_event ev;
    zmq::i_poll_events *events;
};
```
包含 fd, epoll 事件类型和事件触发的处理函数。
    - add_fd。创建事件触发数据结构`poll_entry_t`。包含fd，事件类型，回调函数。通过epoll_ctl(mod = EPOLL_CTL_ADD)将事件添加到 epoll_fd_t 中。其中
    epoll_ctl 的 `struct epoll_event` 指针，指向 `poll_entry_t`，由此可在事件触发时，调用相应的回调函数。
    - rm_fd。移除事件，调用 epoll_ctl(mod = EPOLL_CTL_DEL)
    - set_pollin (handle_t handle_)。 增加 handle_ 中 events 的输入事件标识（EPOLLIN），调用 epoll_ctl(mod = EPOLL_CTL_MOD)修改需要监听的事件类型。
    注意，接口的 handle_t 实质上是 `void *`，这样调用方无从修改 handle_t 的内部实现，同时也对外部屏蔽了内部实现。
    - loop。基于 epoll_wait 实现。其中 epoll_wait 的最大事件数为 max_io_events(256)。在主循环中，依次执行下述步骤：
        - 超时事件处理 (execute_timers)
        - epoll_wait。等待就绪事件
        - 处理就绪事件，其中需要处理下述事件类型：
            - EPOLLERR | EPOLLHUP，这属于错误的情况，按照 in_event 处理
            - EPOLLOUT，使用 out_event 处理
            - EPOLLIN，使用 in_event 处理

zermMQ相关的配置参数，定义在 config.hpp 中，使用 `enum` 定义各配置参数的值。

# class thread_t
包装了 OS 线程的线程类。

# class io_thread_t:public object_t, public i_poll_events
I/O 线程。基于 poll 机制，实现了 i_poll_events 接口。 在 Linux 平台下，使用 epoll_t 作为 poller 。
    - 构造时，会 new 一个poller_t，所以每个 io_thread_t，会有一个属于自己的 poller_t，不同 io_thread_t ，其 poller_t 不共享；
      添加 _mailbox_handle 到 poller 的 EVENT_IN 事件中，监听其读取事件。
    - start。实际调用 poller->start，启动 I/O 线程，线程执行 poll 的 loop 函数。
    - stop。停止线程，通过向指定线程发送 stop 命令实现。
    - in_event。读取事件处理。从 _mailbox 中接收命令（recv）并处理（process_command），直至没有命令可处理。
    - out_event/timer_event。无需要处理的事件
分析 io_thread_t 可见，事件的处理通过 mailbox，无论是业务消息处理，还是停止（stop）线程，都是通过消息进行的。

# 消息机制
zeroMQ 大量使用消息机制实现线程间通信，业务消息收发。这里做下源码分析：

## struct command_t
command_t 数据结构包括下述3部分：
    - zmq::object_t *destination; 命令目的地
    - type。命令类型，例如 stop/bind/activate_read/done等
    - args。命令参数，根据命令类型不同而不同

## class i_mailbox
信箱接口，由recv和send组成，操作对象是 command_t。

### class mailbox_t:public i_mailbox
信箱的实现。


### template <typename T, int N> class ypipe_t
无锁队列，T 表示队列中对象类型，N 表示 pipe 的粒度，表示下一次分配内存空间时，需要多少个对象。
它由 template <typename T, int N> class yqueue_t 组成， yqueue 是高效的队列。可了解下 yqueue和ypipe的实现：
- yqueue，先进先出队列，基于 chunk 和 数组实现。
    - 传统的 queue<T>，其结构是 Node<T> 的原始链表或者数组，链表节点或数组元素的结构如下：
    ```c++
    struct Node
    {
        T value:
        Node* next;
    };
    ```
    如果是链表，那么消息生产、消费频繁时，需要频繁 malloc/free 内存。
    如果是数组，当队列很长时，需要分配大量的连续内存。
    yqueue 结合了链表和数组，它是基于链表的队列，但链表由长度为N的元素数组组成，如下：
    ```c++
    struct Node
    {
        T value[N];
        Node* node;
    }
    ```
    当分配内存时，一次分配长度为 N 的数组，避免分配大量连续内存。当在做消息生产、消费时，又不需要像链表那样频繁 malloc/free 节点内存，
    因为可以直接操作链表节点上的数组。实际上是在数组和链表之间做了一个折中，以达到较高的pop/push效率。
    当然，如果队列的原始链表使用内存池管理（原始链表 + 内存池），队列的pop/push效率应该也会比单纯的malloc/free要高，当然每次pop/push都会涉及内存池的内存管理操作，
    虽然效率高于 raw malloc/free，但每次队列操作，均会涉及内存池的内存管理操作，而yqueue需要操作 N 次队列才会触发一次内存管理操作，因此 原始链表 + 内存池的效率
    可能仍不及 yqueue 。
- 
 
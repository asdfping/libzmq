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
- io_threads_t. ctx包含的io线程。
- mutex_t _slot_sync。用于同步访问全局 `slot-related`数据：sockets，empty-slots，terminating标记。同时也用于同步访问僵尸sockets，并提供内存屏障用于确保所有的cpu-core能看到
一致的数据。
- slots。i_mail_box 的数组指针，用于业务和I/O线程的信箱。

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
    - 



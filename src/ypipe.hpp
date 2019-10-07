/*
    Copyright (c) 2007-2016 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __ZMQ_YPIPE_HPP_INCLUDED__
#define __ZMQ_YPIPE_HPP_INCLUDED__

#include "atomic_ptr.hpp"
#include "yqueue.hpp"
#include "ypipe_base.hpp"

namespace zmq
{
//  Lock-free queue implementation.
//  Only a single thread can read from the pipe at any specific moment.
//  Only a single thread can write to the pipe at any specific moment.
//  T is the type of the object in the queue.
//  N is granularity of the pipe, i.e. how many items are needed to
//  perform next memory allocation.

template <typename T, int N> class ypipe_t : public ypipe_base_t<T>
{
  public:
    //  Initialises the pipe.
    inline ypipe_t ()
    {
        //  Insert terminator element into the queue.
        // 构造时，队列里添加一个空元素，作为终止标记。
        _queue.push ();

        //  Let all the pointers to point to the terminator.
        //  (unless pipe is dead, in which case c is set to NULL).
        // 相关标记设置为终止标记
        _r = _w = _f = &_queue.back ();
        // 最新的flushed元素，也是终止标记
        _c.set (&_queue.back ());
    }

    //  The destructor doesn't have to be virtual. It is made virtual
    //  just to keep ICC and code checking tools from complaining.
    inline virtual ~ypipe_t () {}

    //  Following function (write) deliberately copies uninitialised data
    //  when used with zmq_msg. Initialising the VSM body for
    //  non-VSM messages won't be good for performance.

#ifdef ZMQ_HAVE_OPENVMS
#pragma message save
#pragma message disable(UNINIT)
#endif

    //  Write an item to the pipe.  Don't flush it yet. If incomplete is
    //  set to true the item is assumed to be continued by items
    //  subsequently written to the pipe. Incomplete items are never
    //  flushed down the stream.
    inline void write (const T &value_, bool incomplete_)
    {
        //  Place the value to the queue, add new terminator element.
        //  队列中最后一个元素设置为 value_
        _queue.back () = value_;
        //  push 新的元素，即 结束元素
        _queue.push ();

        //  Move the "flush up to here" poiter. 
        if (!incomplete_)
        // 如果未完成，则 _f 标记设置为 结束元素
            _f = &_queue.back ();
    }

#ifdef ZMQ_HAVE_OPENVMS
#pragma message restore
#endif

    //  Pop an incomplete item from the pipe. Returns true if such
    //  item exists, false otherwise.
    inline bool unwrite (T *value_)
    {
        if (_f == &_queue.back ())
            return false;
        
        // 去掉 最后一个结束元素
        _queue.unpush ();
        // 再获取倒数第二个元素
        *value_ = _queue.back ();
        return true;
    }

    //_w:变化的时机：flush 时，将 _w 设置为与 _f 一致。初始情况下，_w = _f，如果插入的元素标记为已完成，则 _f 不变，_w = _f；
    //   否则 _f 会更新后新插入元素的尾部，使得 _w != _f
    //_f:变化的时机：插入元素，且未完成时(incomplete)。指向未完成的插入元素的后一个元素
    //_c:变化的时机：flush操作时，cas 操作，将其设置为 _f；check_read 操作，将其设置为 NULL。初始值与 _f/_w 相等
    //_r:变化的时机：check_read ，设置为 queue.front()。初始值与 _f/_w 相等

    //  Flush all the completed items into the pipe. Returns false if
    //  the reader thread is sleeping. In that case, caller is obliged to
    //  wake the reader up before using the pipe again.
    inline bool flush ()
    {
        //  If there are no un-flushed items, do nothing.
        //  flush 操作，检查 un-flushed 是否与 flushed item 相等，相等则说明没有 un-flushed 的元素，无需 flush
        if (_w == _f)
            return true;

        //  Try to set 'c' to 'f'. 
        //  说明有 un-flushed 的 元素
        //  _c(last flushed) cas 操作，如果是 _w，则修改为 _f，
        if (_c.cas (_w, _f) != _w) {
            //  Compare-and-swap was unseccessful because 'c' is NULL.
            //  This means that the reader is asleep. Therefore we don't
            //  care about thread-safeness and update c in non-atomic
            //  manner. We'll return false to let the caller know
            //  that reader is sleeping.
            _c.set (_f);
            _w = _f;
            return false;
        }

        //  Reader is alive. Nothing special to do now. Just move
        //  the 'first un-flushed item' pointer to 'f'.
        _w = _f; //将 un-flushed 修改为 _f
        return true;
    }

    //  Check whether item is available for reading.
    inline bool check_read ()
    {
        //  Was the value prefetched already? If so, return.
        //  队列头部不是 未预读取 的，且未预读取的非空
        if (&_queue.front () != _r && _r)
            return true;

        //  There's no prefetched value, so let us prefetch more values.
        //  Prefetching is to simply retrieve the
        //  pointer from c in atomic fashion. If there are no
        //  items to prefetch, set c to NULL (using compare-and-swap).

        // 未预读取的，设置为 队列头部。
        // 上次 flushed 的 item,设置为 NULL
        _r = _c.cas (&_queue.front (), NULL);

        //  If there are no elements prefetched, exit.
        //  During pipe's lifetime r should never be NULL, however,
        //  it can happen during pipe shutdown when items
        //  are being deallocated.
        if (&_queue.front () == _r || !_r)
            return false;

        //  There was at least one value prefetched.
        return true;
    }

    //  Reads an item from the pipe. Returns false if there is no value.
    //  available.
    inline bool read (T *value_)
    {
        //  Try to prefetch a value.
        if (!check_read ())
            return false;

        //  There was at least one value prefetched.
        //  Return it to the caller.

        //  从队列头部读取元素
        *value_ = _queue.front ();

        //读取后，弹出头部元素
        _queue.pop ();
        return true;
    }

    //  Applies the function fn to the first elemenent in the pipe
    //  and returns the value returned by the fn.
    //  The pipe mustn't be empty or the function crashes.
    inline bool probe (bool (*fn_) (const T &))
    {
        bool rc = check_read ();
        zmq_assert (rc);

        return (*fn_) (_queue.front ());
    }

  protected:
    //  Allocation-efficient queue to store pipe items.
    //  Front of the queue points to the first prefetched item, back of
    //  the pipe points to last un-flushed item. Front is used only by
    //  reader thread, while back is used only by writer thread.
    yqueue_t<T, N> _queue;

    //  Points to the first un-flushed item. This variable is used
    //  exclusively by writer thread.
    T *_w; // 第一个 un-flushed 的元素

    //  Points to the first un-prefetched item. This variable is used
    //  exclusively by reader thread.
    T *_r; // 第一个 un-prefetched 的元素

    //  Points to the first item to be flushed in the future.
    T *_f; // 第一个将被 flushed 的元素

    //  The single point of contention between writer and reader thread.
    //  Points past the last flushed item. If it is NULL,
    //  reader is asleep. This pointer should be always accessed using
    //  atomic operations.
    atomic_ptr_t<T> _c; // 读线程和写线程竞争的资源。指向最新的flushed的元素，若为NULL，reader 线程 asleep。

    //  Disable copying of ypipe object.
    ypipe_t (const ypipe_t &);
    const ypipe_t &operator= (const ypipe_t &);
};
}

#endif

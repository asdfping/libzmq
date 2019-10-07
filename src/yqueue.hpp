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

#ifndef __ZMQ_YQUEUE_HPP_INCLUDED__
#define __ZMQ_YQUEUE_HPP_INCLUDED__

#include <stdlib.h>
#include <stddef.h>

#include "err.hpp"
#include "atomic_ptr.hpp"

namespace zmq
{
//  yqueue is an efficient queue implementation. The main goal is
//  to minimise number of allocations/deallocations needed. Thus yqueue
//  allocates/deallocates elements in batches of N.
//
//  yqueue allows one thread to use push/back function and another one
//  to use pop/front functions. However, user must ensure that there's no
//  pop on the empty queue and that both threads don't access the same
//  element in unsynchronised manner.
//
//  T is the type of the object in the queue.
//  N is granularity of the queue (how many pushes have to be done till
//  actual memory allocation is required).
#ifdef HAVE_POSIX_MEMALIGN
// ALIGN is the memory alignment size to use in the case where we have
// posix_memalign available. Default value is 64, this alignment will
// prevent two queue chunks from occupying the same CPU cache line on
// architectures where cache lines are <= 64 bytes (e.g. most things
// except POWER). It is detected at build time to try to account for other
// platforms like POWER and s390x.
template <typename T, int N, size_t ALIGN = ZMQ_CACHELINE_SIZE> class yqueue_t
#else

// T 表示队列中对象的类型，N表示一个chunk中可容纳的对象数量
template <typename T, int N> class yqueue_t
#endif
{
  public:
    //  Create the queue.
    inline yqueue_t ()
    {
        _begin_chunk = allocate_chunk ();
        alloc_assert (_begin_chunk);
        _begin_pos = 0;
        _back_chunk = NULL;
        _back_pos = 0;
        _end_chunk = _begin_chunk;
        _end_pos = 0;
    }

    //  Destroy the queue.
    inline ~yqueue_t ()
    {
        while (true) {
            if (_begin_chunk == _end_chunk) {
                free (_begin_chunk);
                break;
            }
            chunk_t *o = _begin_chunk;
            _begin_chunk = _begin_chunk->next;
            free (o);
        }

        chunk_t *sc = _spare_chunk.xchg (NULL);
        free (sc);
    }

    //  Returns reference to the front element of the queue.
    //  If the queue is empty, behaviour is undefined.
    inline T &front () { return _begin_chunk->values[_begin_pos]; }

    //  Returns reference to the back element of the queue.
    //  If the queue is empty, behaviour is undefined.
    inline T &back () { return _back_chunk->values[_back_pos]; }

    //  Adds an element to the back end of the queue.
    //  push，从尾部添加元素
    inline void push ()
    {
        //_back = _end
        _back_chunk = _end_chunk;
        _back_pos = _end_pos;

        // end_pos 不是 最后一个，则返回
        if (++_end_pos != N)
            return;

        // end_pos 是最后一个，需要新的 chunk
        // 这里使用 atomic_ptr_t，当有2个线程试图 push，获取 _spare_chunk 时，只有一个线程能获取到有效的
        // 内存指针，另一个线程只能获取到 NULL，避免2个线程操作同一个chunk。
        chunk_t *sc = _spare_chunk.xchg (NULL);
        if (sc) {
            // 新的chunk 从 _spare_chunk 中获取，避免 malloc 内存
            _end_chunk->next = sc;
            sc->prev = _end_chunk;
        } else {
            // spare中没有，则 allocate 一块新内存
            _end_chunk->next = allocate_chunk ();
            alloc_assert (_end_chunk->next);
            _end_chunk->next->prev = _end_chunk;
        }

        //更新 _end 的信息
        _end_chunk = _end_chunk->next;
        _end_pos = 0;
    }

    //  Removes element from the back end of the queue. In other words
    //  it rollbacks last push to the queue. Take care: Caller is
    //  responsible for destroying the object being unpushed.
    //  The caller must also guarantee that the queue isn't empty when
    //  unpush is called. It cannot be done automatically as the read
    //  side of the queue can be managed by different, completely
    //  unsynchronised thread.

    //unpush，从尾部删除元素
    inline void unpush ()
    {
        //  First, move 'back' one position backwards.

        // 修改 _back 的信息
        if (_back_pos)
            --_back_pos;
        else {
            _back_pos = N - 1;
            _back_chunk = _back_chunk->prev;
        }

        //  Now, move 'end' position backwards. Note that obsolete end chunk
        //  is not used as a spare chunk. The analysis shows that doing so
        //  would require free and atomic operation per chunk deallocated
        //  instead of a simple free.

        //修改 _end 的信息
        if (_end_pos)
            --_end_pos;
        else {
            _end_pos = N - 1;
            _end_chunk = _end_chunk->prev;
            free (_end_chunk->next);
            _end_chunk->next = NULL;
        }
    }

    //  Removes an element from the front end of the queue.
    // pop，从头部删除元素
    inline void pop ()
    {
        if (++_begin_pos == N) {
            //如果头部 chunk 完全被删除，则头部 chunk 被保存到 _spare_chunk 中备用，而不是立刻被free
            //如果 pop 和 push 的速率一致，那么因 pop 释放的 chunk，先由 _spare_chunk保存，随后被用于
            //push 操作，添加新的chunk。当 pop 完毕时，push恰好使得尾部的 chunk 用满，那么头部空闲的空间
            //就可用于尾部push操作，添加新chunk。避免不必要的 malloc/free。
            chunk_t *o = _begin_chunk;
            _begin_chunk = _begin_chunk->next;
            _begin_chunk->prev = NULL;
            _begin_pos = 0;

            //  'o' has been more recently used than _spare_chunk,
            //  so for cache reasons we'll get rid of the spare and
            //  use 'o' as the spare.
            chunk_t *cs = _spare_chunk.xchg (o);
            free (cs);
        }
    }

  private:
    //  Individual memory chunk to hold N elements.
    struct chunk_t
    {
        T values[N];
        chunk_t *prev;
        chunk_t *next;
    };

    inline chunk_t *allocate_chunk ()
    {
#ifdef HAVE_POSIX_MEMALIGN
        void *pv;
        if (posix_memalign (&pv, ALIGN, sizeof (chunk_t)) == 0)
            return (chunk_t *) pv;
        return NULL;
#else
        return (chunk_t *) malloc (sizeof (chunk_t));
#endif
    }

    //  Back position may point to invalid memory if the queue is empty,
    //  while begin & end positions are always valid. Begin position is
    //  accessed exclusively be queue reader (front/pop), while back and
    //  end positions are accessed exclusively by queue writer (back/push).

    //begin的位置始终有效 -> 读取数据的位置（从头部读取）
    chunk_t *_begin_chunk;
    int _begin_pos;

    //back会指向无效内存，当队列为空时 -> 写入数据的位置（从尾部push）
    chunk_t *_back_chunk;
    int _back_pos;

    //end的位置始终有效，指向队列有效数据的后一个位置
    chunk_t *_end_chunk;
    int _end_pos;

    /**
     * begin、back、end 的关系如下
     * 
     *  +------+------+------+-----+
     *   begin          back   end
     * 
     *  队列第一个元素：begin
     *  队列最后一个元素: back
     *  队列尾部：end
     * 
     *  当 begin == end 时，表示队列为空。上述关系，无论用在 chunk，还是 pos 都是成立的。
     *  试想上述示意图是 chunk，那么在不断 pop 元素时， begin会先和 back相等，此时队列只有
     *  一个chunk。back和begin都有效，但接着 begin = begin->next，此时begin = end，而 begin
     *  原先的位置，也就是 back 已经被 free，它是无效的，此时队列是空的。
     */

    //  People are likely to produce and consume at similar rates.  In
    //  this scenario holding onto the most recently freed chunk saves
    //  us from having to call malloc/free.

    // 消息的消费和生产速率接近。在这种场景下，保存最近释放的 chunks，可以减少 malloc/free 的调用。
    atomic_ptr_t<chunk_t> _spare_chunk;

    //  Disable copying of yqueue.
    yqueue_t (const yqueue_t &);
    const yqueue_t &operator= (const yqueue_t &);
};
}

#endif

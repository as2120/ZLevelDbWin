﻿// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "leveldb/cache.h"
#include "port/port.h"
#include "util/hash.h"
#include "util/mutexlock.h"

namespace leveldb
{

Cache::~Cache()
{
}

namespace
{

// LRU cache implementation

//z 一个entry是变长的，在heap中分配的。
//z entry 保存在一个双向链表环中，由访问时间排序。
// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.
struct LRUHandle
{
    void* value;
    //z 处理删除的函数指针
    void (*deleter)(const Slice&, void* value);//z 删除函数
    LRUHandle* next_hash;//z 为啥存储一个 next_hash 了？value的hash值可能有重复的？
    //z 双向链表
    LRUHandle* next;
    LRUHandle* prev;
    size_t charge;      // TODO(opt): Only allow uint32_t?
    size_t key_length;
    uint32_t refs;
    //z hash值，用于快速 sharding 和比较
    uint32_t hash;      // Hash of key(); used for fast sharding and comparisons
    //z 存放key起始的地方
    //z 给定一个1，什么意思了？
    char key_data[1];   // Beginning of key

    //z 返回key
    Slice key() const
    {
        // For cheaper lookups, we allow a temporary Handle object
        // to store a pointer to a key in "value".
        //z 这
        if (next == this)
        {
            return *(reinterpret_cast<Slice*>(value));
        }
        else
        {
            return Slice(key_data, key_length);
        }
    }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
class HandleTable
{
public:
    HandleTable() : length_(0), elems_(0), list_(NULL)
    {
        Resize();
    }
    ~HandleTable()
    {
        delete[] list_;
    }

    LRUHandle* Lookup(const Slice& key, uint32_t hash)
    {
        return *FindPointer(key, hash);
    }

    LRUHandle* Insert(LRUHandle* h)
    {
        LRUHandle** ptr = FindPointer(h->key(), h->hash);
        LRUHandle* old = *ptr;
        h->next_hash = (old == NULL ? NULL : old->next_hash);
        *ptr = h;
        if (old == NULL)
        {
            ++elems_;
            if (elems_ > length_)
            {
                // Since each cache entry is fairly large, we aim for a small
                // average linked list length (<= 1).
                Resize();
            }
        }
        return old;
    }

    LRUHandle* Remove(const Slice& key, uint32_t hash)
    {
        //z 找到其在list中的位置
        LRUHandle** ptr = FindPointer(key, hash);
        LRUHandle* result = *ptr;
        //z 考虑next_hash的存在，将链都删除
        if (result != NULL)
        {
            *ptr = result->next_hash;
            --elems_;
        }
        //z 返回其位置
        return result;
    }

private:
    // The table consists of an array of buckets where each bucket is
    // a linked list of cache entries that hash into the bucket.
    //z table 包含了一个buckets队列。
    //z 每个 bucket 都是cache entries的一个linked list，hash而来。
    uint32_t length_;//z 这个是 capacity ？
    uint32_t elems_;//z 当前实际有多少元素？
    LRUHandle** list_;//z 存储

    // Return a pointer to slot that points to a cache entry that
    // matches key/hash.  If there is no such cache entry, return a
    // pointer to the trailing slot in the corresponding linked list.
    LRUHandle** FindPointer(const Slice& key, uint32_t hash)
    {
        //z 根据hash值访问list_中的element。
        //z 看值的情况，除了在 shard_ 时用到了hash，后续继续用到 hash 值。
        //z 返回 list_ 所对应的情况
        //z 假设 length_ 为 2^n ， 比如64，减1之后形式就是 111111。
        //z 使用 hash 来 &， 可能会出现值有冲突的情况的么？
        LRUHandle** ptr = &list_[hash & (length_ - 1)];

        //z ptr 中的值不为空，说明该hash已经被占位。那么循环至next_hash链结尾，
        //z 按说由 hash 得出的这个位置，由next_hash所指向的位置hash值应该都是一样，这里为何还要加以比较了？
        while (*ptr != NULL &&
                ((*ptr)->hash != hash || key != (*ptr)->key()))
        {
            ptr = &(*ptr)->next_hash;
        }

        //z 返回找到的，可能为NULL。
        return ptr;
    }

    //z 扩容。
    void Resize()
    {
        uint32_t new_length = 4;
        //z 根据容量，找到一个不小于 elems_ 的最小的 2 的幂次方。
        while (new_length < elems_)
        {
            new_length *= 2;
        }
        //z 申请新的空间
        LRUHandle** new_list = new LRUHandle*[new_length];
        //z 初始化为0。
        memset(new_list, 0, sizeof(new_list[0]) * new_length);
        uint32_t count = 0;
        //z 将原内容移动到新的链表中去。不知道next、prev之类的指针是如何加以维护的
        for (uint32_t i = 0; i < length_; i++)
        {
            LRUHandle* h = list_[i];
            //z 如果为NULL，不用拷贝过去；不为null，移动到新的list中去。
            while (h != NULL)
            {
                //z 记录下一个指针
                LRUHandle* next = h->next_hash;
                Slice key = h->key();
                uint32_t hash = h->hash;
                //z 取新的list中位置
                LRUHandle** ptr = &new_list[hash & (new_length - 1)];
                //z 将 h->next_hash 指向该位置
                //z 将h放到next_hash链表的表头；
                h->next_hash = *ptr;
                //z 然后ptr指向h。
                *ptr = h;
                //z 再拷贝 ptr linked list 中的下一个
                h = next;
                count++;
            }
        }
        assert(elems_ == count);
        //z 删除原来的list
        delete[] list_;
        //z 赋予新的指针和长度
        list_ = new_list;
        length_ = new_length;
    }
};

//z 分片cache的一个分片
// A single shard of sharded cache.
class LRUCache
{
public:
    LRUCache();
    ~LRUCache();

    // Separate from constructor so caller can easily make an array of LRUCache
    //z 设置 capacity
    void SetCapacity(size_t capacity)
    {
        capacity_ = capacity;
    }

    // Like Cache methods, but with an extra "hash" parameter.
    Cache::Handle* Insert(const Slice& key, uint32_t hash,
                          void* value, size_t charge,
                          void (*deleter)(const Slice& key, void* value));
    Cache::Handle* Lookup(const Slice& key, uint32_t hash);
    void Release(Cache::Handle* handle);
    void Erase(const Slice& key, uint32_t hash);

private:
    void LRU_Remove(LRUHandle* e);
    void LRU_Append(LRUHandle* e);
    void Unref(LRUHandle* e);

    // Initialized before use.
    size_t capacity_;

    // mutex_ protects the following state.
    port::Mutex mutex_;
    size_t usage_;
    uint64_t last_id_;

    // Dummy head of LRU list.
    // lru.prev is newest entry, lru.next is oldest entry.
    //z lru_.prev 总是最新的，lru.next是最老的 entry。
    LRUHandle lru_;

    HandleTable table_;
};

LRUCache::LRUCache()
    : usage_(0),
      last_id_(0)
{
    // Make empty circular linked list
    lru_.next = &lru_;
    lru_.prev = &lru_;
}

LRUCache::~LRUCache()
{
    for (LRUHandle* e = lru_.next; e != &lru_; )
    {
        LRUHandle* next = e->next;
        assert(e->refs == 1);  // Error if caller has an unreleased handle
        Unref(e);
        e = next;
    }
}

void LRUCache::Unref(LRUHandle* e)
{
    assert(e->refs > 0);
    e->refs--;
    if (e->refs <= 0)
    {
        usage_ -= e->charge;
        //z 保存了函数指针，使用此函数指针来进行释放方面的操作。
        (*e->deleter)(e->key(), e->value);
        free(e);
    }
}

void LRUCache::LRU_Remove(LRUHandle* e)
{
    e->next->prev = e->prev;
    e->prev->next = e->next;
}

void LRUCache::LRU_Append(LRUHandle* e)
{
    // Make "e" newest entry by inserting just before lru_
    //z 最新的节点总是插入在 lru_ 之前
    e->next = &lru_;
    e->prev = lru_.prev;
    e->prev->next = e;
    e->next->prev = e;
}

Cache::Handle* LRUCache::Loovkup(const Slice& key, uint32_t hash)
{
    //z 这样的话，在查找的时候只用锁定16中的一个，而不影响其他shard_的操作
    MutexLock l(&mutex_);
    //z 在 table 中查找
    LRUHandle* e = table_.Lookup(key, hash);
    if (e != NULL)
    {
        e->refs++;
        LRU_Remove(e);
        LRU_Append(e);
    }
    return reinterpret_cast<Cache::Handle*>(e);
}

void LRUCache::Release(Cache::Handle* handle)
{
    MutexLock l(&mutex_);
    Unref(reinterpret_cast<LRUHandle*>(handle));
}

Cache::Handle* LRUCache::Insert(
    const Slice& key, uint32_t hash, void* value, size_t charge,
    void (*deleter)(const Slice& key, void* value))
{
    MutexLock l(&mutex_);

    //z 新分配一个 LRUHandle 结构
    //z 注意这里，似乎是利用了C结构的布局
    //z e->key_data 已经占用了一个空间了，接着该如何处理了？
    LRUHandle* e = reinterpret_cast<LRUHandle*>(
                       malloc(sizeof(LRUHandle)-1 + key.size()));

    //z 初始化，key是直接存在这里的，value存在外部的
    e->value = value;
    e->deleter = deleter;
    e->charge = charge;
    e->key_length = key.size();
    e->hash = hash;
    //z 引用计数
    e->refs = 2;  // One from LRUCache, one for the returned handle
    //z 拷贝 key 的值
    memcpy(e->key_data, key.data(), key.size());
    LRU_Append(e);
    usage_ += charge;

    LRUHandle* old = table_.Insert(e);
    if (old != NULL)
    {
        LRU_Remove(old);
        Unref(old);
    }

    while (usage_ > capacity_ && lru_.next != &lru_)
    {
        LRUHandle* old = lru_.next;
        LRU_Remove(old);
        table_.Remove(old->key(), old->hash);
        Unref(old);
    }

    return reinterpret_cast<Cache::Handle*>(e);
}

void LRUCache::Erase(const Slice& key, uint32_t hash)
{
    MutexLock l(&mutex_);
    LRUHandle* e = table_.Remove(key, hash);
    if (e != NULL)
    {
        LRU_Remove(e);
        Unref(e);
    }
}

static const int kNumShardBits = 4;
static const int kNumShards = 1 << kNumShardBits;

class ShardedLRUCache : public Cache
{
private:
    //z 一共有16个 LRUCache，插入的时候，会根据key的hash值的前四位插入到不同的shard中去
    //z 由于只取其前四位，得到的值的范围为 0-15，不知道hash出来的值是否均匀分布
    LRUCache shard_[kNumShards];
    port::Mutex id_mutex_;
    uint64_t last_id_;

    static inline uint32_t HashSlice(const Slice& s)
    {
        return Hash(s.data(), s.size(), 0);
    }

    //z 根据hash值的前四位，放入不同的桶中
    static uint32_t Shard(uint32_t hash)
    {
        return hash >> (32 - kNumShardBits);
    }

public:
    explicit ShardedLRUCache(size_t capacity)
        : last_id_(0)
    {
        const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
        //z 对 16 个 shard_ 均设置容量
        for (int s = 0; s < kNumShards; s++)
        {
            shard_[s].SetCapacity(per_shard);
        }
    }
    virtual ~ShardedLRUCache() { }
    virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                           void (*deleter)(const Slice& key, void* value))
    {
        //z 由 key 计算得到一个 hash 值
        const uint32_t hash = HashSlice(key);
        //z 根据 hash 值插入到不同的 shard_ 中去。
        //z charge 用于什么目的了？
        return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
    }
    virtual Handle* Lookup(const Slice& key)
    {
        //z 计算hash值
        const uint32_t hash = HashSlice(key);
        //z 到对应的 shard_ 中去查找
        return shard_[Shard(hash)].Lookup(key, hash);
    }
    virtual void Release(Handle* handle)
    {
        LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
        shard_[Shard(h->hash)].Release(handle);
    }
    virtual void Erase(const Slice& key)
    {
        const uint32_t hash = HashSlice(key);
        shard_[Shard(hash)].Erase(key, hash);
    }
    virtual void* Value(Handle* handle)
    {
        return reinterpret_cast<LRUHandle*>(handle)->value;
    }
    virtual uint64_t NewId()
    {
        MutexLock l(&id_mutex_);
        return ++(last_id_);
    }
};

}  // end anonymous namespace

Cache* NewLRUCache(size_t capacity)
{
    return new ShardedLRUCache(capacity);
}

}

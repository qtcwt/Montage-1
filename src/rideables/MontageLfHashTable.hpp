#ifndef MONTAGE_LF_HASHTABLE_P
#define MONTAGE_LF_HASHTABLE_P

#include <stdio.h>
#include <stdlib.h>

#include <iostream>
#include <atomic>
#include <algorithm>
#include <functional>
#include <vector>
#include <utility>

#include "HarnessUtils.hpp"
#include "ConcurrentPrimitives.hpp"
#include "RMap.hpp"
#include "RCUTracker.hpp"
#include "CustomTypes.hpp"

template <class K, class V>
class MontageLfHashTable : public RMap<K,V>{
public:
    class Payload : public PBlk{
        GENERATE_FIELD(K, key, Payload);
        GENERATE_FIELD(V, val, Payload);
    public:
        Payload(){}
        Payload(K x, V y): m_key(x), m_val(y){}
        // Payload(const Payload& oth): PBlk(oth), m_key(oth.m_key), m_val(oth.m_val){}
        void persist(){}
    };
private:
    struct Node;

    struct MarkPtr{
        std::atomic<Node*> ptr;
        MarkPtr(Node* n):ptr(n){};
        MarkPtr():ptr(nullptr){};
    };

    struct Node{
        K key;
        MarkPtr next;
        Payload* payload;// TODO: does it have to be atomic?
        Node(K k, V v, Node* n):key(k),next(n),payload(PNEW(Payload,k,v)){};
        ~Node(){
            PRECLAIM(payload);
        }

        void rm_payload(){
            // call it before END_OP but after linearization point
            assert(payload!=nullptr && "payload shouldn't be null");
            PRETIRE(payload);
        }
        V get_val(){
            // call it within BEGIN_OP and END_OP
            assert(payload!=nullptr && "payload shouldn't be null");
            return (V)payload->get_val();
        }
    };
    std::hash<K> hash_fn;
    const int idxSize=1000000;//number of buckets for hash table
    padded<MarkPtr>* buckets=new padded<MarkPtr>[idxSize]{};
    bool findNode(MarkPtr* &prev, Node* &curr, Node* &next, K key, int tid);

    RCUTracker<Node> tracker;

    const uint64_t MARK_MASK = ~0x1;
    inline Node* getPtr(Node* mptr){
        return (Node*) ((uint64_t)mptr & MARK_MASK);
    }
    inline bool getMark(Node* mptr){
        return (bool)((uint64_t)mptr & 1);
    }
    inline Node* mixPtrMark(Node* ptr, bool mk){
        return (Node*) ((uint64_t)ptr | mk);
    }
    inline Node* setMark(Node* mptr){
        return mixPtrMark(mptr,true);
    }
public:
    MontageLfHashTable(int task_num) : tracker(task_num, 100, 1000, true) {};
    ~MontageLfHashTable(){};

    optional<V> get(K key, int tid);
    optional<V> put(K key, V val, int tid);
    bool insert(K key, V val, int tid);
    optional<V> remove(K key, int tid);
    optional<V> replace(K key, V val, int tid);
};

template <class T> 
class MontageLfHashTableFactory : public RideableFactory{
    Rideable* build(GlobalTestConfig* gtc){
        return new MontageLfHashTable<T,T>(gtc->task_num);
    }
};


//-------Definition----------
template <class K, class V> 
optional<V> MontageLfHashTable<K,V>::get(K key, int tid) {
    optional<V> res={};
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;

    tracker.start_op(tid);
    // hold epoch from advancing so that the node we find won't be deleted
    if(findNode(prev,curr,next,key,tid)) {
        BEGIN_OP_AUTOEND();
        res=curr->get_val();//never old see new as we find node before BEGIN_OP
    }
    tracker.end_op(tid);

    return res;
}

template <class K, class V> 
optional<V> MontageLfHashTable<K,V>::put(K key, V val, int tid) {
    optional<V> res={};
    Node* tmpNode = nullptr;
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;
    tmpNode = new Node(key, val, nullptr);

    tracker.start_op(tid);
    while(true) {
        if(findNode(prev,curr,next,key,tid)) {
            // exists; replace
            tmpNode->next.ptr.store(curr);
            BEGIN_OP(tmpNode->payload);
            res=curr->get_val();
            if(prev->ptr.compare_exchange_strong(curr,tmpNode)) {
                curr->rm_payload();
                END_OP;
                // mark curr; since findNode only finds the first node >= key, it's ok to have duplicated keys temporarily
                while(!curr->next.ptr.compare_exchange_strong(next,setMark(next)));
                if(tmpNode->next.ptr.compare_exchange_strong(curr,next)) {
                    tracker.retire(curr,tid);
                } else {
                    findNode(prev,curr,next,key,tid);
                }
                break;
            }
            ABORT_OP;
        }
        else {
            //does not exist; insert.
            res={};
            tmpNode->next.ptr.store(curr);
            BEGIN_OP(tmpNode->payload);
            if(prev->ptr.compare_exchange_strong(curr,tmpNode)) {
                END_OP;
                break;
            }
            ABORT_OP;
        }
    }
    tracker.end_op(tid);
    // assert(0&&"put isn't implemented");
    return res;
}

template <class K, class V> 
bool MontageLfHashTable<K,V>::insert(K key, V val, int tid){
    bool res=false;
    Node* tmpNode = nullptr;
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;
    tmpNode = new Node(key, val, nullptr);

    tracker.start_op(tid);
    while(true) {
        if(findNode(prev,curr,next,key,tid)) {
            res=false;
            delete tmpNode;
            break;
        }
        else {
            //does not exist, insert.
            tmpNode->next.ptr.store(curr);
            BEGIN_OP(tmpNode->payload);
            if(prev->ptr.compare_exchange_strong(curr,tmpNode)) {
                END_OP;
                res=true;
                break;
            }
            ABORT_OP;
        }
    }
    tracker.end_op(tid);

    return res;
}

template <class K, class V> 
optional<V> MontageLfHashTable<K,V>::remove(K key, int tid) {
    optional<V> res={};
    MarkPtr* prev=nullptr;
    Node* curr=nullptr;
    Node* next=nullptr;

    tracker.start_op(tid);
    while(true) {
        if(!findNode(prev,curr,next,key,tid)) {
            res={};
            break;
        }
        BEGIN_OP();
        res=curr->get_val();
        if(!curr->next.ptr.compare_exchange_strong(next,setMark(next))) {
            ABORT_OP;
            continue;
        }
        curr->rm_payload();
        END_OP;
        if(prev->ptr.compare_exchange_strong(curr,next)) {
            tracker.retire(curr,tid);
        } else {
            findNode(prev,curr,next,key,tid);
        }
        break;
    }
    tracker.end_op(tid);

    return res;
}

template <class K, class V> 
optional<V> MontageLfHashTable<K,V>::replace(K key, V val, int tid) {
    optional<V> res={};
    // Node* tmpNode = nullptr;
    // MarkPtr* prev=nullptr;
    // Node* curr=nullptr;
    // Node* next=nullptr;
    // tmpNode = new Node(key, val, nullptr);

    // tracker.start_op(tid);
    // while(true){
    //     if(findNode(prev,curr,next,key,tid)){
    //         tmpNode->next.ptr.store(curr);
    //         BEGIN_OP(tmpNode->payload);
    //         res=curr->get_val();
    //         if(prev->ptr.compare_exchange_strong(curr,tmpNode)){
    //             curr->rm_payload();
    //             END_OP;
    //             // mark curr; since findNode only finds the first node >= key, it's ok to have duplicated keys temporarily
    //             while(!curr->next.ptr.compare_exchange_strong(next,setMark(next)));
    //             if(tmpNode->next.ptr.compare_exchange_strong(curr,next)) {
    //                 tracker.retire(curr,tid);
    //             } else {
    //                 findNode(prev,curr,next,key,tid);
    //             }
    //             break;
    //         }
    //         ABORT_OP;
    //     }
    //     else{//does not exist
    //         res={};
    //         delete tmpNode;
    //         break;
    //     }
    // }
    // tracker.end_op(tid);
    assert(0&&"replace isn't implemented");
    return res;
}

template <class K, class V> 
bool MontageLfHashTable<K,V>::findNode(MarkPtr* &prev, Node* &curr, Node* &next, K key, int tid){
    while(true){
        size_t idx=hash_fn(key)%idxSize;
        bool cmark=false;
        prev=&buckets[idx].ui;
        curr=getPtr(prev->ptr.load());

        while(true){//to lock old and curr
            if(curr==nullptr) return false;
            next=curr->next.ptr.load();
            cmark=getMark(next);
            next=getPtr(next);
            auto ckey=curr->key;
            if(prev->ptr.load()!=curr) break;//retry
            if(!cmark) {
                if(ckey>=key) return ckey==key;
                prev=&(curr->next);
            } else {
                if(prev->ptr.compare_exchange_strong(curr,next)) {
                    tracker.retire(curr,tid);
                } else {
                    break;//retry
                }
            }
            curr=next;
        }
    }
}

/* Specialization for strings */
#include <string>
#include "PString.hpp"
template <>
class MontageLfHashTable<std::string, std::string>::Payload : public PBlk{
    GENERATE_FIELD(PString<TESTS_KEY_SIZE>, key, Payload);
    GENERATE_FIELD(PString<TESTS_VAL_SIZE>, val, Payload);

public:
    Payload(std::string k, std::string v) : m_key(this, k), m_val(this, v){}
    void persist(){}
};

#endif

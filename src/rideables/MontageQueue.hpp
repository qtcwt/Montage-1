#ifndef MONTAGE_QUEUE_P
#define MONTAGE_QUEUE_P

#include <iostream>
#include <atomic>
#include <algorithm>
#include "HarnessUtils.hpp"
#include "ConcurrentPrimitives.hpp"
#include "RQueue.hpp"
#include "RCUTracker.hpp"
#include "CustomTypes.hpp"
#include "persist_struct_api.hpp"
#include <mutex>

using namespace pds;

template<typename T>
class MontageQueue : public RQueue<T>{
public:
    class Payload : public PBlk{
        GENERATE_FIELD(T, val, Payload);
        GENERATE_FIELD(uint64_t, sn, Payload); 
    public:
        Payload(){}
        Payload(T v, uint64_t n): m_val(v), m_sn(n){}
        Payload(const Payload& oth): PBlk(oth), m_sn(0), m_val(oth.m_val){}
        void persist(){}
    };

private:
    struct Node{
        Node* next;
        Payload* payload;
        T val; // for debug purpose

        Node(): next(nullptr), payload(nullptr){}; 
        // Node(): next(nullptr){}; 
        Node(T v, uint64_t n=0): next(nullptr), payload(PNEW(Payload, v, n)), val(v){};
        // Node(T v, uint64_t n): next(nullptr), val(v){};

        void set_sn(uint64_t s){
            assert(payload!=nullptr && "payload shouldn't be null");
            payload->set_unsafe_sn(s);
        }
        T get_val(){
            assert(payload!=nullptr && "payload shouldn't be null");
            // old-see-new never happens for locking ds
            return (T)payload->get_unsafe_val();
            // return val;
        }
        ~Node(){
            PDELETE(payload);
        }
    };

public:
    uint64_t global_sn;

private:
    // dequeue pops node from head
    Node* head;
    // enqueue pushes node to tail
    Node* tail;
    std::mutex lock;

public:
    MontageQueue(int task_num): 
        global_sn(0), head(nullptr), tail(nullptr){
    }

    ~MontageQueue(){};

    void enqueue(T val, int tid);
    optional<T> dequeue(int tid);
};

template<typename T>
void MontageQueue<T>::enqueue(T val, int tid){
    Node* new_node = new Node(val);
    std::lock_guard<std::mutex> lk(lock);
    // no read or write so impossible to have old see new exception
    new_node->set_sn(global_sn);
    global_sn++;
    BEGIN_OP_AUTOEND(new_node->payload);
    if(tail == nullptr) {
        head = tail = new_node;
        return;
    }
    tail->next = new_node;
    tail = new_node;
}

template<typename T>
optional<T> MontageQueue<T>::dequeue(int tid){
    optional<T> res = {};
    // while(true){
    lock.lock();
    BEGIN_OP_AUTOEND();
    // try {
    if(head == nullptr) {
        lock.unlock();
        return res;
    }
    Node* tmp = head;
    res = tmp->get_val();
    head = head->next;
    if(head == nullptr) {
        tail = nullptr;
    }
    lock.unlock();
    delete(tmp);
    return res;
    //     } catch (OldSeeNewException& e) {
    //         continue;
    //     }
    // }
}

template <class T> 
class MontageQueueFactory : public RideableFactory{
    Rideable* build(GlobalTestConfig* gtc){
        return new MontageQueue<T>(gtc->task_num);
    }
};

/* Specialization for strings */
#include <string>
#include "PString.hpp"
template <>
class MontageQueue<std::string>::Payload : public PBlk{
    GENERATE_FIELD(PString<TESTS_VAL_SIZE>, val, Payload);
    GENERATE_FIELD(uint64_t, sn, Payload);

public:
    Payload(std::string v, uint64_t n) : m_val(this, v), m_sn(n){}
    void persist(){}
};

#endif

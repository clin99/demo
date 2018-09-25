// 2018/09/21 - modified by Tsung-Wei and Chun-Xun
//   - refactored the code
//  
// TODO:
//   - Problems can occur when external threads insert tasks during spawn.
//
// 2018/09/12 - created by Tsung-Wei Huang and Chun-Xun Lin
//
// Implemented PrivatizedThreadpool using the data structre inspired
// Eigen CXX/Threadpool.

#pragma once

#include <iostream>
#include <functional>
#include <vector>
#include <mutex>
#include <deque>
#include <thread>
#include <stdexcept>
#include <condition_variable>
#include <memory>
#include <future>
#include <optional>
#include <unordered_set>
#include <unordered_map>

#include "move_on_copy.hpp"

namespace tf {

// ---------------------------------------------------------------------------- 

template <typename T, unsigned N>
class RunQueue {

  static_assert((N & (N - 1)) == 0, "N must be power of two");
  static_assert(N > 2, "N must be larger than two");
  
  constexpr static unsigned IDX_MASK = N - 1;
  constexpr static unsigned POS_MASK = (N << 1) - 1;

  struct Entry {
    std::atomic<uint8_t> state;
    T w;
  };

  enum : uint8_t {
    EMPTY,
    BUSY,
    READY
  };

  public:

    RunQueue();
    
    bool push_back(T&&);
    bool push_front(T&&);

    bool push_front(T&);
    bool pop_front(T&);
    bool push_back(T&);
    bool pop_back(T&);
    bool empty() const;

  private:

    std::mutex _mutex;

    std::atomic<unsigned> _front;
    std::atomic<unsigned> _back;


    std::atomic_flag _lock = ATOMIC_FLAG_INIT;

    Entry _array[N];
};

// Constructor    
template <typename T, unsigned N>
RunQueue<T, N>::RunQueue() {

  _front.store(0, std::memory_order_relaxed);
  _back.store(0, std::memory_order_relaxed);

  for(unsigned i=0; i<N; ++i) {
    _array[i].state.store(EMPTY, std::memory_order_relaxed);
  }
}

// insert item w to the beginning of the queue and return true if inserted 
// or false otherwise.
// this function can only be called by the owner thread.
template <typename T, unsigned N>
bool RunQueue<T, N>::push_front(T& w) {

  auto front = _front.load(std::memory_order_relaxed);
  auto& item = _array[front & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);
  
  if(state != EMPTY || 
    !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    return false;
  }

  _front.store((front + 1) & POS_MASK, std::memory_order_relaxed);
  item.w = std::move(w);
  item.state.store(READY, std::memory_order_release);

  return true;
}

template <typename T, unsigned N>
bool RunQueue<T, N>::push_front(T&& w) {
  return push_front(w);
}

// pop the first item out of the queue and store it to w
template <typename T, unsigned N>
bool RunQueue<T, N>::pop_front(T& w) {

  if(empty()) {
    return false;
  }

  auto front = _front.load(std::memory_order_relaxed);
  auto& item = _array[(front - 1) & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);

  if(state != READY || 
    !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    return false;
  }
  
  _front.store((front - 1) & POS_MASK, std::memory_order_relaxed);
  w = std::move(item.w); 
  item.state.store(EMPTY, std::memory_order_release);

  return true;
}

// add an item at the end of the queue
template <typename T, unsigned N>
bool RunQueue<T, N>::push_back(T& w) {
  assert(false);
  std::scoped_lock lock(_mutex);

  auto back  = _back.load(std::memory_order_relaxed);
  auto& item = _array[(back - 1) & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);

  if(state != EMPTY ||
    !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    return false;
  }
  
  _back.store((back - 1) & POS_MASK, std::memory_order_relaxed);
  item.w = std::move(w);
  item.state.store(READY, std::memory_order_release);

  return true;
}

// add an item at the end of the queue
template <typename T, unsigned N>
bool RunQueue<T, N>::push_back(T&& w) {
  return push_back(w);
}

// pop_back removes and returns the last elements in the queue.
// Can fail spuriously.
template <typename T, unsigned N>
bool RunQueue<T, N>::pop_back(T& w) {

  if(empty()) {
    return false;
  }

  //std::unique_lock lock(_mutex, std::try_to_lock);
  if(_lock.test_and_set()) return false;


  auto back  = _back.load(std::memory_order_relaxed);
  auto& item = _array[back & IDX_MASK];
  auto state = item.state.load(std::memory_order_relaxed);

  if (state != READY ||
     !item.state.compare_exchange_strong(state, BUSY, std::memory_order_acquire)) {
    _lock.clear();
    return false;
  }

  w = std::move(item.w);
  _back.store((back + 1) & POS_MASK, std::memory_order_relaxed);
  item.state.store(EMPTY, std::memory_order_release);

  _lock.clear();
  return true;
}

// EMPTY tests whether container is empty.
// Can be called by any thread at any time.
template <typename T, unsigned N>
bool RunQueue<T, N>::empty() const { 
  return _front.load(std::memory_order_relaxed) ==
         _back.load(std::memory_order_relaxed);
}

 

///*  
template < template<typename...> class Func >
class BasicPrivatizedThreadpool {

  using TaskType = Func<void()>;

  struct Worker{
    enum : uint8_t{
      ALIVE,
      EXIT,
      GETUP
    };

    std::condition_variable cv;
    RunQueue<TaskType, 1024> queue;
    uint8_t state {ALIVE};
  };

  public:

    BasicPrivatizedThreadpool(unsigned);
    ~BasicPrivatizedThreadpool();

    size_t num_tasks() const;
    size_t num_workers() const;

    bool is_owner() const;

    void shutdown();
    void spawn(unsigned);
    void wait_for_all();

    template <typename C>
    void silent_async(C&&);

    template <typename C>
    auto async(C&&);

  private:
    
    const std::thread::id _owner {std::this_thread::get_id()};

    mutable std::mutex _mutex;

    std::condition_variable _empty_cv;

    std::deque<TaskType> _task_queue;
    std::vector<std::thread> _threads;
    std::vector<size_t> _coprimes;
    
    std::unordered_map<std::thread::id, size_t> _worker_maps;    
    std::vector<std::unique_ptr<Worker>> _workers;

    // TODO: do we need atomic variable here?
    std::atomic<bool> _allow_steal {true};

    size_t _num_idlers {0};
    size_t _next_queue {0};
    
    bool _wait_for_all {false};

    std::optional<size_t> _nonempty_worker_queue() const;

    void _xorshift32(uint32_t&);
    bool _steal(TaskType&, uint32_t&);

    std::deque<size_t> halt;

};  // class BasicPrivatizedThreadpool. --------------------------------------


// Function: _nonempty_worker_queue
template < template<typename...> class Func >
std::optional<size_t> BasicPrivatizedThreadpool<Func>::_nonempty_worker_queue() const {
  for(size_t i=0;i <_workers.size(); ++i){
    if(!_workers[i]->queue.empty()){
      return i;
    }
  }
  return {};
}

// Function: _xorshift32
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::_xorshift32(uint32_t& x){
  // x must be non zero: https://en.wikipedia.org/wiki/Xorshift
  // Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" 
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
}

// Function: _steal
template < template<typename...> class Func >
bool BasicPrivatizedThreadpool<Func>::_steal(TaskType& w, uint32_t& dice){

  _xorshift32(dice);
  const auto inc = _coprimes[dice % _coprimes.size()];
  const auto queue_num = _workers.size();
  auto victim = dice % queue_num;
  for(size_t i=0; i<queue_num; i++){
    if(_workers[victim]->queue.pop_back(w)){
      return true;
    }
    victim += inc;
    if(victim >= queue_num){
      victim -= queue_num;
    }
  }
  return false;

  //static std::atomic_flag locked {ATOMIC_FLAG_INIT};
  //while (locked.test_and_set(std::memory_order_acquire));

  //for(size_t i=0; i<queue_num; i++){
  //  if(_workers[victim]->queue.pop_back(w)){
  //    locked.clear(std::memory_order_release);                                                                                         
  //    return true;
  //  }    
  //  victim += inc; 
  //  if(victim >= queue_num){
  //    victim -= queue_num;
  //  }    
  //}

  //locked.clear(std::memory_order_release);
  //return false;
}



// Constructor
template < template<typename...> class Func >
BasicPrivatizedThreadpool<Func>::BasicPrivatizedThreadpool(unsigned N){
  spawn(N);
}

// Destructor
template < template<typename...> class Func >
BasicPrivatizedThreadpool<Func>::~BasicPrivatizedThreadpool(){
  shutdown();
}

// Function: is_owner
template < template<typename...> class Func >
bool BasicPrivatizedThreadpool<Func>::is_owner() const {
  return std::this_thread::get_id() == _owner;
}

// Function: num_tasks
template < template<typename...> class Func >
size_t BasicPrivatizedThreadpool<Func>::num_tasks() const { 
  return _task_queue.size(); 
}

// Function: num_workers
template < template<typename...> class Func >
size_t BasicPrivatizedThreadpool<Func>::num_workers() const { 
  return _threads.size();  
}

// Function: shutdown
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::shutdown(){

  if(!is_owner()){
    throw std::runtime_error("Worker thread cannot shut down the pool");
  }

  if(_threads.empty()) {
    return;
  }

  {
    std::unique_lock<std::mutex> lock(_mutex);
    // If all workers are idle && all queues are empty, then master
    // can directly wake up workers without waiting for notified
    if(_num_idlers != num_workers() || _nonempty_worker_queue().has_value() || !_task_queue.empty()){
      _wait_for_all = true;

      // Wake up all workers in case their queues are not empty
      for(auto& w : _workers){
        w->state = Worker::GETUP;
        w->cv.notify_one();
      }

      while(_wait_for_all){
        _empty_cv.wait(lock);
      }
    }

    // Notify workers to exit
    for(auto& w : _workers){
      w->state = Worker::EXIT;
      w->cv.notify_one();
    }
  } // Release lock

  for(auto& t : _threads){
    t.join();
  } 

  halt.clear();
  _threads.clear();  
  _workers.clear();
  _worker_maps.clear();
}

// Function: spawn 
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::spawn(unsigned N) {

  if(! is_owner()){
    throw std::runtime_error("Worker thread cannot spawn threads");
  }

  // Wait untill all workers become idle if any
  if(!_threads.empty()){
    // Disable steal when spawning new threads. Becuz steal allows workers to access 
    // other workers' data structures.
    _allow_steal = false;
    wait_for_all();
  }

  const size_t sz = _threads.size();

  // Lock to synchronize all workers before creating _worker_mapss
  std::scoped_lock lock(_mutex);

  _coprimes.clear();
  for(size_t i=1; i<=sz+N; i++){
    if(std::gcd(i, sz+N) == 1){
      _coprimes.push_back(i);
    }
  }

  for(size_t i=0; i<N; ++i){
    _workers.push_back(std::make_unique<Worker>());
  }

  for(size_t i=0; i<N; ++i){
    _threads.emplace_back([this, i=i+sz]() -> void {

      TaskType t {nullptr};
      Worker& w = *(_workers[i]);
      uint32_t seed = i+1;
      std::unique_lock lock(_mutex, std::defer_lock);

      while(w.state != Worker::EXIT){
        if(!w.queue.pop_front(t)) {
          if(!_allow_steal.load(std::memory_order_relaxed) || !_steal(t, seed)) {
            lock.lock();
            if(!_task_queue.empty()) {
              t = std::move(_task_queue.front());
              _task_queue.pop_front();
            }
            else{
              if(++_num_idlers == num_workers()){
                // Last active thread checks if all queues are empty
                if(auto ret = _nonempty_worker_queue(); ret.has_value()){
                  // if the nonempty queue is mine, continue to process tasks in queue
                  if(*ret == i){
                    --_num_idlers;
                    lock.unlock();
                    continue;
                  }
                  // If any queue is not empty, notify the worker to process the tasks 
                  _workers[*ret]->state = Worker::GETUP;
                  _workers[*ret]->cv.notify_one();
                }
                else{
                  // here only one thread will do so
                  // if all workers are idle && all queues are empty && master is waiting 
                  // notify the master by last thread
                  if(_wait_for_all){
                    _wait_for_all = false;
                    _empty_cv.notify_one();
                  }
                }
              } 

              halt.push_front(i);
              w.state = Worker::ALIVE;
              //while(w.state == Worker::ALIVE && w.queue.empty() && _task_queue.empty()){
              while(w.state == Worker::ALIVE){
                w.cv.wait(lock);
              }
              --_num_idlers;
            } 
            lock.unlock();
          } // End of Steal
        } // End of pop_front 

        if(t){
          t();
          t = nullptr;
        }
      } // End of while ------------------------------------------------------
    });     

    _worker_maps.insert({_threads.back().get_id(), i+sz});
  } // End of For ---------------------------------------------------------------------------------

  _allow_steal = true;
}


// Function: async
template < template<typename...> class Func >
template <typename C>
auto BasicPrivatizedThreadpool<Func>::async(C&& c){

  using R = std::invoke_result_t<C>;

  std::promise<R> p;
  auto fu = p.get_future();
  
  // master thread
  if(num_workers() == 0){
    if constexpr(std::is_same_v<void, R>){
      c();
      p.set_value();
    }
    else{
      p.set_value(c());
    } 
  }
  // have worker(s)
  else{
    if constexpr(std::is_same_v<void, R>){
      silent_async( 
        [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
          c();
          p.get().set_value(); 
        }
      );
    }
    else{
      silent_async( 
        [p = MoC(std::move(p)), c = std::forward<C>(c)]() mutable {
          p.get().set_value(c()); 
        }
      );
    }
  }
  return fu;
}

template < template<typename...> class Func >
template <typename C>
void BasicPrivatizedThreadpool<Func>::silent_async(C&& c){

  TaskType t {std::forward<C>(c)};

  //no worker thread available
  if(num_workers() == 0){
    t();
    return;
  }

  if(auto tid = std::this_thread::get_id(); tid != _owner){
    if(auto itr = _worker_maps.find(tid); itr != _worker_maps.end()){
      if(!_workers[itr->second]->queue.push_front(t)){
        std::scoped_lock<std::mutex> lock(_mutex);       
        _task_queue.push_back(std::move(t));
      }
      return ;
    }
  }

  // owner thread or other threads
  auto id = (++_next_queue) % _workers.size();
  {
    std::scoped_lock lock(_mutex);
    if(halt.empty()){
      _task_queue.push_back(std::move(t));
      _workers[id]->cv.notify_one();
      return ;
    }
    else
    {
      id = halt.front();
      halt.pop_front();
      _workers[id]->queue.push_front(t);
      _workers[id]->state = Worker::GETUP;
      _workers[id]->cv.notify_one();
      return ;
    }
  }


 // if(!_workers[id]->queue.push_back(t)){
 //   std::scoped_lock lock(_mutex);
 //   _task_queue.push_back(std::move(t));
 // }
 // else{
 //   // Lock to make sure the worker will be notified
 //   std::scoped_lock lock(_mutex);
 // }
 // _workers[id]->cv.notify_one();
}


// Function: wait_for_all
template < template<typename...> class Func >
void BasicPrivatizedThreadpool<Func>::wait_for_all() {

  if(!is_owner()){
    throw std::runtime_error("Worker thread cannot wait for all");
  }

  if(num_workers() == 0) {
    return ;
  }

  std::unique_lock lock(_mutex);
  // If all workers are idle && all queues are empty, 
  // then wait_for_all is done.
  if(_num_idlers == num_workers() && !_nonempty_worker_queue() && _task_queue.empty()){
    return ;
  }

  _wait_for_all = true;     

  for(auto& w: _workers){
    w->state = Worker::GETUP;
    w->cv.notify_one();
  }

  while(_wait_for_all){
    _empty_cv.wait(lock);
  }
}
//*/

};  // namespace tf -----------------------------------------------------------





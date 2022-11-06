#ifndef THREAD_POOL_H_
#define THREAD_POOL_H_

#include <pthread.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define DEFAULT_TIME 1

/* task */
typedef struct task_t {
    void* (*behavior)(void *);
    void* arg;
}Task;

/* thread_pool */
typedef struct threadpool_t {
    pthread_mutex_t lock;            //the lock for the whole pool
    pthread_mutex_t thread_counter;  //the lock for using busy thread counter
    pthread_cond_t  queue_not_full;  //the condition var for queue not full
    pthread_cond_t  queue_not_empty; //the condition var for queue not empty

    pthread_t* threads;              //the thread array
    pthread_t  admin_thread;         //the manager thread
    task_t*    tasks;                //the task queue

    //the information for threadpool
    int min_thread_num;              //the minimum number of threads in the threadpool
    int max_thread_num;              //the maximum number of threads in the treadpool
    int live_thread_num;             //the number of live threads in the threadpool
    int busy_thread_num;             //the number of busy threads in the threadpool
    int wait_exit_thread_num;        //the number of threads which are waiting to be destroyed in the threadpool

    //the information for taskqueue
    int queue_front;                 //the header of the queue
    int queue_rear;                  //the tail of the queue
    int queue_size;                  //the size of the queue

    int queue_max_size;              //the maximum task
    
    //the state of the threadpool
    int shutdown;                    //close when true
}ThreadPool;

threadpool_t* threadpool_create(int min_thread_num, int max_thread_num, int max_task_num);
void* threadpool_thread(void* threadpool);
int threadpool_add_task(ThreadPool* pool, void*(*function)(void* arg), void* arg);
void* admin_thread(void* threadpool);
int threadpool_free(ThreadPool* pool);
int threadpool_destroy(ThreadPool* pool);

#endif  //THREAD_POOL_H_
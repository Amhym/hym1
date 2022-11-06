#include "threadpool.h"

//创建一个线程池
threadpool_t* threadpool_create(int min_thread_num, int max_thread_num, int max_task_num) {
    threadpool_t* pool = NULL;

    //给线程池子申请空间
    pool = (threadpool_t*)malloc(sizeof(threadpool_t));
    if(pool==NULL) {
        printf("malloc threadpool fail");
        goto failure;
    }

    //信息初始化
    pool->min_thread_num = min_thread_num;
    pool->max_thread_num = max_thread_num;
    pool->live_thread_num = min_thread_num;
    pool->busy_thread_num = 0;
    pool->wait_exit_thread_num = 0;

    pool->queue_front = 0;
    pool->queue_rear  = 0;
    pool->queue_size  = 0;

    pool->queue_max_size = max_task_num;

    pool->shutdown = false;

    //初始化线程数组
    pool->threads = (pthread_t*)malloc(sizeof(pthread_t)*max_thread_num);
    if(pool->threads==NULL) {
        printf("malloc threads fail");
        goto failure;
    }
    memset(pool->threads,0,sizeof(pthread_t)*max_thread_num);

    //初始化任务队列
    pool->tasks = (Task*)malloc(sizeof(Task)*max_task_num);
    if(pool->tasks==NULL) {
        printf("malloc tasks fail");
        goto failure;
    }
    memset(pool->tasks,0,sizeof(Task)*max_task_num);

    //初始化互斥锁及条件变量
    if(pthread_mutex_init(&(pool->lock),NULL) != 0 ||
    pthread_mutex_init(&(pool->thread_counter),NULL) != 0 ||
    pthread_cond_init(&(pool->queue_not_full),NULL) != 0 ||
    pthread_cond_init(&(pool->queue_not_empty),NULL) != 0) {
        printf("init lock or cond fail");
        goto failure;
    }

    //启动min_thread_num个线程
    for(int i = 0;i < min_thread_num;i++) {
        pthread_create(&(pool->threads[i]),NULL,threadpool_thread,(void*)pool);
        // printf("start thread 0x%x... \n", (unsigned int)pool->threads[i]);
    }

    //启动管理者线程
    pthread_create(&(pool->admin_thread),NULL,admin_thread,(void*)pool);

    success:

    return pool;

    failure:

    //失败则释放线程池空间
    threadpool_free(pool);
    return NULL;
}

//工作线程
void* threadpool_thread(void* threadpool) {
    ThreadPool* pool = (ThreadPool*)threadpool;
    Task task;

    while(true) {
        pthread_mutex_lock(&(pool->lock));

        //无任务则阻塞，此时阻塞原因为“任务队列不为空”，如果有任务则跳出这个循环
        while(pool->queue_size==0 && !pool->shutdown) {
            // printf("thread 0x%x is waiting \n",(unsigned int)pthread_self());
            pthread_cond_wait(&(pool->queue_not_empty),&(pool->lock));

            //判断是否要消除线程
            if(pool->wait_exit_thread_num>0) {
                pool->wait_exit_thread_num--;
                
                //判断此时线程池中的线程是否大于最小数目
                if(pool->live_thread_num>pool->min_thread_num) {
                    // printf("thread 0x%x is exiting \n",(unsigned int)pthread_self());
                    pool->live_thread_num--;
                    pthread_mutex_unlock(&(pool->lock));
                    pthread_exit(NULL);//线程消除
                }
            }
        }

        //线程池处于关闭状态
        if(pool->shutdown) {
            pthread_mutex_unlock(&(pool->lock));
            // printf("thread 0x%x is exiting \n",(unsigned int)pthread_self());
            pthread_exit(NULL);
        }

        //线程执行任务
        task = pool->tasks[pool->queue_front];
        pool->queue_front = (pool->queue_front + 1) % pool->queue_max_size;
        pool->queue_size--;

        //通知线程池可以添加新任务
        pthread_cond_broadcast(&(pool->queue_not_full));
        //释放线程锁
        pthread_mutex_unlock(&(pool->lock));

        //执行任务
        // printf("thread 0x%x start working \n",(unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thread_num++;
        pthread_mutex_unlock(&(pool->thread_counter));

        (*(task.behavior))(task.arg);

        //结束任务
        // printf("thread 0x%x end working \n",(unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thread_num--;
        pthread_mutex_unlock(&(pool->thread_counter));
    }

    pthread_exit(NULL);
}

int threadpool_add_task(ThreadPool* pool, void*(*function)(void* arg), void* arg) {
    pthread_mutex_lock(&(pool->lock));

    //如果队列满了则阻塞
    while(pool->queue_size==pool->queue_max_size && !pool->shutdown) {
        pthread_cond_wait(&(pool->queue_not_full),&(pool->lock));
    }

    //如果线程池处于关闭状态
    if(pool->shutdown) {
        pthread_mutex_unlock(&(pool->lock));
        return -1;
    }

    //清空工作线程回调函数的参数
    if(pool->tasks[pool->queue_rear].arg!=NULL) {
        free(pool->tasks[pool->queue_rear].arg);
        pool->tasks[pool->queue_rear].arg=NULL;
    }

    //添加到任务队列
    pool->tasks[pool->queue_rear].behavior = function;
    pool->tasks[pool->queue_rear].arg      = arg;
    pool->queue_rear = (pool->queue_rear + 1) % pool->queue_max_size;
    pool->queue_size++;

    //唤醒线程
    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->lock));

    return 0;
}

// //检查线程是否存活
// int is_thread_alive(pthread_t tid) {
//     int kill_rc = pthread_kill(tid,0);
//     if(kill_rc==ESRCH) {
//         return false;
//     }
//     return true;
// }

//管理线程
void* admin_thread(void* threadpool) {
    ThreadPool* pool = (ThreadPool*)threadpool;

    while(!pool->shutdown) {
        // printf("admin..........................\n");
        sleep(DEFAULT_TIME);

        //取出任务数和存活线程数
        pthread_mutex_lock(&(pool->lock));
        int queue_size = pool->queue_size;
        int live_thread_num = pool->live_thread_num;
        pthread_mutex_unlock(&(pool->lock));

        //取出忙线程数
        pthread_mutex_lock(&(pool->lock));
        int busy_thread_num = pool->busy_thread_num;
        pthread_mutex_unlock(&(pool->lock));  

    //     printf("admin busy live -%d--%d-\n",busy_thread_num,live_thread_num);
    //     //根据实际情况添加新线程
    //     if(queue_size>=(pool->queue_max_size/2) && live_thread_num<pool->max_thread_num) {
    //         pthread_mutex_lock(&(pool->lock));
    //         int add = 0;
    //         for(int i = 0;i<pool->max_thread_num && add<10 && pool->live_thread_num<pool->max_thread_num;i++) {
    //             if(pool->threads[i]==0 || !is_thread_alive(pool->threads[i])) {
    //                 pthread_create(&(pool->threads[i]),NULL,threadpool_thread,(void*)pool);
    //                 add++;
    //                 pool->live_thread_num++;

    //             }
    //         }
    //         pthread_mutex_unlock(&(pool->lock));
    //     }

    //     //销毁多余的线程
    //     if(busy_thread_num*2 < live_thread_num && live_thread_num > pool->min_thread_num) {
    //         pthread_mutex_lock(&(pool->lock));
    //         pool->wait_exit_thread_num=1;
    //         pthread_mutex_unlock(&(pool->lock));  

    //         pthread_cond_signal(&(pool->queue_not_empty));
    //         // printf("admin clear--");
    //     }
    }

    return NULL;
}

//释放线程池
int threadpool_free(ThreadPool* pool) {
    if(pool==NULL) return -1;
    if(pool->tasks) free(pool->tasks);
    if(pool->threads) {
        free(pool->threads);
        pthread_mutex_lock(&(pool->lock));
        pthread_mutex_destroy(&(pool->lock));
        pthread_mutex_lock(&(pool->thread_counter));
        pthread_mutex_destroy(&(pool->thread_counter));
        pthread_cond_destroy(&(pool->queue_not_empty));
        pthread_cond_destroy(&(pool->queue_not_full));
    }
    free(pool);
    pool=NULL;

    return 0;
}

//销毁线程池
int threadpool_destroy(ThreadPool* pool) {
    if(pool==NULL) return -1;
    pool->shutdown=true;

    //销毁管理线程
    pthread_join(pool->admin_thread,NULL);

    //通知所有线程消除
    for(int i = 0;i < pool->live_thread_num;i++) {
        pthread_cond_broadcast(&(pool->queue_not_empty));
    }

    for(int i = 0;i < pool->live_thread_num;i++) {
        pthread_join(pool->threads[i],NULL);
    }

    threadpool_free(pool);
    return 0;
}
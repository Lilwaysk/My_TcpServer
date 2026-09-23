#include <pthread.h>

typedef struct {
    void *(*function)(void *);          // 函数指针，返回函数
    void *arg;                          // 上面函数的参数
} threadpool_task_t;                    // 各子线程任务结构体

struct threadpool_t {
    pthread_mutex_t lock;               // 用于锁住本结构体(线程池互斥锁)
    pthread_mutex_t thread_counter;     // 记录忙状态线程个数的所

    pthread_cond_t queue_not_full;      // 当任务队列满时，添加任务的线程阻塞，等待此条件变量
    pthread_cond_t queue_not_empty;     // 任务队列里不为空时，通知等待任务的线程

    pthread_t *threads;                 // 存放线程池中每个线程的tid 本质数组
    pthread_t adjust_tid;               // 存管理线程tid
    threadpool_task_t *task_queue;      // 任务队列(数组首地址)

    int min_thr_num;                    // 线程池最小线程数
    int max_thr_num;                    // 线程池最大线程数
    int live_thr_num;                   // 当前存活线程个数
    int busy_thr_num;                   // 忙状态线程个数
    int wait_exit_thr_num;              // 要销毁的线程个数

    int queue_front;                    // task_queue队头下标
    int queue_rear;                     // task_queue队尾下标
    int queue_size;                     // task_queue队列实际任务数
    int queue_max_size;                 // task_queue队中实际任务数

    int shutdown;                       // 标志位，线程池使用状态，true or false
};

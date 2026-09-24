#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <iostream>
#include <string.h>
#include <malloc.h>
using namespace std;


#define DEFAULT_THREAD_VARY 10

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

void *threadpool_thread(void *threadpool);
void *adjust_thread(void *threadpool);

int is_thread_alive(pthread_t tid);
int threadpool_free(threadpool_t *pool);

// 管理者线程
void *adjust_thread(void *threadpool)
{
    int i;
    threadpool_t *pool = (threadpool_t *)threadpool;
    while (!pool->shutdown) {

        sleep(DEFAULT_TIME);                                    // 定时 对线程池管理

        // 都是要先拿锁再访问
        pthread_mutex_lock(&(pool->lock));
        int queue_size = pool->queue_size;                      // 关注任务数
        int live_thr_num = pool->live_thr_num;                  // 存活 线程数
        pthread_mutex_unlock(&(pool->lock));

        pthread_mutex_lock(&(pool->thread_counter));
        int busy_thr_num = pool->busy_thr_num;                  // 忙线程数
        pthread_mutex_unlock(&(pool->thread_counter));

        // 创建新线程 算法：任务数大于最小线程池个数，且存活的线程数少于最大线程个数时 如30>=10 && 40<100
        if (queue_size >= MIN_WAIT_TASK_NUM && live_thr_num < pool->max_thr_num) {
            pthread_mutex_lock(&(pool->lock));
            int add = 0;

            // 一次增加DEFAULT_THREAD个线程
            for (i = 0; i < pool->max_thr_num && add < DEFALUT_THREAD_VARY && pool->live_thr_num < pool->max_thr_num; ++i) {
                if (pool->threads[i] == 0 || !is_thread_alive(pool->threads[i])) {
                    pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void *)pool);
                    add++;
                    pool->live_thr_num++;
                }
            }

            pthread_mutex_unlock(&(pool->lock));
        }

        // 销毁多余的空闲线程 算法: 忙线程x2 小于 存活的线程数 且 存活的线程数 大于 最小线程数时
        if ((busy_thr_num * 2) < live_thr_num && live_thr_num > pool->min_thr_num) {

            // 一次销毁DEFAULT_THREAD个线程，随机10个就可以
            pthread_mutex_lock(&(pool->lock));
            pool->wait_exit_thr_num = DEFAULT_THREAD_VARY;      // 要销毁的线程数 设置为10
            pthread_mutex_unlock(&(pool->lock));

            for (i = 0;i < DEFAULT_THREAD_VARY; i++) {
                // 通知处在空闲状态的线程，他们会自行终止
                pthread_cond_signal(&(pool->queue_not_empty));
            }
        }
    }
}

// 线程池中各个工作线程
void *threadpool_thread(void *threadpool)
{
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;

    while (true) {
        // 刚创建出线程，等待任务队列里有任务，否则阻塞等待任务队列里有任务后再唤醒接受任务
        pthread_mutex_lock(&(pool->lock));

        // queue_size == 0 说明没有任务，调用wait阻塞在条件变量上，若有任务，跳过该while
        while ((pool->queue_size == 0) && (!pool->shutdown)) {
            printf("thread 0x%x is waiting\n", (unsigned int)pthread_self());
            pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));

            // 清除指定数目的空闲线程，如果要结束的线程个数大于0，结束线程
            if (pool->wait_exit_thr_num > 0) {
                pool->wait_exit_thr_num--;

                // 如果线程池里线程个数大于最小值时可以结束当前线程
                if (pool->live_thr_num > pool->min_thr_num) {
                    printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
                    pool->live_thr_num--;
                    pthread_mutex_unlock(&(pool->lock));

                    pthread_exit(NULL);
                }
            }
        }
    }
}

threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size)
{
    int i;
    threadpool_t *pool = NULL;          // 线程池 结构体

    do {
        if ((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL) {
            printf("malloc threadpool fail");
            break;
        }

        pool->min_thr_num = min_thr_num;
        pool->max_thr_num = max_thr_num;
        pool->busy_thr_num = 0;
        pool->live_thr_num = min_thr_num;                       // 活着的线程数 初值=最小线程数
        pool->wait_exit_thr_num = 0;
        pool->queue_size = 0;                                   // 有0个任务
        pool->queue_max_size = queue_max_size;                  // 最大任务队列数
        pool->queue_front = 0;
        pool->queue_rear = 0;
        pool->shutdown = false;                                 // 不关闭线程

        // 根据最大线程上线数，给工作线程数组开辟空间，并赋初值为0
        pool->threads = (pthread_t *)malloc(sizeof(pthread_t)*max_thr_num);
        if (pool->threads == NULL) {
            printf("malloc thread fail");
            break;
        }
        memset(pool->threads, 0, sizeof(pthread_t)*max_thr_num);

        // 给任务队列开辟新的空间
        pool->task_queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t)*queue_max_size);
        if (pool->task_queue == NULL) {
            printf("malloc task_queue fail");
            break;
        }

        // 初始化互斥锁，条件变量
        if (pthread_mutex_init(&(pool->lock), NULL) != 0
            || pthread_mutex_init(&(pool->thread_counter), NULL) != 0
            || pthread_cond_init(&(pool->queue_not_empty), NULL) != 0
            || pthread_cond_init(&(pool->queue_not_full), NULL)  != 0 ) {

                printf("init the lock or cond fail");
                break;
            }

        // 启动min_thr_num 个 work_thread
        for (i = 0; i < min_thr_num; ++i) {
            pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void *)pool);     // pool指向当前线程池
            printf("start thread 0x%x...\n", (unsigned int)pool->threads[i]);
        }
        pthread_create(&(pool->adjust_tid), NULL, adjust_thread, (void *)pool);             // 创建管理者线程

        return pool;

    } while(0);

    threadpool_free(pool);      // 前面代码调用失败时，释放poll存储空间

    return NULL;
}

// 线程池中的线程，模拟业务处理
void *process(void *arg)
{
    printf("thread 0x%x working on task %d\n", (unsigned int)pthread_self.(int)arg);
    sleep(1);
    printf("task %d is end\n",(int)arg);

    return NULL;
}

int main(void)
{
    threadpool_t *thp = threadpool_create(3, 100, 100);         // 创建线程池，池里最小3个线程，最大100，队列最大100
    printf("poll inited");


    int *num = (int *)malloc(sizeof(int)*20);
    for (int i = 0; i < 20; ++i) {
        num[i] = i;
        printf("add task %d\n", i);

        threadpool_add(thp, process, (void*)&num[i]);           // 向线程池中添加任务
    }

    sleep(10);                                                  // 等待子线程完成任务
    threadpool_destroy(thp);

    return 0;
}

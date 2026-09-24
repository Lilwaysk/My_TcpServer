#include <pthread.h>
#include <stdio.h>                          // ★修复: printf 要用(原来靠 iostream 间接包含,不保险)
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <iostream>
#include <string.h>
#include <malloc.h>
#include <signal.h>                         // ★修复: is_thread_alive 里要用 pthread_kill
#include <errno.h>                          // ★修复: is_thread_alive 里要用 ESRCH
using namespace std;


#define DEFAULT_THREAD_VARY 10
#define DEFAULT_TIME 1                      // ★修复: adjust_thread 里用到,原来漏了定义
#define MIN_WAIT_TASK_NUM 10                // ★修复: adjust_thread 里用到,原来漏了定义

typedef struct {
    void *(*function)(void *);          // 函数指针，返回函数
    void *arg;                          // 上面函数的参数
} threadpool_task_t;                    // 各子线程任务结构体

struct threadpool_t {
    pthread_mutex_t lock;               // 用于锁住本结构体(线程池互斥锁)
    pthread_mutex_t thread_counter;     // 记录忙状态线程个数的锁

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

// ★修复: 这个函数原来只有声明没有定义,链接时会报 undefined reference to `is_thread_alive'
int is_thread_alive(pthread_t tid)
{
    int kill_rc = pthread_kill(tid, 0);     // 给线程发 0 号信号,只做存在性检查,不会真的发信号
    if (kill_rc == ESRCH)
        return false;                       // 线程已经不存在了
    return true;
}

int threadpool_destroy(threadpool_t *pool)
{
    int i;
    if (pool == NULL)
        return -1;

    // ★修复: shutdown 是所有线程共享的状态,写它必须拿锁
    // ★修复: 原来是在不持锁的情况下 broadcast,而且用一个 for 循环广播了 N 次(等价于 1 次)
    pthread_mutex_lock(&(pool->lock));
    pool->shutdown = true;
    pthread_cond_broadcast(&(pool->queue_not_empty));   // 唤醒所有空闲线程,让它们去检查 shutdown
    pthread_mutex_unlock(&(pool->lock));

    // 先销毁管理线程
    pthread_join(pool->adjust_tid, NULL);

    // ★修复: 必须等工作线程全部退出,才能释放 pool。
    // 原来广播完就直接 threadpool_free,而工作线程被唤醒后还要抢锁、检查 shutdown、解锁才能退出,
    // 这段时间里主线程已经把整个 pool 释放了 —— 工作线程再去碰 pool->lock 就是访问已释放的内存。
    // threads[i] == 0 的槽位从来没创建过线程,不能 join。
    for (i = 0; i < pool->max_thr_num; ++i) {
        if (pool->threads[i] != 0)
            pthread_join(pool->threads[i], NULL);
    }

    threadpool_free(pool);

    return 0;
}

int threadpool_free(threadpool_t *pool)
{
    if (pool == NULL)
        return -1;

    if (pool->task_queue) {
        free(pool->task_queue);
        pool->task_queue = NULL;        // ★修复: free 之后要置空,否则后面不小心还会用到悬空指针
    }

    // ★修复: 这里原来写的是 pool->task_queue。可它刚刚在第 76 行被 free 过还没置空,
    // 条件恒为真,靠巧合才没出问题 —— 作者想判断的显然是 pool->threads。
    if (pool->threads) {
        free(pool->threads);
        pool->threads = NULL;
        // ★修复: 原来这里是先 pthread_mutex_lock 再 pthread_mutex_destroy,毫无意义,
        // destroy 不需要持锁,而且刚 lock 的锁马上销毁本身就是错的用法。
        pthread_mutex_destroy(&(pool->lock));
        pthread_mutex_destroy(&(pool->thread_counter));
        pthread_cond_destroy(&(pool->queue_not_empty));
        pthread_cond_destroy(&(pool->queue_not_full));
    }
    free(pool);

    // ★修复: 函数声明是 int,原来却没有 return,属于未定义行为(编译器会警告)。
    // 另外原来的 pool = NULL 只是改了个局部变量,调用者看不到,已经删掉。
    return 0;
}

int threadpool_all_threadnum(threadpool_t *pool)
{
    int all_threadnum = -1;                 // 总线程数

    pthread_mutex_lock(&(pool->lock));
    all_threadnum = pool->live_thr_num;     // 存活线程数
    pthread_mutex_unlock(&(pool->lock));

    return all_threadnum;
}

int threadpool_busy_threadnum(threadpool_t *pool)
{
    int busy_threadnum = -1;                // 忙线程数

    pthread_mutex_lock(&(pool->thread_counter));
    busy_threadnum = pool->busy_thr_num;
    pthread_mutex_unlock(&(pool->thread_counter));

    return busy_threadnum;
}

// 向线程池中添加一个任务
int threadpool_add(threadpool_t *pool, void *(*function)(void *arg), void *arg)
{
    pthread_mutex_lock(&(pool->lock));

    // ==为真，队列已经满，调wait阻塞
    while ((pool->queue_size == pool->queue_max_size) && (!pool->shutdown))
        pthread_cond_wait(&(pool->queue_not_full), &pool->lock);

    if (pool->shutdown) {
        pthread_mutex_unlock(&(pool->lock));
        return -1;      // ★修复: 任务根本没被加进队列,必须返回错误码。
                        // 原来返回的是 0(表示成功),调用者会以为任务被受理了。
    }

    // ★修复: 原来这里有一段"清空 arg"的代码,已经整段删掉。
    // 它先判断 arg != NULL 再写入 NULL,对正确性没有任何影响(往空指针里再写一次 NULL 没有副作用),
    // 因为下面那句 .arg 赋值本来就会覆盖这一格。
    //
    // ★修复: 这一行原来是  pool->task_queue[pool->queue_rear].arg = NULL;
    // 这就把 threadpool_add 的形参 arg 整个丢掉了 —— 每个任务拿到的参数都是 NULL,
    // 运行起来 process 里会全部打印 task 0。改成 = arg 才是真正把参数存进队列。
    pool->task_queue[pool->queue_rear].function = function;
    pool->task_queue[pool->queue_rear].arg = arg;

    pool->queue_rear = (pool->queue_rear + 1) % pool->queue_max_size;       // 队尾指针移动，模拟环形队列
    pool->queue_size++;                     // 任务队列里的任务数加一

    // 添加完任务后，队列不为空，唤醒线程池中 等待处理任务的线程
    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->lock));

    return 0;
}

// 管理者线程
void *adjust_thread(void *threadpool)
{
    int i;
    threadpool_t *pool = (threadpool_t *)threadpool;
    while (!pool->shutdown) {

        sleep(DEFAULT_TIME);                                    // 定时 对线程池管理

        // ★修复: 睡醒之后 shutdown 可能已经被 threadpool_destroy 置成 true 了,
        // 直接退出,避免在关闭过程中还去创建新线程。
        if (pool->shutdown)
            break;

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
            // ★修复: 这里原来写成了 DEFALUT_THREAD_VARY(少了个 N),编译直接报错
            for (i = 0; i < pool->max_thr_num && add < DEFAULT_THREAD_VARY && pool->live_thr_num < pool->max_thr_num; ++i) {
                if (pool->threads[i] == 0 || !is_thread_alive(pool->threads[i])) {
                    // ★修复: 原来完全没检查 pthread_create 的返回值。
                    // 创建失败时 live_thr_num 会被虚增,线程数统计从此和实际情况对不上。
                    if (pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void *)pool) != 0) {
                        printf("create thread fail\n");
                        pool->threads[i] = 0;
                        continue;
                    }
                    add++;
                    pool->live_thr_num++;
                }
            }

            pthread_mutex_unlock(&(pool->lock));
        }

        // 销毁多余的空闲线程 算法: 忙线程x2 小于 存活的线程数 且 存活的线程数 大于 最小线程数时
        if ((busy_thr_num * 2) < live_thr_num && live_thr_num > pool->min_thr_num) {

            // ★修复: 设置退出名额和通知必须放在同一把锁里。
            // 原来先解锁、再在锁外循环 signal 10 次:通知可能丢掉,而且循环 10 次是多余的 ——
            // 空闲线程在同一个条件变量上等待,一次 broadcast 就够它们全醒过来抢名额。
            pthread_mutex_lock(&(pool->lock));
            pool->wait_exit_thr_num = DEFAULT_THREAD_VARY;      // 要销毁的线程数 设置为10
            pthread_cond_broadcast(&(pool->queue_not_empty));   // 通知处在空闲状态的线程,它们会自行终止
            pthread_mutex_unlock(&(pool->lock));
        }
    }

    // ★修复: 补上 return,否则编译器警告 "no return statement in function returning non-void"
    return NULL;
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
            // ★修复: 加上了 queue_size == 0 这个条件。
            // pthread_cond_wait 返回后,代码在重新检查外层 while 条件之前就会走到这里,
            // 所以一个线程完全可能是被"来了新任务"的 signal 唤醒的。
            // 原来没有这个判断,它就会误走退出分支 —— 队列里明明有活,它却自己跑了。
            if ((pool->wait_exit_thr_num > 0) && (pool->queue_size == 0)) {
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

        // 如果指定了true，要关闭线程池里的每个线程，自行退出处理---销毁线程池
        if (pool->shutdown) {
            pthread_mutex_unlock(&(pool->lock));
            printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
            // ★修复: 去掉了这里的 pthread_detach(pthread_self())。
            // 被 detach 的线程是 join 不了的,threadpool_destroy 就等不到它退出,
            // 主线程可能在它还在跑的时候就把整个 pool 释放掉。
            pthread_exit(NULL);             // x线程自行结束
        }

        // 从任务队列里获取任务，是一个出队操作
        task.function = pool->task_queue[pool->queue_front].function;
        task.arg = pool->task_queue[pool->queue_front].arg;

        pool->queue_front = (pool->queue_front + 1) % pool->queue_max_size;         // 出队，模拟环形队列
        pool->queue_size--;

        // 通知可以有新的任务添加进来
        pthread_cond_broadcast(&(pool->queue_not_full));

        // 任务取出后，立即将 线程池的锁释放
        pthread_mutex_unlock(&(pool->lock));

        // 执行任务
        printf("thread 0x%x start working\n", (unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));            // 忙状态线程数变量锁
        pool->busy_thr_num++;                                   // 忙线程数+1
        pthread_mutex_unlock(&(pool->thread_counter));

        (*(task.function))(task.arg);                           // 执行回调函数任务

        // 任务结束处理
        printf("thread 0x%x end working\n", (unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thr_num--;                                   // 处理掉一个任务，忙线程数-1
        pthread_mutex_unlock(&(pool->thread_counter));
    }

    pthread_exit(NULL);
}

threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size)
{
    int i;
    threadpool_t *pool = NULL;          // 线程池 结构体

    do {
        // ★修复: 改用 calloc,让整个结构体一开始就是全 0。
        // 原来用 malloc,结构体里全是随机值:一旦后面某一步失败跳到 threadpool_free,
        // pool->task_queue 还是随机值,第 74 行的 if 判断大概率成立,于是 free(随机值) 直接崩。
        if ((pool = (threadpool_t *)calloc(1, sizeof(threadpool_t))) == NULL) {
            printf("malloc threadpool fail");
            break;
        }

        // ★修复: 互斥锁 / 条件变量提到最前面初始化。
        // 这样只要 pool 不是 NULL,threadpool_free 里 destroy 的就一定是真正的锁,
        // 不会碰到还没初始化的内存。
        if (pthread_mutex_init(&(pool->lock), NULL) != 0
            || pthread_mutex_init(&(pool->thread_counter), NULL) != 0
            || pthread_cond_init(&(pool->queue_not_empty), NULL) != 0
            || pthread_cond_init(&(pool->queue_not_full), NULL)  != 0 ) {

                printf("init the lock or cond fail");
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
    // ★修复: 原来这一行是  pthread_self.(int)arg  —— 少了一个右括号和一个逗号,编译直接报错。
    // ★修复: (int)arg 是把指针硬截断成 int,64 位下会丢掉一半。参数本来就是 int*,应该先转回 int* 再取值。
    int id = *(int *)arg;

    printf("thread 0x%x working on task %d\n", (unsigned int)pthread_self(), id);
    sleep(1);
    printf("task %d is end\n", id);

    return NULL;
}

int main(void)
{
    threadpool_t *thp = threadpool_create(3, 100, 100);         // 创建线程池，池里最小3个线程，最大100，队列最大100
    if (thp == NULL) {                                          // ★修复: 创建失败要检查,否则后面全是空指针解引用
        printf("threadpool_create fail\n");
        return -1;
    }
    printf("poll inited\n");                                    // ★修复: 补上换行

    int *num = (int *)malloc(sizeof(int)*20);
    for (int i = 0; i < 20; ++i) {
        num[i] = i;
        printf("add task %d\n", i);

        threadpool_add(thp, process, (void*)&num[i]);           // 向线程池中添加任务
    }

    sleep(10);                                                  // 等待子线程完成任务
    threadpool_destroy(thp);

    free(num);                                                  // ★修复: 这块内存原来一直没释放
    num = NULL;

    return 0;
}

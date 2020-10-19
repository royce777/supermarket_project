#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include "../include/queue.h"
#include <math.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define SYSCALL(r, c, e) \
    if ((r = c) == -1)   \
    {                    \
        perror(e);       \
        exit(errno);     \
    }

#define PRINT(fmt, ...)                      \
    do                                       \
    {                                        \
        fprintf(stdout, fmt, ##__VA_ARGS__); \
        fflush(stdout);                      \
    } while (0)

#define BUFLEN 128
// #define T 200
// #define P 100
// #define K 6
// #define S1
// #define S2
// #define E 3
// #define C 50
// #define CONSTANT_SLEEP_PER_PROD 2000000
// #define CONST_SLEEP 50000000
// K=2, C=20, E=5, T=500, P=80, S=30
// K=6, C=50, E=3, T=200, P=100, S=20
static void *director(void *arg);
static void *cashiers_controller(void *arg);
static void *customers_controller(void *arg);
void open_checkout(pthread_t *cashiers, int *cashier_ids);
void close_checkout(pthread_t *cashiers);

typedef struct _config
{
    int P;
    int T;
    int K;
    int S1;
    int S2;
    int C;
    int E;
    int H; // how many cashiers open on start?
    int G; // wait time per product(msec)
    int L; // director info update interval (msec)
    char *log_name;
} config;

typedef struct ca
{
    unsigned int id;
    unsigned int prod;
    unsigned int shop_time;
    unsigned long enter_time;
    unsigned long qtime;
    unsigned long etime;
    unsigned int visited;
} cl_arg;

typedef struct customer_in_queue
{
    int id;
    unsigned int num_prod;
    int served;
    pthread_mutex_t mux;
} cust_in_q;

typedef struct _cashier
{
    unsigned int passed_prods;
    unsigned int served_customers;
    unsigned long tot_open_time;
    unsigned long tot_serv_time;
    unsigned int closure_num;
} cashier_data;

typedef struct buf_struct
{
    int id;
    unsigned int *opened;
    pthread_mutex_t open_mux;
} bstruct;

typedef struct _buf
{
    int open;
    unsigned long timestamp;
    int num_cust;
} buf;

typedef struct _clnode
{
    cl_arg *data;
    struct _clnode *next;
} clnode;

// CONFIG STRUCT
config *c;
config *parse_config(const char *conf);

// CHECKOUT QUEUES
Queue_t **qu;
// CASHIER DATA
cashier_data *cash_data;

// CHECKOUT - DIRECTOR COMMUNICATION BUFFER

buf **buffer;
pthread_mutex_t *block;

// NUMBER OF OPENED CASHIERS
int num_cash = 0;
// NUMBER OF CUSTOMERS INSIDE SUPERMARKET
int num_cust = 0;
pthread_mutex_t ncustmux = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t exitwait = PTHREAD_COND_INITIALIZER;
// PRINT MUTEX FOR DEBUG
pthread_mutex_t printmux = PTHREAD_MUTEX_INITIALIZER;
// TIME GLOBAL VAR
long times;
unsigned int main_seed;

//
volatile sig_atomic_t sighup = 0;
volatile sig_atomic_t sigquit = 0;

clnode *clist_head; // CUSTOMERS DATA LIST
clnode *clist_tail;
void insert_clist(cl_arg *arg);
//void delete_clist(clnode *arg);
void print_clist(FILE *f);

// CANEXIT VARIABLE ==> COMUNICATION DIRECTOR - CUSTOMERS WITH 0 PRODUCTS
pthread_mutex_t exitmux = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t exwait = PTHREAD_COND_INITIALIZER;
int canexit = 0;
// function that returns current time in milliseconds
unsigned long get_cur_time();

static void handler(int signum)
{

    if (signum == 1)
        sighup = 1;
    if (signum == 3)
        sigquit = 1;
    PRINT("RECEIVED SIGNAL : ====================== %d\n", signum);
}

// ==========================CUSTOMER=============================

int select_checkout(unsigned int *seed);
int my_push(Queue_t **q, cust_in_q *custinf, unsigned int *seed);
void customer_exit();

void *customer(void *arg)
{
    cl_arg *args = (cl_arg *)arg;
    unsigned int id = args->id;
    unsigned int prods = args->prod;
    unsigned int shop_time = args->shop_time;
    unsigned int seed = times + id;
    args->enter_time = get_cur_time();
    //unsigned long enter_time = get_cur_time();

    struct timespec tim = {(shop_time / 1000), ((shop_time % 1000) * 1000000)};
    int err;
    SYSCALL(err, nanosleep(&tim, NULL), "Customer shopping failed\n");
    // PRINT("Customer %d shopped succesfully\n", id);
    //now check your prods number and enqueue / ask director to exit !
    if (prods == 0)
    {
        PRINT("Customer %d has got 0 products, asks to exit !\n", id);
        pthread_mutex_lock(&exitmux);
        while (canexit == 0)
        {
            pthread_cond_wait(&exwait, &exitmux);
        }
        canexit = 0;
        pthread_mutex_unlock(&exitmux);
    }
    else
    {
        //int served = 0;
        //prepare your infos to enqueue !!!
        cust_in_q *infos = malloc(sizeof(cust_in_q));
        if (!infos)
        {
            PRINT("Error in malloc\n");
            exit(EXIT_FAILURE);
        }

        // INITIALIZE CUSTOMER INFOS STRUCT TO ENQUEUE
        infos->id = id;
        infos->num_prod = prods;
        infos->served = 0;
        if (pthread_mutex_init(&infos->mux, NULL) != 0)
        {
            perror("pthread_mutex_init in generating customer struct");
            return NULL;
        }

        // END OF CUSTOMER STRUCT INITIALIZATION

        //lookup for an open desk end enqueue
        //int visited_chouts = 0;
        //unsigned long enqueue_time = get_cur_time();
        // while (!served)
        // {
        //     visited_chouts++;
        //     served = my_push(qu, infos);
        // }
        unsigned long qstart_time = get_cur_time();
        int visited = my_push(qu, infos, &seed);
        unsigned long qend_time = get_cur_time();
        args->qtime = qend_time - qstart_time;
        args->visited = visited;
        //unsigned long served_time = get_cur_time();
        //unsigned long time_in_queue = served_time - enqueue_time;
    }
    //PRINT("Client %d esce dal supermarket\n", id);
    args->etime = get_cur_time();
    customer_exit();
    return NULL;
    // TO DO : write logs, free memory, exit.
}

void customer_exit()
{
    pthread_mutex_lock(&ncustmux);
    num_cust--;
    pthread_cond_signal(&exitwait);
    pthread_mutex_unlock(&ncustmux);
}

int select_checkout(unsigned int *seed)
{
    //unsigned int seed;
    int k;
    k = rand_r(seed) % c->K;
    //PRINT("select_checkout returned %d\n", k);
    return k;
}

int my_push(Queue_t **q, cust_in_q *custinf, unsigned int *seed)
{
    if ((q == NULL) || (custinf == NULL))
    {
        PRINT("my_push null parameter\n");
        errno = EINVAL;
        return -1;
    }
    int done = 0;
    int chout;
    int visited = 0;
    while (done == 0 && sigquit == 0)
    {
        chout = select_checkout(seed);
        LockQueue(q[chout]);
        //PRINT("Customer trying to join %d, and status : %d\n", chout, q[chout]->open);
        if (q[chout]->open)
        {
            visited++;

            Node_t *n = allocNode();
            if (!n)
                return -1;
            n->data = custinf;
            n->next = NULL;

            q[chout]->tail->next = n;
            q[chout]->tail = n;
            q[chout]->qlen += 1;
            pthread_cond_signal(&q[chout]->qcond);
            pthread_mutex_lock(&custinf->mux);
            //PRINT("customer %d and chout = %d\n", custinf->id, chout);
            while (custinf->served == 0 && q[chout]->open == 1 && sigquit == 0)
            {
                //int whosig = custinf->id;
                pthread_mutex_unlock(&custinf->mux);
                pthread_cond_wait(&q[chout]->cwait, &q[chout]->qlock);
                //PRINT("CUSTOMER %d WOKE UP\n", custinf->id);

                pthread_mutex_lock(&custinf->mux);
            }
            if (sigquit == 1)
            {
                if (custinf->served == 0)
                {

                    //PRINT("CUSTOMER %d EXITING BEFORE SERVING\n", custinf->id);
                }
                pthread_mutex_unlock(&custinf->mux);
                done = 1;
            }

            else if (custinf->served == 1)
            {
                done = 1;
            }
            else
            {
                // PRINT("La cassa %d ha chiuso, il cliente %d cambia \n", chout, custinf->id);
            }
            pthread_mutex_unlock(&custinf->mux);
        }
        UnlockQueue(q[chout]);
    }
    return visited;
}

// =========================END CUSTOMER==========================

// ==========================CASHIER==============================

// FUNCTION TO PRINT QUEUE
void print_queue(Queue_t *q, int id);

// function that returns current time in milliseconds
unsigned long get_cur_time()
{
    // long ms;  // Milliseconds
    // time_t s; // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    unsigned long time_in_mill =
        (spec.tv_sec) * 1000 + (spec.tv_nsec) / 1000000;
    // convert tv_sec & tv_usec to millisecond
    return time_in_mill;
}

void *informer(void *arg)
{
    bstruct *op = (bstruct *)arg;
    int id = op->id;
    int opened = 1;

    // prepare for nanosleep
    struct timespec tim;
    tim.tv_sec = 0;
    tim.tv_nsec = c->L * 1000000;
    int err;
    //end
    while (opened)
    {
        //PRINT("INFORMER RUNNING\n");
        SYSCALL(err, nanosleep(&tim, NULL), "Cashier supporter sleep failed\n");
        pthread_mutex_lock(&block[id]); // LOCK BUFFER
        if (buffer[id]->open == 0)
        {
            opened = 0;
            pthread_mutex_unlock(&block[id]); // UNLOCK BUFFER
        }
        else
        {
            int qsize = length(qu[id]);
            unsigned long time_stamp = get_cur_time();
            buffer[id]->timestamp = time_stamp;
            buffer[id]->num_cust = qsize;
            //PRINT("buffer[%d]->num_cust = %d\n", id, buffer[id]->num_cust);
            pthread_mutex_unlock(&block[id]); // UNLOCK BUFFER
        }
    }
    PRINT("Cashier %d is being closed !\n", id);
    pthread_mutex_lock(&op->open_mux);
    *(op->opened) = 0;
    pthread_mutex_unlock(&op->open_mux);
    return NULL;
}

void *cashier(void *arg)
{
    int id = *(int *)arg;
    PRINT("ID of cashier is %d\n", id);
    // array_opened[your-id] = 1;
    open_queue(qu[id]);

    unsigned int opened = 1;
    unsigned int seed = times - id;
    pthread_mutex_lock(&block[id]); // LOCK BUFFER
    buffer[id]->open = 1;
    pthread_mutex_unlock(&block[id]); // UNLOCK BUFFER

    // INITIALIZE INFORMER SUPPORT THREAD PARAMETER STRUCT
    bstruct *informer_arg = malloc(sizeof(bstruct));
    informer_arg->id = id;
    informer_arg->opened = &opened;
    if (pthread_mutex_init(&informer_arg->open_mux, NULL) != 0)
    {
        perror("informer parameter mutex init");
        exit(EXIT_FAILURE);
    }
    // prendi il tempo time
    // TO DO :  scrivere funzione che prende il tempo corrente in millisecondi
    // CREATING SUPPORT THREAD TO INFORM DIRECTOR
    pthread_t support;
    pthread_create(&support, NULL, informer, informer_arg);
    pthread_mutex_lock(&informer_arg->open_mux); // LOCK OPEN MUX
    unsigned long start_time = get_cur_time();
    unsigned int const_wait_time = (rand_r(&seed) % 60) + 20; //msec
    while (opened == 1)
    {
        pthread_mutex_unlock(&informer_arg->open_mux); //UNLOCK OPEN MUX
        // pthread_mutex_lock(&printmux);
        // print_queue(qu[id], id);
        // pthread_mutex_unlock(&printmux);

        cust_in_q *customer = pop(qu[id]);
        if (!customer)
        {
            PRINT("La cassa ha estratto customer NULL \n");
        }
        if (customer->id == -1)
        {
            PRINT("END MESSAGE RECEIVED\n");
            // opened = 0;
            break;
        }
        unsigned long start_serv_time = get_cur_time();
        struct timespec tim;
        tim.tv_sec = 0;
        tim.tv_nsec = customer->num_prod * c->G * 1000000 + const_wait_time * 1000000;
        int err;
        SYSCALL(err, nanosleep(&tim, NULL), "Cashier serving sleep\n");
        cash_data[id].passed_prods += customer->num_prod;
        //int whosig = customer->id;
        LockQueue(qu[id]);
        pthread_mutex_lock(&customer->mux);
        customer->served = 1;
        //PRINT("CASHIER %d SIGNALLING TO %d \n", id, whosig);
        pthread_mutex_unlock(&customer->mux);
        pthread_cond_broadcast(&qu[id]->cwait);
        UnlockQueue(qu[id]);
        unsigned long end_serv_time = get_cur_time();
        cash_data[id].tot_serv_time += (end_serv_time - start_serv_time);
        cash_data[id].served_customers++;
        //PRINT("Customer %d has been served\n", customer->id);

        pthread_mutex_lock(&informer_arg->open_mux); // LOCK OPEN MUX
    }
    unsigned long end_time = get_cur_time();
    cash_data[id].tot_open_time += (end_time - start_time);
    cash_data[id].closure_num += 1;
    // CLOSE QUEUE AND SIGNAL ALL CUSTOMERS IN QUEUE
    close_queue(qu[id]);
    LockQueue(qu[id]);
    pthread_cond_broadcast(&qu[id]->cwait);
    UnlockQueue(qu[id]);
    // ---------END---------------
    pthread_mutex_unlock(&informer_arg->open_mux); //UNLOCK OPEN MUX
    pthread_join(support, NULL);
    PRINT("Supporter of cashier %d joined \n", id);

    return NULL;
}

void print_queue(Queue_t *q, int id)
{
    if (!q)
    {
        perror("print_queue null q\n");
        exit(EXIT_FAILURE);
    }
    LockQueue(q);
    Node_t *n = (Node_t *)q->head;
    PRINT("QUEUE %d : ", id);
    while (n->next != NULL)
    {
        void *data = n->next->data;
        cust_in_q *k = (cust_in_q *)data;
        PRINT("%d ", k->id);
        n = n->next;
    }
    PRINT("END\n");
    UnlockQueue(q);
}
// ==========================END CASHIER =========================

// =========================== MAIN ============================
int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 3)
    {
        PRINT("Please run with : -c <config>\n");
        exit(EXIT_FAILURE);
    }
    char *conf_path;
    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1)
    {
        switch (opt)
        {
        case 'c':
            conf_path = optarg;
            break;
        default:
            abort();
        }
    }
    if (strcmp(conf_path, "./test/config.txt") != 0)
    {
        PRINT("%s\n", conf_path);
        fprintf(stderr, "Wrong config file\n");
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    if ((c = parse_config(conf_path)) == NULL)
    {
        exit(EXIT_FAILURE);
    }
    PRINT("START\n");
    times = time(NULL);
    main_seed = times;

    // INITIALIZE CHECKOUT QUEUES
    qu = malloc(c->K * sizeof(Queue_t *));
    for (int i = 0; i < c->K; i++)
    {
        qu[i] = initQueue();
    }

    // CHECKOUT-DIRECTOR BUFFER INITIALIZATION
    buffer = malloc(c->K * sizeof(buf *));
    if (!buffer)
    {
        perror("Malloc cashier data");
        exit(EXIT_FAILURE);
    }

    // INITIALIZE buffer ARRAY
    for (int i = 0; i < c->K; i++)
    {
        buffer[i] = malloc(sizeof(buf));
    }

    for (int i = 0; i < c->K; i++)
    {
        buffer[i]->open = 0;
        buffer[i]->timestamp = 0;
        buffer[i]->num_cust = 0;
    }
    block = malloc(c->K * sizeof(pthread_mutex_t));
    for (int i = 0; i < c->K; i++)
    {
        if (pthread_mutex_init(&block[i], NULL) != 0)
        {
            perror("qlock mutex init");
            exit(EXIT_FAILURE);
        }
    }
    cash_data = malloc(c->K * sizeof(cashier_data));
    for (int i = 0; i < c->K; i++)
    {
        cash_data[i].passed_prods = 0;
        cash_data[i].served_customers = 0;
        cash_data[i].closure_num = 0;
        cash_data[i].tot_open_time = 0;
        cash_data[i].tot_serv_time = 0;
    }

    struct sigaction s;
    memset(&s, 0, sizeof(s));
    s.sa_handler = handler;
    if (sigaction(SIGQUIT, &s, NULL) == -1)
    {
        perror("Handler error");
    }
    if (sigaction(SIGHUP, &s, NULL) == -1)
    {
        perror("Handler error");
    }
    clist_head = NULL;
    clist_tail = NULL;

    pthread_t dir;
    PRINT("sto per creare il direttore\n");
    pthread_create(&dir, NULL, director, NULL);
    pthread_join(dir, NULL);
    PRINT("il direttore e' terminato\n");
    FILE *stats;
    if ((stats = fopen(c->log_name, "w")) == NULL)
    {
        fprintf(stderr, "Log file open failed\n");
        exit(EXIT_FAILURE);
    }
    print_clist(stats);
    return 0;
}

static void *director(void *arg)
{
    pthread_t cash_controller;
    pthread_create(&cash_controller, NULL, cashiers_controller, NULL);
    pthread_t cust_controller;
    pthread_create(&cust_controller, NULL, customers_controller, NULL);

    pthread_join(cash_controller, NULL);
    pthread_join(cust_controller, NULL);
    return NULL;
}

static void *customers_controller(void *arg)
{
    pthread_t *customers = malloc(c->C * sizeof(pthread_t));
    int cust_thread_size = c->C;
    for (int i = 0; i < c->C; i++)
    {
        cl_arg *client = malloc(sizeof(cl_arg));
        client->id = i;
        client->prod = rand_r(&main_seed) % c->P;
        client->shop_time = rand_r(&main_seed) % c->T;
        if (client->shop_time <= 10)
            client->shop_time = 11;
        client->enter_time = 0;
        client->etime = 0;
        client->qtime = 0;
        client->visited = 0;
        pthread_create(&(customers[i]), NULL, customer, client);
        insert_clist(client);
        pthread_mutex_lock(&ncustmux);
        num_cust++;
        pthread_mutex_unlock(&ncustmux);
        // PRINT("E' stato creato il cliente %d \n", i);
    }

    while (sigquit == 0 && sighup == 0)
    {
        pthread_mutex_lock(&exitmux);
        if (canexit == 0)
        {
            canexit = 1;
            pthread_cond_broadcast(&exwait);
        }
        pthread_mutex_unlock(&exitmux);

        pthread_mutex_lock(&ncustmux);
        while (c->C - num_cust < c->E)
        {
            pthread_cond_wait(&exitwait, &ncustmux);
        }
        //PRINT("DENTRO CI SONO %d CUSTOMERS -> NE FACCIO ENTRARE ALTRI\n", num_cust);
        int new_cust = c->C - num_cust;
        int oldsize = cust_thread_size;
        cust_thread_size += new_cust;
        if ((customers = realloc(customers, cust_thread_size * sizeof(pthread_t))) == NULL)
        {
            perror("customers thread array realloc");
            exit(EXIT_FAILURE);
        }
        for (int i = 0; i < new_cust; i++)
        {
            cl_arg *client = malloc(sizeof(cl_arg));
            client->id = oldsize + i;
            client->prod = rand_r(&main_seed) % c->P;
            client->shop_time = rand_r(&main_seed) % c->T;
            if (client->shop_time <= 10)
                client->shop_time = 11;
            client->enter_time = 0;
            client->etime = 0;
            client->qtime = 0;
            client->visited = 0;
            insert_clist(client);
            //PRINT("Customer %d prods %d shtime %d\n", client->id, client->prod, client->shop_time);
            pthread_create(&(customers[oldsize + i]), NULL, customer, client);
            num_cust++;
        }
        pthread_mutex_unlock(&ncustmux);
    }
    pthread_mutex_lock(&ncustmux);
    while (num_cust > 0)
    {
        pthread_mutex_unlock(&ncustmux);

        pthread_mutex_lock(&exitmux);
        if (canexit == 0)
        {
            canexit = 1;
            pthread_cond_broadcast(&exwait);
        }
        pthread_mutex_unlock(&exitmux);

        pthread_mutex_lock(&ncustmux);
    }
    pthread_mutex_unlock(&ncustmux);
    //PRINT("JOINING CUSTOMERS\n");
    for (int i = 0; i < cust_thread_size; i++)
    {
        pthread_join(customers[i], NULL);
    }
    //PRINT("ALL CUSTOMERS JOINED GG \n");
    for (int i = 0; i < c->K; i++)
    {
        PRINT("Cashier %d served : %d customers\n", i, cash_data[i].served_customers);
    }
    return NULL;
}

static void *cashiers_controller(void *arg)
{
    // cashier_data **cash_data = (cashier_data **)arg;
    pthread_t *cashiers = malloc(c->K * sizeof(pthread_t));
    // OPEN THE FIRST CASHIER
    int *cashier_ids = malloc(c->K * sizeof(int));
    for (int i = 0; i < c->K; i++)
    {
        cashier_ids[i] = i;
    }
    for (int i = 0; i < c->H; i++)
    {
        pthread_create(&(cashiers[i]), NULL, cashier, &cashier_ids[i]);
        num_cash++;
    }
    PRINT("First checkouts opened !\n");

    // prepare for nanosleep
    struct timespec tim;
    tim.tv_sec = 0;
    tim.tv_nsec = 30000000;
    int err;
    //end
    int last_action = 2;
    while (sigquit == 0)
    {
        //PRINT("STO ITERANDO =================\n");
        if (sighup == 1)
        {
            pthread_mutex_lock(&ncustmux);
            //PRINT("num_cust %d\n", num_cust);
            if (num_cust == 0)
            {
                pthread_mutex_unlock(&ncustmux);
                //PRINT("FAMO IR BREAK DER CONTROLLER DEE CASSE\n");
                break;
            }
            pthread_mutex_unlock(&ncustmux);
        }
        SYSCALL(err, nanosleep(&tim, NULL), "Cashier controller sleep failed\n");
        //PRINT("Cashier controller slept\n");

        int action = 0;
        int few = 0; // cashiers with <= 1 customers
        int i = 0;   // --> index of the cashier
        if (last_action == 1)
            last_action = 2;
        else
            last_action = 1;
        unsigned long current_time = get_cur_time();
        while (!action && i < c->K)
        {
            pthread_mutex_lock(&block[i]); // LOCK BUFFER
            //PRINT("buffer[%d]->open = %d, num_cust = %d\n", i, buffer[i]->open, buffer[i]->num_cust);
            if (buffer[i]->open == 1)
                if (abs(current_time - buffer[i]->timestamp) < 30)
                {
                    if (!action)
                        if (last_action == 1)
                            if (buffer[i]->num_cust < 2)
                                if ((++few) > 1)

                                {
                                    pthread_mutex_unlock(&block[i]); // UNLOCK BUFFER
                                    close_checkout(cashiers);
                                    action = 1;
                                }
                    if (!action)
                        if (last_action == 2)
                            if (buffer[i]->num_cust > 10)
                            {
                                pthread_mutex_unlock(&block[i]); // UNLOCK BUFFER
                                open_checkout(cashiers, cashier_ids);
                                action = 1;
                            }
                }
            pthread_mutex_unlock(&block[i]); // UNLOCK BUFFER
            i++;
        }
    }
    if (sigquit == 1)
    {
        //close_checkout(cashiers);
        for (int i = 0; i < c->K; i++)
        {
            int ok = 0;
            pthread_mutex_lock(&block[i]); // LOCK BUFFER
            if (buffer[i]->open == 1)
            {
                cust_in_q *end = malloc(sizeof(cust_in_q));
                end->id = -1;
                if (push(qu[i], end) == -1)
                {
                    PRINT("end message push error\n");
                }
                buffer[i]->open = 0;
                ok = 1;
            }
            pthread_mutex_unlock(&block[i]);
            if (ok)
                pthread_join(cashiers[i], NULL);
        }
    }
    if (sighup == 1)
    {
        for (int i = 0; i < c->K; i++)
        {
            int ok = 0;
            pthread_mutex_lock(&block[i]); // LOCK BUFFER
            if (buffer[i]->open == 1)
            {
                PRINT("CASHIER %d IS STILL ON\n", i);
                cust_in_q *end = malloc(sizeof(cust_in_q));
                end->id = -1;
                if (push(qu[i], end) == -1)
                {
                    PRINT("end message push error\n");
                }
                buffer[i]->open = 0;
                ok = 1;
            }
            pthread_mutex_unlock(&block[i]);
            if (ok)
                pthread_join(cashiers[i], NULL);
        }
    }
    return NULL;
}

void close_checkout(pthread_t *cashiers)
{
    if (sighup == 0 && sigquit == 0 && num_cash > 1)
    {
        //PRINT("GONNA CLOSE A CASHIER\n");
        int done = 0;
        int k;
        cust_in_q *tmp = NULL;
        while (!done)
        {
            int to_close = rand_r(&main_seed) % c->K;
            pthread_mutex_lock(&block[to_close]); // LOCK BUFFER
            if (buffer[to_close]->open == 1)
            {
                cust_in_q *end = malloc(sizeof(cust_in_q));
                end->id = -1;
                if (push(qu[to_close], end) == -1)
                {
                    PRINT("end message push error\n");
                }
                tmp = end;
                buffer[to_close]->open = 0;
                done = 1;
                k = to_close;
                num_cash--;
            }
            pthread_mutex_unlock(&block[to_close]); // UNLOCK BUFFER
        }
        //PRINT("GONNA JOIN CASHIER %d\n", k);
        pthread_join(cashiers[k], NULL);
        free(tmp);
        PRINT("CASHIER %d CLOSED\n", k);
    }
}

void open_checkout(pthread_t *cashiers, int *cashier_ids)
{
    //PRINT("WILL OPEN A CASHIER\n");
    if (num_cash < c->K)
    {
        int done = 0;
        int k;
        while (!done)
        {
            int to_open = rand_r(&main_seed) % c->K;
            pthread_mutex_lock(&block[to_open]); // LOCK BUFFER
            if (buffer[to_open]->open == 0)
            {
                buffer[to_open]->open = 1;
                done = 1;
                k = to_open;
                num_cash++;
            }
            pthread_mutex_unlock(&block[to_open]); // UNLOCK BUFFER
        }
        PRINT("OPENING CASHIER %d", k);
        pthread_create(&(cashiers[k]), NULL, cashier, &cashier_ids[k]);
    }
}

// ============== CUSTOMERS LIST AUX FUNCTIONS =========================
void insert_clist(cl_arg *arg)
{
    if (arg == NULL)
    {
        PRINT("Insert clist null arg\n");
        exit(EXIT_FAILURE);
    }
    clnode *tmp = malloc(sizeof(clnode));
    tmp->data = arg;
    tmp->next = NULL;
    if (clist_tail == NULL)
    {
        clist_head = tmp;
        clist_tail = clist_head;
    }
    else
    {
        clist_tail->next = tmp;
        clist_tail = tmp;
    }
}

// void print_clist()
// {
//     clnode *tmp = clist_head;
//     while (tmp != NULL)
//     {
//         cl_arg *tmp2 = tmp->data;
//         if (tmp2 == NULL)
//         {
//             PRINT("Null data inside clnode\n");
//             exit(EXIT_FAILURE);
//         }
//         PRINT("Customer: %d prod %d shoptime %d inqueue %lu total %lu visited %d\n", tmp2->id, tmp2->prod, tmp2->shop_time, tmp2->qtime, (tmp2->etime - tmp2->enter_time), tmp2->visited);
//         tmp = tmp->next;
//     }
// }
void print_clist(FILE *f)
{
    if (f == NULL)
    {
        fprintf(stderr, "Log file is NULL\n");
        exit(EXIT_FAILURE);
    }
    clnode *tmp = clist_head;
    while (tmp != NULL)
    {
        cl_arg *tmp2 = tmp->data;
        if (tmp2 == NULL)
        {
            fprintf(stderr, "Null data inside clnode\n");
            exit(EXIT_FAILURE);
        }
        fprintf(f, "Customer: %d || prods: %d || in queue %0.3f sec || total %0.3f sec || visited %d\n", tmp2->id, tmp2->prod, (double)(tmp2->qtime) / 1000, (double)(tmp2->etime - tmp2->enter_time) / 1000, tmp2->visited);
        free(tmp2);
        clist_head = clist_head->next;
        free(tmp);
        tmp = clist_head;
    }
    for (int i = 0; i < c->K; i++)
    {
        if (cash_data[i].served_customers > 0)
            fprintf(f, "Checkout: %d || prods: %d || served customers %d || tot. open time %0.3f sec || avg service time %0.3f sec || closed %d times\n", i, cash_data[i].passed_prods, cash_data[i].served_customers, (double)(cash_data[i].tot_open_time) / 1000, (double)((cash_data[i].tot_serv_time) / (cash_data[i].served_customers)) / 1000, cash_data[i].closure_num);
        else
            fprintf(f, "Checkout: %d || prods: %d || served customers %d || tot. open time %0.3f sec || avg service time 0.000 sec || closed %d times\n", i, cash_data[i].passed_prods, cash_data[i].served_customers, (double)(cash_data[i].tot_open_time) / 1000, cash_data[i].closure_num);
    }
    fclose(f);
    free(cash_data);
}

int check_config(config *conf)
{
    if (!(conf->P >= 0 && conf->T > 10 && (conf->E > 0 && conf->E < conf->C) && conf->K > 0 && conf->C > 1 && conf->S1 > 0 && conf->S2 > 0 && conf->S1 < conf->S2 && conf->H > 0 && conf->H <= conf->K && conf->G > 0 && conf->L > 0))
    {
        fprintf(stderr, "Invalid configuration settings, respect the following: P>=0, T>10, 0<E<C, K>0, C>1, S1>0, S2>0, S1<S2, H>0, H<=K, G>0, L>0 \n");
        fflush(stderr);
        return -1;
    }
    if (strlen(conf->log_name) == 0)
    {
        fprintf(stderr, "Invalid log file name \n");
        fflush(stderr);
        return -1;
    }
    else
        return 1;
}

config *parse_config(const char *conf)
{
    FILE *cfg;
    char *buffer;
    if ((cfg = fopen(conf, "r")) == NULL)
    {
        fclose(cfg);
        fprintf(stderr, "Config file fopen failed\n");
        return NULL;
    }
    if ((c = malloc(sizeof(config))) == NULL)
    {
        fclose(cfg);
        free(c);
        return NULL;
    }
    if ((buffer = malloc(BUFLEN * sizeof(char))) == NULL)
    {
        fclose(cfg);
        free(c);
        free(buffer);
        return NULL;
    }
    while (fgets(buffer, BUFLEN, cfg) != NULL)
    {
        char *var;
        char *val;
        char *rest = buffer;
        var = strtok_r(rest, "=", &rest);
        val = strtok_r(NULL, "=", &rest);
        if (strcmp("P", var) == 0)
            c->P = atoi(val);
        if (strcmp("T", var) == 0)
            c->T = atoi(val);
        if (strcmp("C", var) == 0)
            c->C = atoi(val);
        if (strcmp("E", var) == 0)
            c->E = atoi(val);
        if (strcmp("K", var) == 0)
            c->K = atoi(val);
        if (strcmp("S1", var) == 0)
            c->S1 = atoi(val);
        if (strcmp("S2", var) == 0)
            c->S2 = atoi(val);
        if (strcmp("H", var) == 0)
            c->H = atoi(val);
        if (strcmp("G", var) == 0)
            c->G = atoi(val);
        if (strcmp("L", var) == 0)
            c->L = atoi(val);
        if (strcmp("LOG", var) == 0)
        {
            c->log_name = malloc((strlen(val)) * sizeof(char));
            // int i=0;
            // while(val[i]!='\n'){
            //     c->log_name[i]=val[i];
            // }

            strncpy(c->log_name, val, strlen(val) - 1);
        }
    }
    if ((check_config(c)) == -1)
    {
        fprintf(stderr, "Config parsing failed\n");
        fflush(stderr);
        fclose(cfg);
        free(c->log_name);
        free(c);
        free(buffer);
        return NULL;
    }
    fclose(cfg);
    free(buffer);
    return c;
}
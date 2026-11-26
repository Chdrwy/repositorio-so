/* Camada de abstracao de threads do trabalho. A ideia e ter uma API so
 * (as funcoes pt_...) que funciona igual no Linux e no Windows. Por baixo,
 * no Linux a gente usa pthreads e no Windows a API Win32 nativa.
 * Importante: nada de winpthreads no Windows, o enunciado proibe.*/
 
#ifndef PLATFORM_THREAD_H
#define PLATFORM_THREAD_H

#include <stdint.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  typedef HANDLE              pt_thread_t;
  typedef CRITICAL_SECTION    pt_mutex_t;
  typedef CONDITION_VARIABLE  pt_cond_t;
  typedef HANDLE              pt_sem_t;
#else
  #include <pthread.h>
  #include <semaphore.h>
  typedef pthread_t           pt_thread_t;
  typedef pthread_mutex_t     pt_mutex_t;
  typedef pthread_cond_t      pt_cond_t;
  typedef sem_t               pt_sem_t;
#endif

/* Tipo da funcao que a thread vai rodar: recebe um void* e devolve um void*,
 * igual ao pthread. */
typedef void *(*pt_thread_func_t)(void *arg);

/* Combinado: todas as funcoes devolvem 0 quando da certo e algo diferente
 * de 0 quando da erro. */

/* Threads */
int pt_thread_create(pt_thread_t *thread, pt_thread_func_t funcao, void *argumento);
int pt_thread_join(pt_thread_t *thread);

/* Mutex */
int pt_mutex_init(pt_mutex_t *mutex);
int pt_mutex_lock(pt_mutex_t *mutex);
int pt_mutex_unlock(pt_mutex_t *mutex);
int pt_mutex_destroy(pt_mutex_t *mutex);

/* Variavel de condicao */
int pt_cond_init(pt_cond_t *cond);
int pt_cond_wait(pt_cond_t *cond, pt_mutex_t *mutex);  /* tem que chamar com o mutex travado */
int pt_cond_signal(pt_cond_t *cond);                   /* acorda uma thread que esta esperando */
int pt_cond_broadcast(pt_cond_t *cond);                /* acorda todas */
int pt_cond_destroy(pt_cond_t *cond);

/* Semaforo contador.
 * valor_inicial e o valor que o semaforo comeca. valor_maximo so existe
 * porque o Windows obriga a passar um maximo; no Linux ele e ignorado. */
int pt_sem_init(pt_sem_t *sem, unsigned int valor_inicial, unsigned int valor_maximo);
int pt_sem_wait(pt_sem_t *sem);   /* diminui 1; se estiver em 0, fica bloqueado */
int pt_sem_post(pt_sem_t *sem);   /* aumenta 1 e acorda alguem que estiver esperando */
int pt_sem_destroy(pt_sem_t *sem);

/* Outras coisas */
uint64_t pt_time_ns(void);      /* relogio monotonico em nanossegundos */
int      pt_num_cpus(void);     /* quantos processadores logicos a maquina tem */

#endif

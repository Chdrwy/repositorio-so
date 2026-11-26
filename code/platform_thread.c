/* nesse mesmo arquivo temos as duas versoes:
 * a parte de dentro do #ifdef _WIN32 e a do Windows (CreateThread,
 * CRITICAL_SECTION, CONDITION_VARIABLE, CreateSemaphore) e a parte do #else
 * e a do Linux (pthreads e semaforos POSIX). Sem winpthreads.*/
 
#ifndef _WIN32
  #define _POSIX_C_SOURCE 200809L   /* precisa disso pro clock_gettime aparecer */
#endif

#include "platform_thread.h"
#include <stdlib.h>

#ifdef _WIN32

/* ===================== Parte do Windows ===================== */

/* O CreateThread quer uma funcao no formato DWORD WINAPI f(LPVOID), que e
 * diferente do formato da nossa camada (void *f(void *)). Entao a gente
 * cria a thread numa funcao "trampolim", que so chama a funcao do usuario.
 * Essa struct leva a funcao e o argumento ate o trampolim. */
typedef struct {
    pt_thread_func_t funcao;
    void *argumento;
} dados_trampolim_t;

static DWORD WINAPI funcao_trampolim(LPVOID parametro)
{
    dados_trampolim_t *ponteiro_dados;
    dados_trampolim_t dados;

    /* copia os dados e ja libera a memoria que o pt_thread_create alocou */
    ponteiro_dados = (dados_trampolim_t *)parametro;
    dados = *ponteiro_dados;
    free(ponteiro_dados);

    dados.funcao(dados.argumento);
    return 0;
}

int pt_thread_create(pt_thread_t *thread, pt_thread_func_t funcao, void *argumento)
{
    dados_trampolim_t *dados;

    dados = (dados_trampolim_t *)malloc(sizeof(dados_trampolim_t));
    if (dados == NULL) {
        return 1;
    }
    dados->funcao = funcao;
    dados->argumento = argumento;

    *thread = CreateThread(NULL, 0, funcao_trampolim, dados, 0, NULL);
    if (*thread == NULL) {
        free(dados);
        return 1;
    }
    return 0;
}

int pt_thread_join(pt_thread_t *thread)
{
    DWORD resultado_espera;

    resultado_espera = WaitForSingleObject(*thread, INFINITE);
    if (resultado_espera != WAIT_OBJECT_0) {
        return 1;
    }
    CloseHandle(*thread);
    *thread = NULL;
    return 0;
}

int pt_mutex_init(pt_mutex_t *mutex)
{
    InitializeCriticalSection(mutex);
    return 0;
}

int pt_mutex_lock(pt_mutex_t *mutex)
{
    EnterCriticalSection(mutex);
    return 0;
}

int pt_mutex_unlock(pt_mutex_t *mutex)
{
    LeaveCriticalSection(mutex);
    return 0;
}

int pt_mutex_destroy(pt_mutex_t *mutex)
{
    DeleteCriticalSection(mutex);
    return 0;
}

int pt_cond_init(pt_cond_t *cond)
{
    InitializeConditionVariable(cond);
    return 0;
}

int pt_cond_wait(pt_cond_t *cond, pt_mutex_t *mutex)
{
    BOOL deu_certo;

    /* solta a critical section, dorme, e trava de novo quando acordar */
    deu_certo = SleepConditionVariableCS(cond, mutex, INFINITE);
    if (deu_certo) {
        return 0;
    } else {
        return 1;
    }
}

int pt_cond_signal(pt_cond_t *cond)
{
    WakeConditionVariable(cond);
    return 0;
}

int pt_cond_broadcast(pt_cond_t *cond)
{
    WakeAllConditionVariable(cond);
    return 0;
}

int pt_cond_destroy(pt_cond_t *cond)
{
    /* no Windows a variavel de condicao nao precisa ser destruida */
    (void)cond;
    return 0;
}

int pt_sem_init(pt_sem_t *sem, unsigned int valor_inicial, unsigned int valor_maximo)
{
    *sem = CreateSemaphore(NULL, (LONG)valor_inicial, (LONG)valor_maximo, NULL);
    if (*sem == NULL) {
        return 1;
    }
    return 0;
}

int pt_sem_wait(pt_sem_t *sem)
{
    DWORD resultado_espera;

    resultado_espera = WaitForSingleObject(*sem, INFINITE);
    if (resultado_espera == WAIT_OBJECT_0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_sem_post(pt_sem_t *sem)
{
    BOOL deu_certo;

    deu_certo = ReleaseSemaphore(*sem, 1, NULL);
    if (deu_certo) {
        return 0;
    } else {
        return 1;
    }
}

int pt_sem_destroy(pt_sem_t *sem)
{
    BOOL deu_certo;
    int retorno;

    deu_certo = CloseHandle(*sem);
    if (deu_certo) {
        retorno = 0;
    } else {
        retorno = 1;
    }
    *sem = NULL;
    return retorno;
}

/* A frequencia do contador so e lida na primeira vez que a funcao e chamada.
 * Se duas threads chamassem ao mesmo tempo na primeira vez, as duas iam
 * gravar o mesmo valor, entao nao tem problema. E de qualquer jeito o
 * benchmark e os testes chamam essa funcao na thread principal antes de
 * criar as outras. */
uint64_t pt_time_ns(void)
{
    static LARGE_INTEGER frequencia = {0};
    LARGE_INTEGER contador;
    uint64_t ticks;
    uint64_t ticks_por_segundo;
    uint64_t segundos;
    uint64_t resto;
    uint64_t nanossegundos;

    if (frequencia.QuadPart == 0) {
        QueryPerformanceFrequency(&frequencia);
    }
    QueryPerformanceCounter(&contador);

    ticks = (uint64_t)contador.QuadPart;
    ticks_por_segundo = (uint64_t)frequencia.QuadPart;

    /* se fizesse ticks * 1000000000 direto podia estourar os 64 bits,
     * entao separa a parte dos segundos inteiros e o que sobra */
    segundos = ticks / ticks_por_segundo;
    resto = ticks % ticks_por_segundo;
    nanossegundos = segundos * 1000000000ULL + (resto * 1000000000ULL) / ticks_por_segundo;
    return nanossegundos;
}

int pt_num_cpus(void)
{
    SYSTEM_INFO informacoes;

    GetSystemInfo(&informacoes);
    return (int)informacoes.dwNumberOfProcessors;
}

#else

/* ===================== Parte do Linux (POSIX) ===================== */

#include <errno.h>
#include <time.h>
#include <unistd.h>

int pt_thread_create(pt_thread_t *thread, pt_thread_func_t funcao, void *argumento)
{
    int resultado;

    resultado = pthread_create(thread, NULL, funcao, argumento);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_thread_join(pt_thread_t *thread)
{
    int resultado;

    resultado = pthread_join(*thread, NULL);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_mutex_init(pt_mutex_t *mutex)
{
    int resultado;

    resultado = pthread_mutex_init(mutex, NULL);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_mutex_lock(pt_mutex_t *mutex)
{
    int resultado;

    resultado = pthread_mutex_lock(mutex);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_mutex_unlock(pt_mutex_t *mutex)
{
    int resultado;

    resultado = pthread_mutex_unlock(mutex);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_mutex_destroy(pt_mutex_t *mutex)
{
    int resultado;

    resultado = pthread_mutex_destroy(mutex);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_cond_init(pt_cond_t *cond)
{
    int resultado;

    resultado = pthread_cond_init(cond, NULL);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_cond_wait(pt_cond_t *cond, pt_mutex_t *mutex)
{
    int resultado;

    resultado = pthread_cond_wait(cond, mutex);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_cond_signal(pt_cond_t *cond)
{
    int resultado;

    resultado = pthread_cond_signal(cond);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_cond_broadcast(pt_cond_t *cond)
{
    int resultado;

    resultado = pthread_cond_broadcast(cond);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_cond_destroy(pt_cond_t *cond)
{
    int resultado;

    resultado = pthread_cond_destroy(cond);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_sem_init(pt_sem_t *sem, unsigned int valor_inicial, unsigned int valor_maximo)
{
    int resultado;

    /* o semaforo POSIX nao tem valor maximo, entao esse parametro nao e usado */
    (void)valor_maximo;

    /* o 0 no meio quer dizer que o semaforo e so entre threads do mesmo processo */
    resultado = sem_init(sem, 0, valor_inicial);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_sem_wait(pt_sem_t *sem)
{
    int resultado;

    resultado = sem_wait(sem);
    /* se um sinal do sistema interromper a espera, o sem_wait volta com
     * EINTR sem ter pegado o semaforo. Nesse caso e so tentar de novo. */
    while (resultado == -1 && errno == EINTR) {
        resultado = sem_wait(sem);
    }

    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_sem_post(pt_sem_t *sem)
{
    int resultado;

    resultado = sem_post(sem);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

int pt_sem_destroy(pt_sem_t *sem)
{
    int resultado;

    resultado = sem_destroy(sem);
    if (resultado == 0) {
        return 0;
    } else {
        return 1;
    }
}

uint64_t pt_time_ns(void)
{
    struct timespec tempo;
    uint64_t nanossegundos;

    clock_gettime(CLOCK_MONOTONIC, &tempo);
    nanossegundos = (uint64_t)tempo.tv_sec * 1000000000ULL + (uint64_t)tempo.tv_nsec;
    return nanossegundos;
}

int pt_num_cpus(void)
{
    long quantidade;

    quantidade = sysconf(_SC_NPROCESSORS_ONLN);
    if (quantidade > 0) {
        return (int)quantidade;
    } else {
        return 1;
    }
}

#endif

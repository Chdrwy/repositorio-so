/* teste_plataforma.c
 * Testa cada operacao da camada platform_thread na plataforma em que o
 * programa estiver rodando. Cada teste confere o resultado de verdade (nao
 * so se "rodou sem erro"). Nos testes de bloqueio a gente confere que a
 * thread realmente ficou parada ANTES de mandar o post ou o signal.
 * Tem tambem uma thread vigia: se algum teste travar, ela mostra FALHA e
 * encerra o programa, em vez de deixar tudo parado para sempre.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "platform_thread.h"

#define NUM_THREADS     4
#define NUM_INCREMENTOS 100000
#define NUM_ESPERANDO   3          /* quantas threads esperam no teste de broadcast */

static int falhas = 0;

/* mostra [OK] ou [FALHA] e conta as falhas */
static void verifica(int condicao, const char *mensagem)
{
    if (condicao) {
        printf("[OK]    %s\n", mensagem);
    } else {
        printf("[FALHA] %s\n", mensagem);
        falhas++;
    }
    fflush(stdout);
}

/* Espera ocupada (a camada nao tem sleep). So serve para dar tempo de uma
 * thread chegar no ponto em que ela bloqueia. */
static void espera_ms(int milissegundos)
{
    uint64_t fim;

    fim = pt_time_ns() + (uint64_t)milissegundos * 1000000ULL;
    while (pt_time_ns() < fim) {
        /* nao faz nada, so espera o tempo passar */
    }
}

/* ---------------------------------------------------------------- */
/* Vigia: se a etapa atual passar do prazo, mostra FALHA e encerra. */
/* ---------------------------------------------------------------- */
static volatile uint64_t vigia_prazo_ns = 0;
static const char *volatile vigia_etapa = "inicio";   /* o volatile vale para o ponteiro */
static volatile int vigia_terminar = 0;

/* avisa o vigia qual teste esta rodando agora e quanto tempo ele tem */
static void comeca_etapa(const char *nome, int segundos)
{
    vigia_etapa = nome;
    vigia_prazo_ns = pt_time_ns() + (uint64_t)segundos * 1000000000ULL;
}

static void *thread_vigia(void *argumento)
{
    (void)argumento;

    while (!vigia_terminar) {
        if (vigia_prazo_ns != 0 && pt_time_ns() > vigia_prazo_ns) {
            printf("[FALHA] TRAVOU (prazo esgotado) em: %s\n", (const char *)vigia_etapa);
            fflush(stdout);
            exit(2);
        }
        espera_ms(20);
    }
    return NULL;
}

/* ---- Teste 1: o mutex protege um contador compartilhado ---- */
static pt_mutex_t mutex_contador;
static long contador = 0;

static void *incrementa_contador(void *argumento)
{
    int i;

    (void)argumento;
    for (i = 0; i < NUM_INCREMENTOS; i++) {
        pt_mutex_lock(&mutex_contador);
        contador++;
        pt_mutex_unlock(&mutex_contador);
    }
    return NULL;
}

/* ---- Teste 2: thread bloqueada no semaforo ---- */
static pt_sem_t semaforo_bloqueio;
static volatile int passou_do_semaforo = 0;

static void *espera_no_semaforo(void *argumento)
{
    (void)argumento;
    pt_sem_wait(&semaforo_bloqueio);
    passou_do_semaforo = 1;
    return NULL;
}

/* ---- Teste 3: variavel de condicao (sempre com while) ---- */
static pt_mutex_t mutex_cond;
static pt_cond_t  variavel_cond;
static int pronto = 0;
static volatile int acordou = 0;

static void *espera_na_condicao(void *argumento)
{
    (void)argumento;
    pt_mutex_lock(&mutex_cond);
    while (!pronto) {
        pt_cond_wait(&variavel_cond, &mutex_cond);
    }
    acordou = 1;
    pt_mutex_unlock(&mutex_cond);
    return NULL;
}

/* ---- Teste 4: signal libera uma "ficha", broadcast libera todas ---- */
static int fichas = 0;            /* so mexe com o mutex_cond travado */
static int liberado = 0;          /* so mexe com o mutex_cond travado */
static volatile int passaram = 0; /* so mexe com o mutex_cond travado */

static void *espera_uma_ficha(void *argumento)
{
    (void)argumento;
    pt_mutex_lock(&mutex_cond);
    while (fichas == 0) {
        pt_cond_wait(&variavel_cond, &mutex_cond);
    }
    fichas--;
    passaram++;
    pt_mutex_unlock(&mutex_cond);
    return NULL;
}

static void *espera_liberacao(void *argumento)
{
    (void)argumento;
    pt_mutex_lock(&mutex_cond);
    while (!liberado) {
        pt_cond_wait(&variavel_cond, &mutex_cond);
    }
    passaram++;
    pt_mutex_unlock(&mutex_cond);
    return NULL;
}

/* le quantas threads ja passaram, com o mutex travado */
static int quantas_passaram(void)
{
    int quantidade;

    pt_mutex_lock(&mutex_cond);
    quantidade = passaram;
    pt_mutex_unlock(&mutex_cond);
    return quantidade;
}

int main(void)
{
    pt_thread_t threads[NUM_THREADS];
    pt_thread_t vigia;
    pt_thread_t thread_semaforo;
    pt_thread_t thread_cond;
    pt_thread_t threads_esperando[NUM_ESPERANDO];
    pt_sem_t semaforo;
    int num_cpus;
    int tudo_certo;
    int i;
    uint64_t leitura_anterior;
    uint64_t leitura_atual;
    time_t segundo_inicial;
    time_t segundo_medido;
    uint64_t inicio_medicao;
    uint64_t fim_medicao;
    double segundos_medidos;
    char mensagem[120];

    if (pt_thread_create(&vigia, thread_vigia, NULL) != 0) {
        printf("[FALHA] nao foi possivel criar o vigia\n");
        return 1;
    }

    /* Teste 0: numero de CPUs */
    num_cpus = pt_num_cpus();
    printf("CPUs logicas detectadas: %d\n\n", num_cpus);
    verifica(num_cpus >= 1, "pt_num_cpus devolve pelo menos 1");

    /* Teste 1: criar/juntar threads e mutex */
    comeca_etapa("mutex com 4 threads", 20);
    tudo_certo = 1;
    if (pt_mutex_init(&mutex_contador) != 0) {
        tudo_certo = 0;
    }
    for (i = 0; i < NUM_THREADS; i++) {
        if (pt_thread_create(&threads[i], incrementa_contador, NULL) != 0) {
            tudo_certo = 0;
        }
    }
    for (i = 0; i < NUM_THREADS; i++) {
        if (pt_thread_join(&threads[i]) != 0) {
            tudo_certo = 0;
        }
    }
    verifica(tudo_certo, "criar e juntar 4 threads");
    verifica(contador == (long)NUM_THREADS * NUM_INCREMENTOS, "mutex: contador final = 400000 (sem perda)");
    verifica(pt_mutex_destroy(&mutex_contador) == 0, "mutex: destroy devolve 0");

    /* Teste 2a: semaforo contador sem bloquear.
     * Cada passo so e feito se os anteriores deram certo. */
    comeca_etapa("semaforo sem bloqueio", 5);
    tudo_certo = 1;
    if (pt_sem_init(&semaforo, 2, 10) != 0) {
        tudo_certo = 0;
    }
    /* 2 waits (o semaforo comeca em 2, entao nao bloqueia) */
    for (i = 0; i < 2; i++) {
        if (tudo_certo) {
            if (pt_sem_wait(&semaforo) != 0) {
                tudo_certo = 0;
            }
        }
    }
    /* 3 posts */
    for (i = 0; i < 3; i++) {
        if (tudo_certo) {
            if (pt_sem_post(&semaforo) != 0) {
                tudo_certo = 0;
            }
        }
    }
    /* mais 3 waits (agora o semaforo esta em 3) */
    for (i = 0; i < 3; i++) {
        if (tudo_certo) {
            if (pt_sem_wait(&semaforo) != 0) {
                tudo_certo = 0;
            }
        }
    }
    verifica(tudo_certo, "semaforo contador: 2 waits, 3 posts, 3 waits sem travar");
    verifica(pt_sem_destroy(&semaforo) == 0, "semaforo: destroy devolve 0");

    /* Teste 2b: semaforo em 0 bloqueia; o post de outra thread libera */
    comeca_etapa("semaforo bloqueante", 10);
    pt_sem_init(&semaforo_bloqueio, 0, 10);
    pt_thread_create(&thread_semaforo, espera_no_semaforo, NULL);
    espera_ms(100);
    verifica(passou_do_semaforo == 0, "semaforo em 0: thread fica bloqueada no wait");
    pt_sem_post(&semaforo_bloqueio);
    pt_thread_join(&thread_semaforo);
    verifica(passou_do_semaforo == 1, "semaforo: post libera a thread bloqueada");
    pt_sem_destroy(&semaforo_bloqueio);

    /* Teste 3: o wait fica bloqueado ate o signal */
    comeca_etapa("variavel de condicao: signal", 10);
    pt_mutex_init(&mutex_cond);
    pt_cond_init(&variavel_cond);
    pt_thread_create(&thread_cond, espera_na_condicao, NULL);
    espera_ms(100);
    verifica(acordou == 0, "variavel de condicao: thread fica esperando no wait");
    pt_mutex_lock(&mutex_cond);
    pronto = 1;
    pt_cond_signal(&variavel_cond);
    pt_mutex_unlock(&mutex_cond);
    pt_thread_join(&thread_cond);
    verifica(acordou == 1, "variavel de condicao: signal acorda a thread, que segue");

    /* Teste 4a: com 3 threads esperando, um signal com 1 ficha libera so 1 */
    comeca_etapa("variavel de condicao: signal com 3 esperando", 10);
    passaram = 0;
    fichas = 0;
    for (i = 0; i < NUM_ESPERANDO; i++) {
        pt_thread_create(&threads_esperando[i], espera_uma_ficha, NULL);
    }
    espera_ms(100);
    verifica(quantas_passaram() == 0, "3 threads esperando: nenhuma passa sem ficha");
    pt_mutex_lock(&mutex_cond);
    fichas = 1;
    pt_cond_signal(&variavel_cond);
    pt_mutex_unlock(&mutex_cond);
    espera_ms(100);
    verifica(quantas_passaram() == 1, "signal + 1 ficha: exatamente 1 thread passa");
    pt_mutex_lock(&mutex_cond);
    fichas = NUM_ESPERANDO - 1;
    pt_cond_broadcast(&variavel_cond);
    pt_mutex_unlock(&mutex_cond);
    for (i = 0; i < NUM_ESPERANDO; i++) {
        pt_thread_join(&threads_esperando[i]);
    }
    verifica(quantas_passaram() == NUM_ESPERANDO, "broadcast + fichas restantes: as outras passam");

    /* Teste 4b: um broadcast acorda TODAS. Se acordasse so uma, as outras
     * ficavam presas e o vigia acusava a falha. */
    comeca_etapa("variavel de condicao: broadcast acorda todas", 10);
    passaram = 0;
    liberado = 0;
    for (i = 0; i < NUM_ESPERANDO; i++) {
        pt_thread_create(&threads_esperando[i], espera_liberacao, NULL);
    }
    espera_ms(100);
    verifica(quantas_passaram() == 0, "3 threads esperando a liberacao");
    pt_mutex_lock(&mutex_cond);
    liberado = 1;
    pt_cond_broadcast(&variavel_cond);      /* so UM broadcast, nenhum signal */
    pt_mutex_unlock(&mutex_cond);
    for (i = 0; i < NUM_ESPERANDO; i++) {
        pt_thread_join(&threads_esperando[i]);
    }
    verifica(quantas_passaram() == NUM_ESPERANDO, "um unico broadcast acorda as 3 threads");
    verifica(pt_cond_destroy(&variavel_cond) == 0, "variavel de condicao: destroy devolve 0");
    verifica(pt_mutex_destroy(&mutex_cond) == 0, "mutex da condicao: destroy devolve 0");

    /* Teste 5: o relogio nao anda para tras e esta mesmo em nanossegundos */
    comeca_etapa("relogio", 10);
    tudo_certo = 1;
    leitura_anterior = pt_time_ns();
    for (i = 0; i < 1000000; i++) {
        leitura_atual = pt_time_ns();
        if (leitura_atual < leitura_anterior) {
            tudo_certo = 0;
            break;
        }
        leitura_anterior = leitura_atual;
    }
    verifica(tudo_certo && leitura_anterior > 0, "relogio: 1 milhao de leituras sem andar para tras");

    /* compara 1 segundo do relogio normal do sistema (time) com o pt_time_ns */
    segundo_inicial = time(NULL);
    while (time(NULL) == segundo_inicial) {
        /* espera virar o segundo, para comecar certinho no inicio de um */
    }
    segundo_medido = time(NULL);
    inicio_medicao = pt_time_ns();
    while (time(NULL) == segundo_medido) {
        /* espera passar esse segundo inteiro */
    }
    fim_medicao = pt_time_ns();
    segundos_medidos = (double)(fim_medicao - inicio_medicao) / 1e9;
    snprintf(mensagem, sizeof(mensagem), "relogio: 1 s de parede = %.3f s medidos (esperado 0,9 a 1,1)", segundos_medidos);
    verifica(segundos_medidos > 0.9 && segundos_medidos < 1.1, mensagem);

    vigia_terminar = 1;
    pt_thread_join(&vigia);

    if (falhas == 0) {
        printf("\n%s (%d falha(s))\n", "TUDO CERTO", falhas);
        return 0;
    } else {
        printf("\n%s (%d falha(s))\n", "HA FALHAS", falhas);
        return 1;
    }
}

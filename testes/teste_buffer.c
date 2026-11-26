/* Testes do buffer. Como usar: teste_buffer A   ou   teste_buffer B
 * O que e testado: ordem FIFO, buffer circular, bloqueio com buffer cheio,
 * bloqueio com buffer vazio, close acordando consumidores que estavam
 * bloqueados, terminar sem travar, e o teste de concorrencia (nenhum item
 * perdido ou repetido, ordem dos itens de cada produtor, termino).
 * Tem uma thread vigia: se algum teste travar, ela mostra FALHA e encerra.*/
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_thread.h"
#include "buffer.h"

#define TOTAL_ITENS 200000
#define MAX_THREADS 8

static int falhas = 0;
static sync_kind_t versao_testada = SYNC_A_COND;

/* mostra [OK] ou [FALHA] e conta as falhas */
static void verifica(int condicao, const char *mensagem)
{
    if (condicao) {
        printf("[OK]    %s\n", mensagem);
    } else {
        printf("[FALHA] %s\n", mensagem);
        falhas++;
    }
}

/* espera ocupada, so para dar tempo de uma thread chegar onde ela bloqueia */
static void espera_ms(int milissegundos)
{
    uint64_t fim;

    fim = pt_time_ns() + (uint64_t)milissegundos * 1000000ULL;
    while (pt_time_ns() < fim) {
        /* nao faz nada, so espera */
    }
}

/* ---------------------------------------------------------------- */
/* Vigia: se a etapa atual passar do prazo, mostra FALHA e encerra. */
/* ---------------------------------------------------------------- */
static volatile uint64_t vigia_prazo_ns = 0;
static const char *volatile vigia_etapa = "inicio";   /* o volatile vale para o ponteiro */
static volatile int vigia_terminar = 0;

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

/* ---------------------------------------------------------------- */
/* Testes basicos (com uma thread so, mais uma ajudante as vezes)    */
/* ---------------------------------------------------------------- */
typedef struct {
    buffer_t *buffer;
    volatile int terminou;
} args_produtor_extra_t;

static void *produtor_extra(void *parametro)
{
    args_produtor_extra_t *args;
    item_t item;

    args = (args_produtor_extra_t *)parametro;
    item.id = 99;
    item.t_prod_ns = 0;
    item.t_cons_ns = 0;

    buffer_put(args->buffer, &item);   /* aqui tem que bloquear: o buffer esta cheio */
    args->terminou = 1;
    return NULL;
}

typedef struct {
    buffer_t *buffer;
    volatile int terminou;
    int retorno;
    item_t item;
} args_consumidor_unico_t;

static void *consumidor_unico(void *parametro)
{
    args_consumidor_unico_t *args;

    args = (args_consumidor_unico_t *)parametro;
    args->retorno = buffer_get(args->buffer, &args->item);   /* tem que bloquear: buffer vazio */
    args->terminou = 1;
    return NULL;
}

static void teste_vazio_e_close(void)
{
    buffer_t *buffer;
    args_consumidor_unico_t args_um;
    args_consumidor_unico_t args_tres[3];
    pt_thread_t thread_um;
    pt_thread_t threads_tres[3];
    item_t item;
    int parados;
    int terminaram_certo;
    int i;

    /* buffer vazio: o get fica bloqueado ate chegar um item */
    comeca_etapa("buffer vazio: get bloqueia ate o put", 10);
    buffer = buffer_create(versao_testada, 4, 0);
    verifica(buffer != NULL, "criar buffer de capacidade 4 (teste de vazio)");
    if (buffer == NULL) {
        return;
    }
    args_um.buffer = buffer;
    args_um.terminou = 0;
    args_um.retorno = -9;
    args_um.item.id = 0;
    args_um.item.t_prod_ns = 0;
    args_um.item.t_cons_ns = 0;
    pt_thread_create(&thread_um, consumidor_unico, &args_um);
    espera_ms(150);
    verifica(args_um.terminou == 0, "buffer vazio: consumidor fica bloqueado no get");

    item.id = 7;
    item.t_prod_ns = 0;
    item.t_cons_ns = 0;
    buffer_put(buffer, &item);
    pt_thread_join(&thread_um);
    verifica(args_um.terminou == 1 && args_um.retorno == 1 && args_um.item.id == 7,
             "buffer vazio: put libera o consumidor com o item certo");
    buffer_close(buffer, 1);
    buffer_destroy(buffer);

    /* 3 consumidores bloqueados num buffer vazio: o close tem que acordar
     * TODOS e todos tem que receber fim (0) */
    comeca_etapa("close acorda consumidores bloqueados", 10);
    buffer = buffer_create(versao_testada, 4, 0);
    verifica(buffer != NULL, "criar buffer de capacidade 4 (teste de close)");
    if (buffer == NULL) {
        return;
    }
    for (i = 0; i < 3; i++) {
        args_tres[i].buffer = buffer;
        args_tres[i].terminou = 0;
        args_tres[i].retorno = -9;
        pt_thread_create(&threads_tres[i], consumidor_unico, &args_tres[i]);
    }
    espera_ms(150);

    parados = 0;
    for (i = 0; i < 3; i++) {
        if (args_tres[i].terminou == 0) {
            parados++;
        }
    }
    verifica(parados == 3, "3 consumidores bloqueados em buffer vazio");

    buffer_close(buffer, 3);
    for (i = 0; i < 3; i++) {
        pt_thread_join(&threads_tres[i]);
    }

    terminaram_certo = 0;
    for (i = 0; i < 3; i++) {
        if (args_tres[i].terminou == 1 && args_tres[i].retorno == 0) {
            terminaram_certo++;
        }
    }
    verifica(terminaram_certo == 3, "close acorda os 3 consumidores e todos recebem fim (0)");
    buffer_destroy(buffer);
}

static void teste_basico(void)
{
    buffer_t *buffer;
    item_t item;
    item_t saida;
    args_produtor_extra_t args_extra;
    pt_thread_t thread_extra;
    int tudo_certo;
    int retirados;
    int i;

    item.id = 0;
    item.t_prod_ns = 0;
    item.t_cons_ns = 0;

    buffer = buffer_create(versao_testada, 4, 0);
    verifica(buffer != NULL, "criar buffer de capacidade 4");
    if (buffer == NULL) {
        return;
    }

    /* coloca 3 e tira 3: tem que sair na mesma ordem (FIFO).
     * Cada operacao so e feita se as anteriores deram certo (se um get
     * fosse chamado com o buffer vazio, ele ia travar). */
    tudo_certo = 1;
    for (i = 0; i < 3; i++) {
        item.id = i;
        if (tudo_certo) {
            if (buffer_put(buffer, &item) != 0) {
                tudo_certo = 0;
            }
        }
    }
    for (i = 0; i < 3; i++) {
        if (tudo_certo) {
            if (buffer_get(buffer, &saida) != 1) {
                tudo_certo = 0;
            } else if (saida.id != i) {
                tudo_certo = 0;
            }
        }
    }
    verifica(tudo_certo, "inserir 3 e retirar 3 em ordem FIFO");

    /* os indices agora estao na posicao 3, entao colocar mais 3 itens faz
     * o indice dar a volta no vetor */
    tudo_certo = 1;
    for (i = 10; i < 13; i++) {
        item.id = i;
        if (tudo_certo) {
            if (buffer_put(buffer, &item) != 0) {
                tudo_certo = 0;
            }
        }
    }
    for (i = 10; i < 13; i++) {
        if (tudo_certo) {
            if (buffer_get(buffer, &saida) != 1) {
                tudo_certo = 0;
            } else if (saida.id != i) {
                tudo_certo = 0;
            }
        }
    }
    verifica(tudo_certo, "comportamento circular (indice volta ao inicio)");

    /* buffer vazio e fechado: o get tem que devolver 0 sem travar */
    buffer_close(buffer, 1);
    verifica(buffer_get(buffer, &saida) == 0, "buffer vazio e fechado: get devolve fim sem travar");
    buffer_destroy(buffer);

    /* buffer cheio: o produtor extra tem que ficar bloqueado */
    buffer = buffer_create(versao_testada, 2, 0);
    verifica(buffer != NULL, "criar buffer de capacidade 2");
    if (buffer == NULL) {
        return;
    }
    item.id = 1;
    buffer_put(buffer, &item);
    item.id = 2;
    buffer_put(buffer, &item);

    args_extra.buffer = buffer;
    args_extra.terminou = 0;
    pt_thread_create(&thread_extra, produtor_extra, &args_extra);
    espera_ms(150);
    verifica(args_extra.terminou == 0, "buffer cheio: produtor fica bloqueado");

    buffer_get(buffer, &saida);          /* abre uma vaga */
    pt_thread_join(&thread_extra);
    verifica(args_extra.terminou == 1, "buffer cheio: produtor prossegue quando surge vaga");

    retirados = 0;
    while (retirados < 2) {
        if (buffer_get(buffer, &saida) == 1) {
            retirados++;
        } else {
            break;
        }
    }
    verifica(retirados == 2, "itens restantes retirados");
    buffer_destroy(buffer);
}

/* ---------------------------------------------------------------- */
/* Teste de concorrencia                                            */
/* ---------------------------------------------------------------- */
typedef struct {
    buffer_t *buffer;
    int64_t primeiro_id;
    int64_t quantidade;
    int64_t produzidos;
    int erro;
} args_produtor_t;

typedef struct {
    buffer_t *buffer;
    int64_t itens_por_produtor;
    int64_t consumidos;
    int64_t erros_latencia;
    unsigned char *vezes_visto;       /* vezes_visto[id] = quantas vezes esse id chegou */
    int64_t ultimo_id[MAX_THREADS];   /* ultimo id que chegou de cada produtor */
    int ordem_certa;
    int erro;
} args_consumidor_t;

static void *thread_produtora(void *parametro)
{
    args_produtor_t *args;
    item_t item;
    int64_t k;

    args = (args_produtor_t *)parametro;
    item.id = 0;
    item.t_prod_ns = 0;
    item.t_cons_ns = 0;

    for (k = 0; k < args->quantidade; k++) {
        item.id = args->primeiro_id + k;
        if (buffer_put(args->buffer, &item) != 0) {
            args->erro = 1;
            break;
        }
        args->produzidos++;
    }
    return NULL;
}

static void *thread_consumidora(void *parametro)
{
    args_consumidor_t *args;
    item_t item;
    int resultado_get;
    int produtor_do_item;

    args = (args_consumidor_t *)parametro;

    resultado_get = buffer_get(args->buffer, &item);
    while (resultado_get == 1) {
        if (item.id < 0 || item.id >= TOTAL_ITENS) {
            args->erro = 1;
        } else {
            args->vezes_visto[item.id]++;

            /* os itens de um mesmo produtor tem que chegar em ordem crescente */
            produtor_do_item = (int)(item.id / args->itens_por_produtor);
            if (item.id <= args->ultimo_id[produtor_do_item]) {
                args->ordem_certa = 0;
            }
            args->ultimo_id[produtor_do_item] = item.id;

            if (args->buffer->medir_latencia && item.t_cons_ns < item.t_prod_ns) {
                args->erros_latencia++;
            }
            args->consumidos++;
        }

        resultado_get = buffer_get(args->buffer, &item);
    }

    if (resultado_get < 0) {
        args->erro = 1;
    }
    return NULL;
}

static void teste_concorrencia(int num_produtores, int num_consumidores, int capacidade, int medir_latencia)
{
    /* static porque o vigia guarda o ponteiro desse nome e le depois */
    static char nome_teste[160];
    char mensagem[220];
    const char *letra_versao;
    const char *texto_latencia;
    buffer_t *buffer;
    pt_thread_t threads_prod[MAX_THREADS];
    pt_thread_t threads_cons[MAX_THREADS];
    args_produtor_t args_prod[MAX_THREADS];
    args_consumidor_t args_cons[MAX_THREADS];
    int64_t itens_por_produtor;
    int64_t total_produzido;
    int64_t total_consumido;
    int64_t total_erros_latencia;
    int64_t id;
    uint64_t tempo_inicio;
    double segundos;
    int tudo_certo;
    int soma_vezes;
    int p;
    int c;
    int i;

    if (versao_testada == SYNC_A_COND) {
        letra_versao = "A";
    } else {
        letra_versao = "B";
    }
    if (medir_latencia) {
        texto_latencia = " (com latencia)";
    } else {
        texto_latencia = "";
    }
    snprintf(nome_teste, sizeof(nome_teste), "%s %dx%d buffer=%d%s",
             letra_versao, num_produtores, num_consumidores, capacidade, texto_latencia);
    comeca_etapa(nome_teste, 120);

    buffer = buffer_create(versao_testada, capacidade, medir_latencia);
    if (buffer == NULL) {
        verifica(0, nome_teste);
        return;
    }

    itens_por_produtor = TOTAL_ITENS / num_produtores;
    tudo_certo = 1;

    memset(args_prod, 0, sizeof(args_prod));
    memset(args_cons, 0, sizeof(args_cons));
    for (c = 0; c < num_consumidores; c++) {
        args_cons[c].buffer = buffer;
        args_cons[c].itens_por_produtor = itens_por_produtor;
        args_cons[c].ordem_certa = 1;
        args_cons[c].vezes_visto = (unsigned char *)calloc(TOTAL_ITENS, 1);
        for (i = 0; i < MAX_THREADS; i++) {
            args_cons[c].ultimo_id[i] = -1;
        }
    }

    tempo_inicio = pt_time_ns();
    for (c = 0; c < num_consumidores; c++) {
        if (pt_thread_create(&threads_cons[c], thread_consumidora, &args_cons[c]) != 0) {
            tudo_certo = 0;
        }
    }
    for (p = 0; p < num_produtores; p++) {
        args_prod[p].buffer = buffer;
        args_prod[p].primeiro_id = (int64_t)p * itens_por_produtor;
        args_prod[p].quantidade = itens_por_produtor;
        if (pt_thread_create(&threads_prod[p], thread_produtora, &args_prod[p]) != 0) {
            tudo_certo = 0;
        }
    }
    for (p = 0; p < num_produtores; p++) {
        pt_thread_join(&threads_prod[p]);
    }
    buffer_close(buffer, num_consumidores);     /* a producao acabou */
    for (c = 0; c < num_consumidores; c++) {
        pt_thread_join(&threads_cons[c]);
    }
    segundos = (double)(pt_time_ns() - tempo_inicio) / 1e9;

    /* junta o que cada thread contou */
    total_produzido = 0;
    total_consumido = 0;
    total_erros_latencia = 0;
    for (p = 0; p < num_produtores; p++) {
        total_produzido = total_produzido + args_prod[p].produzidos;
        if (args_prod[p].erro) {
            tudo_certo = 0;
        }
    }
    for (c = 0; c < num_consumidores; c++) {
        total_consumido = total_consumido + args_cons[c].consumidos;
        total_erros_latencia = total_erros_latencia + args_cons[c].erros_latencia;
        if (!args_cons[c].ordem_certa) {
            tudo_certo = 0;
        }
        if (args_cons[c].erro) {
            tudo_certo = 0;
        }
        if (args_cons[c].vezes_visto == NULL) {
            tudo_certo = 0;
        }
    }
    if (total_produzido != TOTAL_ITENS) {
        tudo_certo = 0;
    }
    if (total_consumido != TOTAL_ITENS) {
        tudo_certo = 0;
    }
    if (total_erros_latencia != 0) {
        tudo_certo = 0;
    }

    /* cada id tem que ter chegado exatamente uma vez (nem perdido, nem repetido).
     * So confere se ate aqui esta tudo certo (se um vetor vezes_visto for
     * NULL, tudo_certo ja e 0 e o laco nem comeca). */
    for (id = 0; id < TOTAL_ITENS; id++) {
        if (!tudo_certo) {
            break;
        }
        soma_vezes = 0;
        for (c = 0; c < num_consumidores; c++) {
            soma_vezes = soma_vezes + args_cons[c].vezes_visto[id];
        }
        if (soma_vezes != 1) {
            tudo_certo = 0;
        }
    }

    snprintf(mensagem, sizeof(mensagem), "%s (prod=%lld cons=%lld, %.3f s)",
             nome_teste, (long long)total_produzido, (long long)total_consumido, segundos);
    verifica(tudo_certo, mensagem);

    for (c = 0; c < num_consumidores; c++) {
        free(args_cons[c].vezes_visto);
    }
    buffer_destroy(buffer);
}

int main(int argc, char **argv)
{
    pt_thread_t vigia;
    int capacidades[3];
    int i;

    if (argc > 1) {
        if (argv[1][0] == 'B' || argv[1][0] == 'b') {
            versao_testada = SYNC_B_SEM;
        }
    }

    if (versao_testada == SYNC_A_COND) {
        printf("=== Testes do buffer: versao %s ===\n\n", "A (variaveis de condicao)");
    } else {
        printf("=== Testes do buffer: versao %s ===\n\n", "B (semaforos)");
    }

    if (pt_thread_create(&vigia, thread_vigia, NULL) != 0) {
        printf("[FALHA] nao foi possivel criar o vigia\n");
        return 1;
    }

    comeca_etapa("testes basicos", 10);
    teste_basico();
    teste_vazio_e_close();
    printf("\n--- Concorrencia (%d itens por teste) ---\n", TOTAL_ITENS);

    capacidades[0] = 1;
    capacidades[1] = 16;
    capacidades[2] = 256;
    for (i = 0; i < 3; i++) {
        teste_concorrencia(1, 1, capacidades[i], 0);
        teste_concorrencia(4, 4, capacidades[i], 0);
    }
    teste_concorrencia(1, 1, 1, 1);   /* mesma configuracao do modo latencia, com o relogio ligado */

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

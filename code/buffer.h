/* Buffer circular limitado (tamanho fixo), com dois jeitos de sincronizar:
 *   Versao A: um mutex + duas variaveis de condicao
 *   Versao B: dois semaforos contadores + um mutex
 * A versao e escolhida no buffer_create(). Depois disso quem usa o buffer
 * (produtores, consumidores, benchmark) chama sempre as mesmas funcoes e nao
 * precisa saber qual versao esta rodando.
 */
#ifndef BUFFER_H
#define BUFFER_H

#include <stdint.h>
#include "platform_thread.h"

/* item especial que avisa o consumidor que acabou (so a versao B usa) */
#define ITEM_SENTINELA ((int64_t)-1)

/* Como a gente mede a latencia:
 *   latencia = t_cons_ns - t_prod_ns
 *   t_prod_ns e o instante em que o item foi gravado no buffer (dentro do put,
 *   com o mutex travado) e t_cons_ns e o instante em que ele foi tirado do
 *   buffer (dentro do get, tambem com o mutex travado).
 * Ou seja, e o tempo que o item ficou parado dentro do buffer. O tempo que o
 * produtor ficou esperando vaga nao entra. Isso so e medido quando
 * medir_latencia = 1 (modo latencia do benchmark: 1 produtor, 1 consumidor,
 * buffer de tamanho 1). No Windows o relogio (QueryPerformanceCounter) tem
 * resolucao de 100 ns. */
typedef struct {
    int64_t  id;         /* numero do item (itens normais tem id >= 0) */
    uint64_t t_prod_ns;  /* quando entrou no buffer (se medir_latencia) */
    uint64_t t_cons_ns;  /* quando saiu do buffer (se medir_latencia) */
} item_t;

typedef enum {
    SYNC_A_COND = 0,
    SYNC_B_SEM = 1
} sync_kind_t;

typedef struct buffer buffer_t;

struct buffer {
    sync_kind_t kind;
    int         capacity;
    int         medir_latencia;
    item_t     *slots;
    int         idx_in;      /* posicao onde entra o proximo item */
    int         idx_out;     /* posicao de onde sai o proximo item */
    int         count;       /* quantos itens tem agora (so a versao A usa) */
    pt_mutex_t  mutex;

    /* coisas da versao A */
    pt_cond_t   nao_cheio;
    pt_cond_t   nao_vazio;
    int         fechado;     /* vira 1 quando a producao acabou */

    /* coisas da versao B */
    pt_sem_t    vagas;       /* quantas posicoes livres tem */
    pt_sem_t    itens;       /* quantos itens tem para tirar */

    /* ponteiros para as funcoes da versao escolhida */
    int  (*op_put)(buffer_t *buffer, const item_t *item);
    int  (*op_get)(buffer_t *buffer, item_t *item_saida);
    int  (*op_close)(buffer_t *buffer, int num_consumidores);
    void (*op_destroy)(buffer_t *buffer);
};

/* Funcoes que o resto do programa usa */

/* Cria o buffer. Se der algum erro devolve NULL. */
buffer_t *buffer_create(sync_kind_t kind, int capacity, int medir_latencia);

/* Coloca um item no buffer. Se estiver cheio, espera ter vaga.
 * Devolve 0 se deu certo. */
int buffer_put(buffer_t *buffer, const item_t *item);

/* Tira um item do buffer. Se estiver vazio, espera chegar um.
 * Devolve 1 se pegou um item, 0 se acabou (nao vem mais nada) e -1 se deu erro.
 * Depois de receber 0 o consumidor tem que parar de chamar buffer_get. */
int buffer_get(buffer_t *buffer, item_t *item_saida);

/* Tem que ser chamada UMA vez so, depois que todos os produtores ja
 * terminaram (depois do join deles). num_consumidores e quantos consumidores
 * existem e vao continuar chamando buffer_get ate receber 0 (a versao B
 * precisa disso). Cuidado: na versao B, se num_consumidores for maior que o
 * numero de consumidores que ainda estao rodando, o put da sentinela que
 * sobra fica travado quando o buffer enche. */
int buffer_close(buffer_t *buffer, int num_consumidores);

void buffer_destroy(buffer_t *buffer);

/* Usadas so dentro do buffer: cada versao liga as suas funcoes no buffer */
int buffer_cond_init(buffer_t *buffer);   /* esta no buffer_cond.c */
int buffer_sem_init(buffer_t *buffer);    /* esta no buffer_sem.c */

#endif

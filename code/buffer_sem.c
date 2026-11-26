/* VERSAO B: dois semaforos contadores e um mutex.
 *   vagas: comeca com a capacidade (no inicio esta tudo livre)
 *   itens: comeca com 0 (no inicio nao tem nada para tirar)
 *   mutex: protege SO os indices (e a copia do item na posicao que o indice
 *          aponta). Os sem_wait e os sem_post ficam todos fora do mutex.*/
 
#include "buffer.h"

static int inserir_versao_b(buffer_t *buffer, const item_t *item)
{
    int posicao;
    int resultado_post;

    /* reserva uma vaga (se nao tiver, fica esperando aqui) */
    if (pt_sem_wait(&buffer->vagas) != 0) {
        return 1;
    }

    /* parte protegida pelo mutex: so o indice e a posicao do vetor */
    pt_mutex_lock(&buffer->mutex);
    posicao = buffer->idx_in;
    buffer->slots[posicao] = *item;
    if (buffer->medir_latencia && item->id != ITEM_SENTINELA) {
        buffer->slots[posicao].t_prod_ns = pt_time_ns();
    }
    buffer->idx_in = (buffer->idx_in + 1) % buffer->capacity;
    pt_mutex_unlock(&buffer->mutex);

    /* avisa que tem mais um item (fora do mutex) */
    resultado_post = pt_sem_post(&buffer->itens);
    return resultado_post;
}

static int retirar_versao_b(buffer_t *buffer, item_t *item_saida)
{
    uint64_t instante_retirada;
    item_t item_retirado;
    int posicao;

    instante_retirada = 0;

    /* espera ter um item para tirar */
    if (pt_sem_wait(&buffer->itens) != 0) {
        return -1;
    }

    /* parte protegida pelo mutex: so o indice e a posicao do vetor */
    pt_mutex_lock(&buffer->mutex);
    if (buffer->medir_latencia) {
        instante_retirada = pt_time_ns();
    }
    posicao = buffer->idx_out;
    item_retirado = buffer->slots[posicao];
    buffer->idx_out = (buffer->idx_out + 1) % buffer->capacity;
    pt_mutex_unlock(&buffer->mutex);

    /* libera a vaga (fora do mutex) */
    if (pt_sem_post(&buffer->vagas) != 0) {
        return -1;
    }

    /* se pegou a sentinela, acabou para esse consumidor */
    if (item_retirado.id == ITEM_SENTINELA) {
        return 0;
    }

    *item_saida = item_retirado;
    item_saida->t_cons_ns = instante_retirada;
    return 1;
}

static int fechar_versao_b(buffer_t *buffer, int num_consumidores)
{
    item_t sentinela;
    int i;

    sentinela.id = ITEM_SENTINELA;
    sentinela.t_prod_ns = 0;
    sentinela.t_cons_ns = 0;

    /* uma sentinela por consumidor: cada um pega exatamente uma e para */
    for (i = 0; i < num_consumidores; i++) {
        if (inserir_versao_b(buffer, &sentinela) != 0) {
            return 1;
        }
    }
    return 0;
}

static void destruir_versao_b(buffer_t *buffer)
{
    pt_sem_destroy(&buffer->vagas);
    pt_sem_destroy(&buffer->itens);
}

int buffer_sem_init(buffer_t *buffer)
{
    unsigned int capacidade;

    capacidade = (unsigned int)buffer->capacity;

    /* vagas + itens nunca passa da capacidade, entao o maximo que o
     * CreateSemaphore pede no Windows nunca e ultrapassado */
    if (pt_sem_init(&buffer->vagas, capacidade, capacidade) != 0) {
        return 1;
    }
    if (pt_sem_init(&buffer->itens, 0, capacidade) != 0) {
        pt_sem_destroy(&buffer->vagas);
        return 1;
    }

    buffer->op_put = inserir_versao_b;
    buffer->op_get = retirar_versao_b;
    buffer->op_close = fechar_versao_b;
    buffer->op_destroy = destruir_versao_b;
    return 0;
}

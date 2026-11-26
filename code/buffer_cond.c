/* VERSAO A: um mutex e duas variaveis de condicao (nao_cheio e nao_vazio).*/


#include "buffer.h"

static int inserir_versao_a(buffer_t *buffer, const item_t *item)
{
    int posicao;

    pt_mutex_lock(&buffer->mutex);

    /* buffer cheio: espera alguem tirar um item */
    while (buffer->count == buffer->capacity) {
        pt_cond_wait(&buffer->nao_cheio, &buffer->mutex);
    }

    posicao = buffer->idx_in;
    buffer->slots[posicao] = *item;
    if (buffer->medir_latencia) {
        buffer->slots[posicao].t_prod_ns = pt_time_ns();
    }
    buffer->idx_in = (buffer->idx_in + 1) % buffer->capacity;
    buffer->count = buffer->count + 1;

    /* agora tem pelo menos um item, entao avisa um consumidor */
    pt_cond_signal(&buffer->nao_vazio);
    pt_mutex_unlock(&buffer->mutex);
    return 0;
}

static int retirar_versao_a(buffer_t *buffer, item_t *item_saida)
{
    uint64_t instante_retirada;
    int posicao;

    instante_retirada = 0;

    pt_mutex_lock(&buffer->mutex);

    /* espera enquanto estiver vazio, a nao ser que a producao ja tenha acabado */
    while (buffer->count == 0 && !buffer->fechado) {
        pt_cond_wait(&buffer->nao_vazio, &buffer->mutex);
    }

    /* se saiu do while com o buffer vazio, e porque fechou: acabou */
    if (buffer->count == 0) {
        pt_mutex_unlock(&buffer->mutex);
        return 0;
    }

    if (buffer->medir_latencia) {
        instante_retirada = pt_time_ns();
    }

    posicao = buffer->idx_out;
    *item_saida = buffer->slots[posicao];
    item_saida->t_cons_ns = instante_retirada;
    buffer->idx_out = (buffer->idx_out + 1) % buffer->capacity;
    buffer->count = buffer->count - 1;

    /* agora tem pelo menos uma vaga, entao avisa um produtor */
    pt_cond_signal(&buffer->nao_cheio);
    pt_mutex_unlock(&buffer->mutex);
    return 1;
}

static int fechar_versao_a(buffer_t *buffer, int num_consumidores)
{
    /* a versao A nao precisa saber quantos consumidores tem */
    (void)num_consumidores;

    pt_mutex_lock(&buffer->mutex);
    buffer->fechado = 1;
    /* acorda todo mundo: os consumidores esvaziam o que sobrou e,
     * quando acharem o buffer vazio e fechado, retornam 0 */
    pt_cond_broadcast(&buffer->nao_vazio);
    pt_cond_broadcast(&buffer->nao_cheio);
    pt_mutex_unlock(&buffer->mutex);
    return 0;
}

static void destruir_versao_a(buffer_t *buffer)
{
    pt_cond_destroy(&buffer->nao_cheio);
    pt_cond_destroy(&buffer->nao_vazio);
}

int buffer_cond_init(buffer_t *buffer)
{
    if (pt_cond_init(&buffer->nao_cheio) != 0) {
        return 1;
    }
    if (pt_cond_init(&buffer->nao_vazio) != 0) {
        pt_cond_destroy(&buffer->nao_cheio);
        return 1;
    }

    buffer->fechado = 0;
    buffer->op_put = inserir_versao_a;
    buffer->op_get = retirar_versao_a;
    buffer->op_close = fechar_versao_a;
    buffer->op_destroy = destruir_versao_a;
    return 0;
}

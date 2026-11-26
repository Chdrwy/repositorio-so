/* Parte do buffer que e igual nas duas versoes: criar, destruir e repassar
 * cada chamada para a funcao da versao escolhida (A ou B).*/
 
#include <stdio.h>
#include <stdlib.h>
#include "buffer.h"

buffer_t *buffer_create(sync_kind_t kind, int capacity, int medir_latencia)
{
    buffer_t *buffer;
    int deu_erro;

    if (capacity <= 0) {
        return NULL;
    }

    /* calloc ja zera tudo, entao idx_in, idx_out, count e fechado comecam em 0 */
    buffer = (buffer_t *)calloc(1, sizeof(buffer_t));
    if (buffer == NULL) {
        return NULL;
    }

    buffer->slots = (item_t *)calloc((size_t)capacity, sizeof(item_t));
    if (buffer->slots == NULL) {
        free(buffer);
        return NULL;
    }

    buffer->kind = kind;
    buffer->capacity = capacity;
    buffer->medir_latencia = medir_latencia;

    if (pt_mutex_init(&buffer->mutex) != 0) {
        free(buffer->slots);
        free(buffer);
        return NULL;
    }

    /* inicializa a parte que e especifica de cada versao */
    deu_erro = 0;
    if (kind == SYNC_A_COND) {
        deu_erro = buffer_cond_init(buffer);
    } else if (kind == SYNC_B_SEM) {
        deu_erro = buffer_sem_init(buffer);
    } else {
        deu_erro = 1;
    }

    if (deu_erro != 0) {
        pt_mutex_destroy(&buffer->mutex);
        free(buffer->slots);
        free(buffer);
        return NULL;
    }

    return buffer;
}

int buffer_put(buffer_t *buffer, const item_t *item)
{
    int resultado;

    resultado = buffer->op_put(buffer, item);
    return resultado;
}

int buffer_get(buffer_t *buffer, item_t *item_saida)
{
    int resultado;

    resultado = buffer->op_get(buffer, item_saida);
    return resultado;
}

int buffer_close(buffer_t *buffer, int num_consumidores)
{
    int resultado;

    resultado = buffer->op_close(buffer, num_consumidores);
    return resultado;
}

void buffer_destroy(buffer_t *buffer)
{
    if (buffer == NULL) {
        return;
    }
    if (buffer->op_destroy != NULL) {
        buffer->op_destroy(buffer);
    }
    pt_mutex_destroy(&buffer->mutex);
    free(buffer->slots);
    free(buffer);
}

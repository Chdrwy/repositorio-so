/* benchmark.c
 * Roda UMA repeticao de UMA configuracao e escreve o resultado no final de
 * um arquivo CSV. O script experimentos/rodar_benchmark.sh chama esse
 * programa uma vez para cada configuracao e cada repeticao.
 *
 * Tem dois modos:
 *   vazao    : mede o tempo total e a vazao (o relogio por item fica DESLIGADO)
 *   latencia : so aceita 1 produtor, 1 consumidor e buffer 1; o relogio por
 *              item fica LIGADO e a gente guarda 1 amostra a cada --amostra itens.
 *
 * O tempo medido (tempo_s) vai de antes de criar as threads ate o join do
 * ultimo consumidor, entao inclui criar e juntar as P+C threads e o
 * buffer_close. A vazao e total_consumido / tempo_s.
 * A definicao de latencia esta no buffer.h. A amostra inclui o item 0 (o
 * primeiro, que pega a partida das threads); quem tira ele das estatisticas
 * e o analiser.py.
 *
 * Exemplo de uso:
 *   benchmark --modo vazao --sync A --prod 4 --cons 4 --buffer 16 \
 *             --items 200000 --rep 1 --out dados/vazao_linux.csv
 */
#ifndef _WIN32
  #define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "platform_thread.h"
#include "buffer.h"

/* nome da plataforma que vai na primeira coluna do CSV */
#ifdef _WIN32
  #define PLATAFORMA "windows"
#elif defined(__linux__)
  #define PLATAFORMA "linux"
#else
  #define PLATAFORMA "freebsd"
#endif

static const char CABECALHO_VAZAO[] =
    "plataforma,so_versao,sync,produtores,consumidores,buffer,"
    "total_itens,repeticao,tempo_s,vazao_itens_s,"
    "total_produzido,total_consumido,valido";

static const char CABECALHO_LATENCIA[] =
    "plataforma,sync,produtores,consumidores,buffer,"
    "repeticao,item_id,latencia_ns";

/* uma amostra de latencia: qual item e quanto tempo ele ficou no buffer */
typedef struct {
    int64_t id;
    int64_t latencia_ns;
} amostra_t;

/* o que cada thread produtora recebe e devolve */
typedef struct {
    buffer_t *buffer;
    int64_t primeiro_id;
    int64_t quantidade;
    int64_t produzidos;
    int erro;
    char espaco_cache[64];   /* sobra de espaco para as structs vizinhas nao
                                cairem na mesma linha de cache (o vetor dessas
                                structs e compartilhado entre as threads) */
} args_produtor_t;

/* o que cada thread consumidora recebe e devolve */
typedef struct {
    buffer_t *buffer;
    int64_t consumidos;
    uint64_t soma_ids;
    int erro;
    int64_t erros_latencia;
    amostra_t *amostras;
    int64_t num_amostras;
    int64_t max_amostras;
    int64_t passo_amostragem;
    char espaco_cache[64];   /* mesmo motivo da struct do produtor */
} args_consumidor_t;

/* resultado de uma execucao */
typedef struct {
    double  tempo_s;
    int64_t total_produzido;
    int64_t total_consumido;
    int64_t erros_latencia;
    int     valido;
} resultado_t;

static void *thread_produtora(void *parametro)
{
    args_produtor_t *args;
    item_t item;
    int64_t k;
    int64_t produzidos;

    args = (args_produtor_t *)parametro;
    item.id = 0;
    item.t_prod_ns = 0;
    item.t_cons_ns = 0;
    produzidos = 0;

    for (k = 0; k < args->quantidade; k++) {
        item.id = args->primeiro_id + k;
        if (buffer_put(args->buffer, &item) != 0) {
            args->erro = 1;
            break;
        }
        produzidos++;
    }

    args->produzidos = produzidos;
    return NULL;
}

static void *thread_consumidora(void *parametro)
{
    args_consumidor_t *args;
    item_t item;
    int resultado_get;
    int64_t consumidos;
    int64_t erros_latencia;
    int64_t num_amostras;
    uint64_t soma;

    args = (args_consumidor_t *)parametro;
    consumidos = 0;
    erros_latencia = 0;
    num_amostras = 0;
    soma = 0;

    resultado_get = buffer_get(args->buffer, &item);
    while (resultado_get == 1) {
        consumidos++;
        soma = soma + (uint64_t)item.id;

        /* no modo latencia, guarda 1 item a cada passo_amostragem */
        if (args->amostras != NULL && (item.id % args->passo_amostragem) == 0) {
            if (item.t_cons_ns >= item.t_prod_ns) {
                if (num_amostras < args->max_amostras) {
                    args->amostras[num_amostras].id = item.id;
                    args->amostras[num_amostras].latencia_ns = (int64_t)(item.t_cons_ns - item.t_prod_ns);
                    num_amostras++;
                }
            } else {
                /* relogio andou para tras: conta como erro */
                erros_latencia++;
            }
        }

        resultado_get = buffer_get(args->buffer, &item);
    }

    if (resultado_get < 0) {
        args->erro = 1;
    }
    args->consumidos = consumidos;
    args->soma_ids = soma;
    args->erros_latencia = erros_latencia;
    args->num_amostras = num_amostras;
    return NULL;
}

/* libera tudo o que a funcao executa() alocou (qualquer ponteiro pode ser NULL) */
static void libera_execucao(buffer_t *buffer, pt_thread_t *threads_prod, pt_thread_t *threads_cons,
                            args_produtor_t *args_prod, args_consumidor_t *args_cons,
                            int num_consumidores)
{
    int c;

    if (args_cons != NULL) {
        for (c = 0; c < num_consumidores; c++) {
            free(args_cons[c].amostras);
        }
    }
    if (buffer != NULL) {
        buffer_destroy(buffer);
    }
    free(threads_prod);
    free(threads_cons);
    free(args_prod);
    free(args_cons);
}

/* Roda uma repeticao. Devolve 0 se conseguiu rodar (mesmo que a validacao
 * tenha falhado: isso fica em resultado->valido) e 1 se nem conseguiu rodar.
 * No modo latencia, as amostras voltam em *amostras_saida e quem chamou
 * fica responsavel por dar free nelas. */
static int executa(sync_kind_t kind, int num_produtores, int num_consumidores, int capacidade,
                   int64_t total_itens, int medir_latencia, int64_t passo_amostragem,
                   resultado_t *resultado, amostra_t **amostras_saida, int64_t *num_amostras_saida)
{
    buffer_t *buffer;
    pt_thread_t *threads_prod;
    pt_thread_t *threads_cons;
    args_produtor_t *args_prod;
    args_consumidor_t *args_cons;
    int64_t itens_por_produtor;
    uint64_t tempo_inicio;
    uint64_t tempo_fim;
    uint64_t soma_ids;
    uint64_t soma_esperada;
    int teve_erro;
    int p;
    int c;

    memset(resultado, 0, sizeof(resultado_t));
    *amostras_saida = NULL;
    *num_amostras_saida = 0;

    buffer = buffer_create(kind, capacidade, medir_latencia);
    threads_prod = (pt_thread_t *)calloc((size_t)num_produtores, sizeof(pt_thread_t));
    threads_cons = (pt_thread_t *)calloc((size_t)num_consumidores, sizeof(pt_thread_t));
    args_prod = (args_produtor_t *)calloc((size_t)num_produtores, sizeof(args_produtor_t));
    args_cons = (args_consumidor_t *)calloc((size_t)num_consumidores, sizeof(args_consumidor_t));

    if (buffer == NULL || threads_prod == NULL || threads_cons == NULL ||
        args_prod == NULL || args_cons == NULL) {
        libera_execucao(buffer, threads_prod, threads_cons, args_prod, args_cons, num_consumidores);
        return 1;
    }

    itens_por_produtor = total_itens / num_produtores;

    for (c = 0; c < num_consumidores; c++) {
        args_cons[c].buffer = buffer;
        if (medir_latencia) {
            args_cons[c].passo_amostragem = passo_amostragem;
            args_cons[c].max_amostras = total_itens / passo_amostragem + 1;
            args_cons[c].amostras = (amostra_t *)calloc((size_t)args_cons[c].max_amostras, sizeof(amostra_t));
            if (args_cons[c].amostras == NULL) {
                libera_execucao(buffer, threads_prod, threads_cons, args_prod, args_cons, num_consumidores);
                return 1;
            }
        }
    }

    /* cada produtor gera uma faixa de ids diferente: 0..n-1, n..2n-1, ... */
    for (p = 0; p < num_produtores; p++) {
        args_prod[p].buffer = buffer;
        args_prod[p].primeiro_id = (int64_t)p * itens_por_produtor;
        args_prod[p].quantidade = itens_por_produtor;
    }

    tempo_inicio = pt_time_ns();

    for (c = 0; c < num_consumidores; c++) {
        if (pt_thread_create(&threads_cons[c], thread_consumidora, &args_cons[c]) != 0) {
            fprintf(stderr, "erro ao criar thread consumidora\n");
            exit(3);
        }
    }
    for (p = 0; p < num_produtores; p++) {
        if (pt_thread_create(&threads_prod[p], thread_produtora, &args_prod[p]) != 0) {
            fprintf(stderr, "erro ao criar thread produtora\n");
            exit(3);
        }
    }

    /* primeiro espera todos os produtores, depois fecha o buffer e
     * espera os consumidores terminarem */
    for (p = 0; p < num_produtores; p++) {
        pt_thread_join(&threads_prod[p]);
    }
    buffer_close(buffer, num_consumidores);
    for (c = 0; c < num_consumidores; c++) {
        pt_thread_join(&threads_cons[c]);
    }

    tempo_fim = pt_time_ns();
    resultado->tempo_s = (double)(tempo_fim - tempo_inicio) / 1e9;

    /* junta os resultados de todas as threads */
    teve_erro = 0;
    soma_ids = 0;
    for (p = 0; p < num_produtores; p++) {
        resultado->total_produzido = resultado->total_produzido + args_prod[p].produzidos;
        if (args_prod[p].erro != 0) {
            teve_erro = 1;
        }
    }
    for (c = 0; c < num_consumidores; c++) {
        resultado->total_consumido = resultado->total_consumido + args_cons[c].consumidos;
        resultado->erros_latencia = resultado->erros_latencia + args_cons[c].erros_latencia;
        soma_ids = soma_ids + args_cons[c].soma_ids;
        if (args_cons[c].erro != 0) {
            teve_erro = 1;
        }
    }

    /* os ids vao de 0 ate total-1, entao a soma tem que dar total*(total-1)/2.
     * Isso pega perda ou duplicacao de item, alem da contagem. */
    soma_esperada = (uint64_t)total_itens * ((uint64_t)total_itens - 1) / 2;

    resultado->valido = 1;
    if (teve_erro) {
        resultado->valido = 0;
    }
    if (resultado->total_produzido != total_itens) {
        resultado->valido = 0;
    }
    if (resultado->total_consumido != total_itens) {
        resultado->valido = 0;
    }
    if (soma_ids != soma_esperada) {
        resultado->valido = 0;
    }
    if (resultado->erros_latencia != 0) {
        resultado->valido = 0;
    }

    if (medir_latencia) {
        /* no modo latencia so existe 1 consumidor. O vetor de amostras passa
         * a ser de quem chamou, entao tira ele daqui para nao ser liberado */
        *amostras_saida = args_cons[0].amostras;
        *num_amostras_saida = args_cons[0].num_amostras;
        args_cons[0].amostras = NULL;
    }

    libera_execucao(buffer, threads_prod, threads_cons, args_prod, args_cons, num_consumidores);
    return 0;
}

/* Abre o CSV para acrescentar no final. Se o arquivo ainda nao existia,
 * escreve o cabecalho primeiro. */
static FILE *abrir_csv(const char *caminho, const char *cabecalho)
{
    FILE *teste;
    FILE *arquivo;
    int ja_existia;

    ja_existia = 0;
    teste = fopen(caminho, "r");
    if (teste != NULL) {
        ja_existia = 1;
        fclose(teste);
    }

    arquivo = fopen(caminho, "a");
    if (arquivo == NULL) {
        return NULL;
    }
    if (!ja_existia) {
        fprintf(arquivo, "%s\n", cabecalho);
    }
    return arquivo;
}

static void mostra_uso(void)
{
    fprintf(stderr,
      "uso: benchmark --modo vazao|latencia --sync A|B --prod P --cons C\n"
      "                --buffer N --items T --rep R --out arquivo.csv\n"
      "                [--so-versao texto] [--amostra K]\n");
}

int main(int argc, char **argv)
{
    const char *modo;
    const char *arquivo_saida;
    const char *so_versao;
    const char *opcao;
    const char *valor;
    char versao_sync;
    int num_produtores;
    int num_consumidores;
    int capacidade;
    int repeticao;
    int64_t total_itens;
    int64_t passo_amostragem;
    int medir_latencia;
    char so_limpo[128];
    size_t tamanho;
    size_t j;
    sync_kind_t kind;
    resultado_t resultado;
    amostra_t *amostras;
    int64_t num_amostras;
    int64_t i_amostra;
    double vazao;
    const char *texto_valido;
    FILE *arquivo;
    int i;

    modo = NULL;
    arquivo_saida = NULL;
    so_versao = "desconhecido";
    versao_sync = 0;
    num_produtores = 0;
    num_consumidores = 0;
    capacidade = 0;
    repeticao = 0;
    total_itens = 0;
    passo_amostragem = 100;
    amostras = NULL;
    num_amostras = 0;

    /* le os argumentos, sempre no formato --opcao valor */
    i = 1;
    while (i < argc) {
        opcao = argv[i];
        if (i + 1 < argc) {
            valor = argv[i + 1];
        } else {
            valor = NULL;
        }
        if (valor == NULL) {
            mostra_uso();
            return 1;
        }

        if (strcmp(opcao, "--modo") == 0) {
            modo = valor;
        } else if (strcmp(opcao, "--sync") == 0) {
            versao_sync = valor[0];
        } else if (strcmp(opcao, "--prod") == 0) {
            num_produtores = atoi(valor);
        } else if (strcmp(opcao, "--cons") == 0) {
            num_consumidores = atoi(valor);
        } else if (strcmp(opcao, "--buffer") == 0) {
            capacidade = atoi(valor);
        } else if (strcmp(opcao, "--items") == 0) {
            total_itens = atoll(valor);
        } else if (strcmp(opcao, "--rep") == 0) {
            repeticao = atoi(valor);
        } else if (strcmp(opcao, "--out") == 0) {
            arquivo_saida = valor;
        } else if (strcmp(opcao, "--so-versao") == 0) {
            so_versao = valor;
        } else if (strcmp(opcao, "--amostra") == 0) {
            passo_amostragem = atoll(valor);
        } else {
            mostra_uso();
            return 1;
        }

        i = i + 2;
    }

    /* confere se veio tudo que precisa */
    if (modo == NULL || arquivo_saida == NULL) {
        mostra_uso();
        return 1;
    }
    if (num_produtores <= 0 || num_consumidores <= 0 || capacidade <= 0 || repeticao <= 0) {
        mostra_uso();
        return 1;
    }
    if (total_itens <= 0 || passo_amostragem <= 0) {
        mostra_uso();
        return 1;
    }
    if (versao_sync != 'A' && versao_sync != 'B') {
        mostra_uso();
        return 1;
    }

    medir_latencia = 0;
    if (strcmp(modo, "latencia") == 0) {
        medir_latencia = 1;
    } else if (strcmp(modo, "vazao") != 0) {
        mostra_uso();
        return 1;
    }

    if (total_itens % num_produtores != 0) {
        fprintf(stderr, "erro: --items deve ser divisivel por --prod\n");
        return 1;
    }
    if (medir_latencia) {
        if (num_produtores != 1 || num_consumidores != 1 || capacidade != 1) {
            fprintf(stderr, "erro: modo latencia exige prod=1, cons=1, buffer=1\n");
            return 1;
        }
    }

    /* copia a versao do SO e troca virgula e quebra de linha por espaco,
     * senao estraga as colunas do CSV */
    snprintf(so_limpo, sizeof(so_limpo), "%s", so_versao);
    tamanho = strlen(so_limpo);
    for (j = 0; j < tamanho; j++) {
        if (so_limpo[j] == ',' || so_limpo[j] == '\n' || so_limpo[j] == '\r') {
            so_limpo[j] = ' ';
        }
    }

    if (versao_sync == 'A') {
        kind = SYNC_A_COND;
    } else {
        kind = SYNC_B_SEM;
    }

    if (executa(kind, num_produtores, num_consumidores, capacidade, total_itens,
                medir_latencia, passo_amostragem, &resultado, &amostras, &num_amostras) != 0) {
        fprintf(stderr, "erro: nao foi possivel executar a configuracao\n");
        return 3;
    }

    if (resultado.tempo_s > 0) {
        vazao = (double)resultado.total_consumido / resultado.tempo_s;
    } else {
        vazao = 0.0;
    }

    if (resultado.valido) {
        texto_valido = "valido";
    } else {
        texto_valido = "INVALIDO";
    }
    printf("%s %c %dx%d buf=%d rep=%d tempo=%.4fs vazao=%.0f itens/s %s\n",
           modo, versao_sync, num_produtores, num_consumidores, capacidade, repeticao,
           resultado.tempo_s, vazao, texto_valido);

    if (!medir_latencia) {
        /* modo vazao: uma linha por execucao, mesmo se for invalida
         * (fica marcada com valido = 0) */
        arquivo = abrir_csv(arquivo_saida, CABECALHO_VAZAO);
        if (arquivo == NULL) {
            fprintf(stderr, "erro ao abrir %s\n", arquivo_saida);
            return 4;
        }
        fprintf(arquivo, "%s,%s,%c,%d,%d,%d,%lld,%d,%.6f,%.2f,%lld,%lld,%d\n",
                PLATAFORMA, so_limpo, versao_sync, num_produtores, num_consumidores, capacidade,
                (long long)total_itens, repeticao, resultado.tempo_s, vazao,
                (long long)resultado.total_produzido, (long long)resultado.total_consumido,
                resultado.valido);
        fclose(arquivo);
    } else if (resultado.valido) {
        /* modo latencia: uma linha por amostra, so se a execucao foi valida */
        arquivo = abrir_csv(arquivo_saida, CABECALHO_LATENCIA);
        if (arquivo == NULL) {
            fprintf(stderr, "erro ao abrir %s\n", arquivo_saida);
            return 4;
        }
        for (i_amostra = 0; i_amostra < num_amostras; i_amostra++) {
            fprintf(arquivo, "%s,%c,%d,%d,%d,%d,%lld,%lld\n",
                    PLATAFORMA, versao_sync, num_produtores, num_consumidores, capacidade, repeticao,
                    (long long)amostras[i_amostra].id, (long long)amostras[i_amostra].latencia_ns);
        }
        fclose(arquivo);
    }

    free(amostras);

    if (resultado.valido) {
        return 0;
    } else {
        return 2;
    }
}

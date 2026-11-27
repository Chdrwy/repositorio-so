#!/usr/bin/env python3
"""
Le os CSVs brutos (sem mexer neles) e gera, na pasta de saida:
  - estatisticas_vazao.csv    media e desvio padrao AMOSTRAL (n-1) de cada configuracao
  - estatisticas_latencia.csv resumo da distribuicao da latencia (media, desvio,
                              percentis, zeros) de cada versao
  - razao_A_B.csv             razao A/B da vazao calculada DENTRO de cada repeticao
                              (A e B da mesma repeticao rodam uma logo depois da
                              outra, entao a razao cancela variacoes lentas da maquina)
  - deriva_vazao.csv          para ver se a vazao foi mudando ao longo das repeticoes
                              (correlacao de Spearman entre vazao e repeticao, e a
                              razao media(5 primeiras) / media(5 ultimas))
  - vazao_<so>.png            grafico 1: vazao x tamanho do buffer
  - latencia_<so>.png         grafico 2: distribuicao da latencia

Escolhas que a gente fez:
  - Grafico 1: um arquivo por SO, com DOIS paineis (1x1 e 4x4) e uma curva por
    versao (A e B). Os resultados de 1x1 e 4x4 nunca sao misturados.
  - O eixo vertical do grafico 1 e em escala log e tem os MESMOS limites no
    Linux e no Windows. Os limites saem dos proprios dados, entao nenhum ponto
    fica fora do grafico.
  - Execucoes com valido != 1 geram aviso e ficam FORA das estatisticas.
  - Latencia = t_cons - t_prod (a definicao esta em code/buffer.h). O item 0 de
    cada repeticao (o primeiro item, que pega a partida das threads) fica FORA
    por padrao; com --incluir-item0 ele entra. Latencias iguais a zero (podem
    aparecer no Windows, que tem resolucao de 100 ns) entram nas estatisticas,
    mas nao no grafico (escala log nao aceita zero); a quantidade aparece no titulo.

Como usar (na raiz do repositorio):
  python3 experimentos/analisar.py
  python3 experimentos/analisar.py --dados dados_teste --reps 1   (teste rapido)
"""
import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")   # salva os graficos em arquivo, sem abrir janela
import matplotlib.pyplot as plt

RAIZ = Path(__file__).resolve().parent.parent
PLATAFORMAS = ["linux", "windows"]
NOME_SO = {"linux": "Linux", "windows": "Windows"}
BUFFERS = [1, 16, 256]
PCS = [1, 4]
SYNCS = ["A", "B"]
NOME_SYNC = {"A": "Versão A (variável de condição)", "B": "Versão B (semáforos)"}
COR = {"A": "tab:blue", "B": "tab:orange"}

avisos = []


def aviso(mensagem):
    avisos.append(mensagem)
    print("AVISO:", mensagem)


def carregar(pasta_dados, plataforma, tipo):
    caminho = pasta_dados / f"{tipo}_{plataforma}.csv"
    if not caminho.exists():
        aviso(f"arquivo ausente: {caminho}")
        return None
    tabela = pd.read_csv(caminho)
    return tabela


# ------------------------------------------------------------------ conferencias
def validar_vazao(df, plataforma, reps):
    linhas_invalidas = df[df["valido"] != 1]
    if len(linhas_invalidas) > 0:
        aviso(f"{plataforma}: {len(linhas_invalidas)} execucao(oes) de vazao com valido != 1 "
              f"(excluidas das estatisticas):\n{linhas_invalidas.to_string(index=False)}")

    produzido_certo = df["total_produzido"] == df["total_itens"]
    consumido_certo = df["total_consumido"] == df["total_itens"]
    if not (produzido_certo & consumido_certo).all():
        aviso(f"{plataforma}: total produzido/consumido diferente do total de itens")

    if df["total_itens"].nunique() != 1:
        aviso(f"{plataforma}: total de itens varia entre execucoes")

    total = int(df["total_itens"].iloc[0])
    if total < 100000 or total > 1000000:
        aviso(f"{plataforma}: total de itens {total} fora da faixa 100.000-1.000.000")

    if not (df["produtores"] == df["consumidores"]).all():
        aviso(f"{plataforma}: ha linhas com produtores != consumidores")

    # daqui para frente so usa as execucoes validas
    validas = df[df["valido"] == 1]

    contagem = validas.groupby(["sync", "produtores", "buffer"])["repeticao"].nunique()
    numero_de_configuracoes = len(SYNCS) * len(PCS) * len(BUFFERS)
    faltou_algo = False
    if len(contagem) != numero_de_configuracoes:
        faltou_algo = True
    for quantidade in contagem:
        if quantidade < reps:
            faltou_algo = True
    if faltou_algo:
        aviso(f"{plataforma}: esperado 12 configuracoes x {reps} repeticoes validas; obtido:\n{contagem}")

    duplicadas = df.duplicated(["sync", "produtores", "buffer", "repeticao"]).sum()
    if duplicadas:
        aviso(f"{plataforma}: {duplicadas} linhas duplicadas de (config, repeticao)")

    return validas


def validar_latencia(df, plataforma, reps, amostras):
    produtores_errado = df["produtores"] != 1
    consumidores_errado = df["consumidores"] != 1
    buffer_errado = df["buffer"] != 1
    fora = df[produtores_errado | consumidores_errado | buffer_errado]
    if len(fora) > 0:
        aviso(f"{plataforma}: {len(fora)} linhas de latencia fora de 1x1 / buffer 1 (excluidas)")
        df = df.drop(fora.index)

    repeticoes_por_versao = df.groupby("sync")["repeticao"].nunique()
    amostras_por_versao = df.groupby("sync")["latencia_ns"].size()
    for versao in SYNCS:
        if versao not in repeticoes_por_versao.index:
            aviso(f"{plataforma}: sem latencia para a versao {versao}")
            continue
        repeticoes = repeticoes_por_versao[versao]
        quantidade = amostras_por_versao[versao]
        if repeticoes != reps or quantidade != reps * amostras:
            aviso(f"{plataforma} versao {versao}: reps={repeticoes} amostras={quantidade} "
                  f"(esperado {reps} e {reps * amostras})")

    if (df["latencia_ns"] < 0).any():
        aviso(f"{plataforma}: latencia negativa encontrada")
    return df


# ------------------------------------------------------------------ estatisticas
def estat_vazao(df, plataforma):
    grupos = df.groupby(["sync", "produtores", "buffer"])

    # o std do pandas ja e o desvio padrao amostral (divide por n-1)
    quantidade = grupos["repeticao"].size()
    tempo_medio = grupos["tempo_s"].mean()
    tempo_desvio = grupos["tempo_s"].std()
    vazao_media = grupos["vazao_itens_s"].mean()
    vazao_desvio = grupos["vazao_itens_s"].std()

    tabela = pd.DataFrame({
        "n": quantidade,
        "tempo_medio_s": tempo_medio,
        "tempo_desvio_s": tempo_desvio,
        "vazao_media": vazao_media,
        "vazao_desvio": vazao_desvio,
    })
    tabela = tabela.reset_index()
    tabela["coef_variacao_pct"] = 100 * tabela["vazao_desvio"] / tabela["vazao_media"]
    tabela.insert(0, "plataforma", plataforma)
    return tabela


def razao_pareada(df, plataforma):
    """Calcula a razao A/B dentro de cada repeticao e depois resume."""
    tabela = df.pivot_table(index=["produtores", "buffer", "repeticao"],
                            columns="sync", values="vazao_itens_s")
    tabela = tabela.dropna()   # so as repeticoes que tem A e B
    tabela["razao"] = tabela["A"] / tabela["B"]

    grupos = tabela.groupby(["produtores", "buffer"])["razao"]
    resumo = pd.DataFrame({
        "n": grupos.size(),
        "razao_media": grupos.mean(),
        "razao_desvio": grupos.std(),
        "razao_min": grupos.min(),
        "razao_max": grupos.max(),
    })
    resumo = resumo.reset_index()
    resumo.insert(0, "plataforma", plataforma)
    return resumo


def deriva(df, plataforma):
    linhas = []
    for chave, grupo in df.groupby(["sync", "produtores", "buffer"]):
        versao = chave[0]
        produtores = chave[1]
        tamanho_buffer = chave[2]

        grupo = grupo.sort_values("repeticao")
        correlacao = grupo["repeticao"].corr(grupo["vazao_itens_s"], method="spearman")

        # compara a media das primeiras k repeticoes com a das ultimas k
        k = min(5, len(grupo) // 2)
        if k == 0:
            k = 1
        media_inicio = grupo["vazao_itens_s"].head(k).mean()
        media_fim = grupo["vazao_itens_s"].tail(k).mean()

        linha = {}
        linha["plataforma"] = plataforma
        linha["sync"] = versao
        linha["produtores"] = produtores
        linha["buffer"] = tamanho_buffer
        linha["spearman_rep_vazao"] = correlacao
        linha[f"media_primeiras_{k}_sobre_ultimas_{k}"] = media_inicio / media_fim
        linhas.append(linha)
    return pd.DataFrame(linhas)


def estat_latencia(df, plataforma, item0_excluido):
    linhas = []
    for versao in SYNCS:
        valores = df.loc[df["sync"] == versao, "latencia_ns"].to_numpy(dtype=float)
        if valores.size == 0:
            continue
        linha = {
            "plataforma": plataforma,
            "sync": versao,
            "n": valores.size,
            "item0_excluido": item0_excluido,
            "media_ns": valores.mean(),
            "desvio_ns": valores.std(ddof=1),
            "minimo_ns": valores.min(),
            "p25_ns": np.percentile(valores, 25),
            "mediana_ns": np.median(valores),
            "p75_ns": np.percentile(valores, 75),
            "p95_ns": np.percentile(valores, 95),
            "p99_ns": np.percentile(valores, 99),
            "maximo_ns": valores.max(),
            "n_zeros": int((valores == 0).sum()),
            "valores_distintos": int(np.unique(valores).size),
        }
        linhas.append(linha)
    return pd.DataFrame(linhas)


# ------------------------------------------------------------------ graficos
def limites_eixo_vazao(lista_estatisticas):
    """Limites do eixo Y iguais para todas as plataformas, cobrindo media +- desvio."""
    menor = None
    maior = None
    for tabela in lista_estatisticas:
        for indice in range(len(tabela)):
            media = tabela["vazao_media"].iloc[indice]
            desvio = tabela["vazao_desvio"].iloc[indice]
            if pd.isna(desvio):
                desvio = 0
            # na escala log nao da para usar valor <= 0; nesse caso usa a media
            ponto_baixo = media - desvio
            if not ponto_baixo > 0:
                ponto_baixo = media
            ponto_alto = media + desvio
            if menor is None or ponto_baixo < menor:
                menor = ponto_baixo
            if maior is None or ponto_alto > maior:
                maior = ponto_alto
    return menor / 1.5, maior * 1.5


def grafico_vazao(est, plataforma, texto_so, reps, limites_y, pasta_saida):
    fig, eixos = plt.subplots(1, 2, figsize=(11, 4.3), sharey=True)
    posicoes = list(range(len(BUFFERS)))
    rotulos = []
    for tamanho in BUFFERS:
        rotulos.append(str(tamanho))

    for indice in range(len(PCS)):
        ax = eixos[indice]
        pc = PCS[indice]
        for versao in SYNCS:
            filtro = (est["sync"] == versao) & (est["produtores"] == pc)
            parte = est[filtro]
            parte = parte.set_index("buffer")
            parte = parte.reindex(BUFFERS)
            ax.errorbar(posicoes, parte["vazao_media"], yerr=parte["vazao_desvio"], marker="o",
                        capsize=4, color=COR[versao], label=NOME_SYNC[versao])
        ax.set_xticks(posicoes)
        ax.set_xticklabels(rotulos)
        ax.set_yscale("log")
        ax.set_ylim(limites_y[0], limites_y[1])
        ax.set_xlabel("Tamanho do buffer (posições)")
        ax.set_title(f"{pc} produtor(es) × {pc} consumidor(es)")
        ax.grid(True, which="both", alpha=0.3)

    eixos[0].set_ylabel("Vazão (itens/s, escala log)")
    eixos[0].legend()
    fig.suptitle(f"Vazão × tamanho do buffer — {NOME_SO[plataforma]} ({texto_so}); "
                 f"média ± desvio padrão amostral, {reps} repetições", fontsize=10)
    fig.tight_layout()
    fig.savefig(pasta_saida / f"vazao_{plataforma}.png", dpi=200)
    plt.close(fig)


def grafico_latencia(df, plataforma, item0_excluido, pasta_saida):
    # separa as latencias de cada versao, em microssegundos
    dados = {}
    for versao in SYNCS:
        em_ns = df.loc[df["sync"] == versao, "latencia_ns"].to_numpy(dtype=float)
        dados[versao] = em_ns / 1000.0

    # so os valores maiores que zero (a escala log nao aceita zero)
    positivos = {}
    for versao in SYNCS:
        valores = dados[versao]
        positivos[versao] = valores[valores > 0]

    fig, (ax_box, ax_hist) = plt.subplots(1, 2, figsize=(11, 4.3))

    listas_box = []
    for versao in SYNCS:
        listas_box.append(positivos[versao])
    nomes_box = []
    for versao in SYNCS:
        nomes_box.append(NOME_SYNC[versao])

    ax_box.boxplot(listas_box, showfliers=True,
                   flierprops=dict(markersize=2, alpha=0.3))
    ax_box.set_xticks([1, 2])
    ax_box.set_xticklabels(nomes_box, fontsize=8)
    ax_box.set_yscale("log")
    ax_box.set_ylabel("Latência (µs, escala log)")
    ax_box.set_title("Box plot (amostras > 0)")
    ax_box.grid(True, which="both", axis="y", alpha=0.3)

    # histograma cortado entre os percentis 0,1 e 99,9 (so para o desenho ficar legivel)
    todos = np.concatenate(listas_box)
    limites = np.percentile(todos, [0.1, 99.9])
    limite_baixo = limites[0]
    limite_alto = limites[1]
    faixas = np.logspace(np.log10(limite_baixo), np.log10(limite_alto), 70)

    fora_do_recorte = {}
    for versao in SYNCS:
        valores = positivos[versao]
        abaixo = valores < limite_baixo
        acima = valores > limite_alto
        fora_do_recorte[versao] = int((abaixo | acima).sum())

    for versao in SYNCS:
        ax_hist.hist(positivos[versao], bins=faixas, alpha=0.55, color=COR[versao], label=NOME_SYNC[versao])
    ax_hist.set_xscale("log")
    ax_hist.set_xlabel("Latência (µs, escala log)")
    ax_hist.set_ylabel("Número de amostras")
    ax_hist.set_title(f"Histograma (recorte p0,1–p99,9; fora: A={fora_do_recorte['A']}, "
                      f"B={fora_do_recorte['B']})", fontsize=10)
    ax_hist.legend(fontsize=8)
    ax_hist.grid(True, which="both", alpha=0.3)

    zeros = {}
    for versao in SYNCS:
        zeros[versao] = int((dados[versao] == 0).sum())

    if item0_excluido:
        texto_item0 = "item 0 excluído"
    else:
        texto_item0 = "item 0 incluído"

    fig.suptitle(f"Distribuição da latência — {NOME_SO[plataforma]} (1 produtor, 1 consumidor, "
                 f"buffer 1; {len(dados['A'])} amostras A, {len(dados['B'])} B; {texto_item0}; "
                 f"zeros omitidos: A={zeros['A']}, B={zeros['B']})", fontsize=9)
    fig.tight_layout()
    fig.savefig(pasta_saida / f"latencia_{plataforma}.png", dpi=200)
    plt.close(fig)


def formatar_numero(valor):
    # numeros pequenos com 4 casas, numeros grandes sem casas decimais
    if abs(valor) < 100:
        return f"{valor:,.4f}"
    else:
        return f"{valor:,.0f}"


# ------------------------------------------------------------------ main
def main():
    parser = argparse.ArgumentParser(description="Estatisticas e graficos do Tema 2")
    parser.add_argument("--dados", default=str(RAIZ / "dados"), help="pasta com os CSVs brutos")
    parser.add_argument("--saida", default=str(RAIZ / "experimentos" / "resultados"),
                        help="pasta de saida (estatisticas e graficos)")
    parser.add_argument("--reps", type=int, default=30, help="repeticoes esperadas por configuracao")
    parser.add_argument("--amostras", type=int, default=2000,
                        help="amostras de latencia por repeticao (itens / passo de amostragem)")
    parser.add_argument("--incluir-item0", action="store_true",
                        help="manter o item 0 (transitorio de partida) nas estatisticas de latencia")
    argumentos = parser.parse_args()

    pasta_dados = Path(argumentos.dados)
    pasta_saida = Path(argumentos.saida)
    pasta_saida.mkdir(parents=True, exist_ok=True)
    pd.set_option("display.width", 200)
    pd.set_option("display.float_format", formatar_numero)

    if argumentos.incluir_item0:
        excluir_item0 = False
    else:
        excluir_item0 = True

    # 1) le, confere e calcula tudo antes de desenhar
    vazao_validas = {}
    vazao_estatisticas = {}
    vazao_texto_so = {}
    latencias = {}
    for plataforma in PLATAFORMAS:
        tabela_vazao = carregar(pasta_dados, plataforma, "vazao")
        if tabela_vazao is not None:
            validas = validar_vazao(tabela_vazao, plataforma, argumentos.reps)
            vazao_validas[plataforma] = validas
            vazao_estatisticas[plataforma] = estat_vazao(validas, plataforma)
            vazao_texto_so[plataforma] = tabela_vazao["so_versao"].iloc[0]

        tabela_latencia = carregar(pasta_dados, plataforma, "latencia")
        if tabela_latencia is not None:
            tabela_latencia = validar_latencia(tabela_latencia, plataforma,
                                               argumentos.reps, argumentos.amostras)
            if excluir_item0:
                quantos_item0 = int((tabela_latencia["item_id"] == 0).sum())
                tabela_latencia = tabela_latencia[tabela_latencia["item_id"] != 0]
                print(f"{plataforma}: {quantos_item0} amostras de item 0 excluidas da latencia (transitorio de partida)")
            latencias[plataforma] = tabela_latencia

    # 2) graficos e tabelas (o eixo da vazao e o mesmo para as duas plataformas)
    if len(vazao_estatisticas) > 0:
        lista = []
        for plataforma in vazao_estatisticas:
            lista.append(vazao_estatisticas[plataforma])
        limites_y = limites_eixo_vazao(lista)
    else:
        limites_y = None

    todas_vazao = []
    todas_razao = []
    todas_deriva = []
    todas_latencia = []
    for plataforma in PLATAFORMAS:
        print(f"\n================ {NOME_SO[plataforma].upper()} ================")

        if plataforma in vazao_estatisticas:
            validas = vazao_validas[plataforma]
            estatisticas = vazao_estatisticas[plataforma]
            grafico_vazao(estatisticas, plataforma, vazao_texto_so[plataforma],
                          argumentos.reps, limites_y, pasta_saida)
            razao = razao_pareada(validas, plataforma)
            tabela_deriva = deriva(validas, plataforma)
            todas_vazao.append(estatisticas)
            todas_razao.append(razao)
            todas_deriva.append(tabela_deriva)

            print("\nVazão (itens/s) — média e desvio padrão amostral:")
            colunas = ["sync", "produtores", "buffer", "n", "vazao_media",
                       "vazao_desvio", "coef_variacao_pct", "tempo_medio_s"]
            print(estatisticas[colunas].to_string(index=False))
            print("\nRazão A/B pareada por repetição (>1 significa A mais rápida):")
            print(razao.to_string(index=False))

            com_deriva = 0
            for valor in tabela_deriva["spearman_rep_vazao"]:
                if abs(valor) > 0.4:
                    com_deriva = com_deriva + 1
            print(f"\nDeriva: {com_deriva}/{len(tabela_deriva)} configuracoes com |Spearman(repeticao, vazao)| > 0,4")

        if plataforma in latencias:
            estatisticas_lat = estat_latencia(latencias[plataforma], plataforma, excluir_item0)
            todas_latencia.append(estatisticas_lat)
            grafico_latencia(latencias[plataforma], plataforma, excluir_item0, pasta_saida)
            print("\nLatência (ns):")
            print(estatisticas_lat.to_string(index=False))

    if len(todas_vazao) > 0:
        pd.concat(todas_vazao).to_csv(pasta_saida / "estatisticas_vazao.csv", index=False)
        pd.concat(todas_razao).to_csv(pasta_saida / "razao_A_B.csv", index=False)
        pd.concat(todas_deriva).to_csv(pasta_saida / "deriva_vazao.csv", index=False)
    if len(todas_latencia) > 0:
        pd.concat(todas_latencia).to_csv(pasta_saida / "estatisticas_latencia.csv", index=False)

    print("\n================ RESUMO ================")
    print("Arquivos gerados em:", pasta_saida)
    if len(avisos) == 0:
        print("Avisos:", "nenhum")
        return 0
    else:
        print("Avisos:", len(avisos))
        return 1


if __name__ == "__main__":
    sys.exit(main())

# Projeto de SO – Tema 2: Variável de Condição e Semáforo

Trabalho de GBC045 – Sistemas Operacionais (FACOM/UFU), 2026/2.
Projeto "Threads, Sincronização e Avaliação Multiplataforma".

## Integrantes

| Nome | Matrícula |
|---|---|
| Bianca Fugita Maekawa Giacarelli | 12511BCC040 |
| Mateus Mattioli Chedraoui | 12511BCC010 |
| Rafaella Tavares Bonella | 12511BCC038 |
| Raissa Rodrigues Dias | 12511BCC026 |

Somos um grupo de 4 integrantes, com autorização do professor.

## O que tem aqui

A gente implementou o problema do produtor-consumidor com um **buffer circular limitado**,
sincronizado de dois jeitos:

- **Versão A:** um mutex e duas variáveis de condição (`nao_cheio` e `nao_vazio`).
- **Versão B:** dois semáforos contadores (`vagas` e `itens`) e um mutex que protege só os índices.

Tudo roda em cima de uma camada de abstração de threads (`platform_thread.h/.c`) com a mesma
API (`pt_*`) no Linux e no Windows. No Linux ela usa pthreads e semáforos POSIX. No Windows ela
usa a API Win32 nativa (`CreateThread`, `CRITICAL_SECTION`, `CONDITION_VARIABLE`,
`CreateSemaphore`), sem winpthreads.

Depois a gente mediu vazão e latência das duas versões nos dois sistemas, rodando de forma
nativa (sem WSL e sem Cygwin).

O vídeo de apresentação está na pasta `video/`. O relatório técnico (PDF de 4 páginas) foi
entregue no Moodle.

## Estrutura do repositório

```
README.md
video/                         vídeo de apresentação
code/
  platform_thread.h / .c       camada de abstração (Linux e Windows no mesmo arquivo, via #ifdef _WIN32)
  buffer.h / buffer.c          buffer circular (parte comum das duas versões)
  buffer_cond.c                versão A: mutex + 2 variáveis de condição
  buffer_sem.c                 versão B: 2 semáforos + mutex
  benchmark.c                  roda uma repetição de uma configuração e grava no CSV
testes/
  teste_plataforma.c           testa cada operação da camada
  teste_buffer.c               testa o buffer (versão A ou B)
dados/
  vazao_linux.csv              dados brutos de vazão (1 linha por execução)
  vazao_windows.csv
  latencia_linux.csv           dados brutos de latência (1 linha por amostra)
  latencia_windows.csv
  ambiente_linux.txt           máquina, sistema, compilador e controles usados
  ambiente_windows.txt
  preliminar/                  medição antiga, de antes das correções (não usada no relatório)
experimentos/
  rodar_benchmark.sh           compila e roda o experimento inteiro
  analisar.py                  lê os CSVs e gera estatísticas e gráficos
  resultados/                  estatísticas (CSV) e os 4 gráficos (PNG)
```

Os CSVs de `dados/` são os dados brutos exatamente como o benchmark gerou, sem nenhuma edição
manual. O `.gitattributes` impede que o git troque o fim de linha desses arquivos.

## Ambiente usado nos experimentos

| | Linux | Windows |
|---|---|---|
| Máquina | Lenovo LOQ | Notebook HP |
| Processador | Intel Core i5-13450HX (10 núcleos, 16 threads) | Intel Core i5-1035G1 (4 núcleos, 8 threads) |
| Sistema | Ubuntu, kernel 7.0.0-44-generic | Windows 11 (versão 10.0.26300) |
| Compilador | gcc 15.2.0 | gcc 16.2.0 (MinGW-w64, MSYS2 UCRT64) |
| Flags | `-std=gnu11 -Wall -Wextra -O2 -pthread` | `-std=gnu11 -Wall -Wextra -O2 -D_WIN32_WINNT=0x0601` |
| Energia | governor `performance`, na tomada | plano Alto desempenho, na tomada |

Os detalhes completos de cada máquina estão em `dados/ambiente_linux.txt` e
`dados/ambiente_windows.txt`, que o próprio script gera.

## Roteiro de reprodução

Todos os comandos são rodados na raiz do repositório.

### 1. O que precisa instalar

- **Linux:** `gcc`, `bash` e `python3`. No Ubuntu: `sudo apt install build-essential python3 python3-pip`.
- **Windows:** [MSYS2](https://www.msys2.org). Abra o terminal **MSYS2 UCRT64** (não o "MSYS", que é
  baseado em Cygwin) e rode `pacman -S mingw-w64-ucrt-x86_64-gcc`.
- **Análise (em qualquer um dos dois):** Python 3 com pandas, numpy e matplotlib:
  `pip install pandas numpy matplotlib` (se o pip reclamar, use um ambiente virtual com
  `python3 -m venv .venv` e `source .venv/bin/activate` antes).

### 2. Compilar e rodar os testes

Linux:
```
gcc -std=gnu11 -Wall -Wextra -O2 -pthread -Icode testes/teste_plataforma.c code/platform_thread.c -o testes/teste_plataforma
gcc -std=gnu11 -Wall -Wextra -O2 -pthread -Icode testes/teste_buffer.c code/buffer.c code/buffer_cond.c code/buffer_sem.c code/platform_thread.c -o testes/teste_buffer
./testes/teste_plataforma
./testes/teste_buffer A
./testes/teste_buffer B
```

Windows (terminal MSYS2 UCRT64, **sem** `-pthread`):
```
gcc -std=gnu11 -Wall -Wextra -O2 -D_WIN32_WINNT=0x0601 -Icode testes/teste_plataforma.c code/platform_thread.c -o testes/teste_plataforma.exe
gcc -std=gnu11 -Wall -Wextra -O2 -D_WIN32_WINNT=0x0601 -Icode testes/teste_buffer.c code/buffer.c code/buffer_cond.c code/buffer_sem.c code/platform_thread.c -o testes/teste_buffer.exe
./testes/teste_plataforma.exe
./testes/teste_buffer.exe A
./testes/teste_buffer.exe B
```

Cada programa tem que terminar com `TUDO CERTO (0 falha(s))`. Se algum teste travar, uma thread
vigia mostra `[FALHA] TRAVOU` e encerra o programa.

### 3. Rodar o experimento

O script `experimentos/rodar_benchmark.sh` compila o benchmark, grava o ambiente e roda as
12 configurações (1×1 e 4×4; buffer 1, 16 e 256; versões A e B) com 30 repetições cada, mais as
execuções de latência (1×1, buffer 1, versões A e B). Tudo é feito com 200.000 itens.

Linux (o script exige o governor `performance` em todas as CPUs):
```
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
bash experimentos/rodar_benchmark.sh
```

Windows (terminal MSYS2 UCRT64; deixar na tomada, no modo de melhor desempenho e sem suspensão):
```
bash experimentos/rodar_benchmark.sh
```

O script se recusa a rodar se já existirem `dados/vazao_<so>.csv` ou `dados/latencia_<so>.csv`,
para não apagar dados brutos sem querer. Para refazer, mova esses arquivos para outra pasta antes.

Variáveis opcionais (colocar antes do comando):

| Variável | Padrão | Para que serve |
|---|---|---|
| `REPS` | 30 | repetições por configuração |
| `ITEMS` | 200000 | total de itens por execução |
| `AMOSTRA` | 100 | latência: guarda 1 item a cada N |
| `DADOS` | `dados` | pasta onde os CSVs são gravados |
| `PAUSA_S` | 0 | pausa em segundos entre repetições (para a CPU esfriar) |
| `ALLOW_NONPERF` | 0 | 1 = ignora o governor no Linux (só para teste rápido) |

Teste rápido, para conferir se está tudo funcionando sem mexer em `dados/`:
```
REPS=1 ALLOW_NONPERF=1 DADOS=dados_teste bash experimentos/rodar_benchmark.sh
python3 experimentos/analisar.py --dados dados_teste --reps 1 --saida dados_teste/resultados
```

Também dá para rodar uma execução só, direto pelo benchmark (o executável é criado pelo script;
no Windows ele se chama `code/benchmark.exe`):
```
./code/benchmark --modo vazao --sync A --prod 4 --cons 4 --buffer 16 --items 200000 --rep 1 --out saida.csv
./code/benchmark --modo latencia --sync B --prod 1 --cons 1 --buffer 1 --items 200000 --rep 1 --amostra 100 --out lat.csv
```

### 4. Gerar estatísticas e gráficos

```
python3 experimentos/analisar.py
```

O script lê `dados/vazao_*.csv` e `dados/latencia_*.csv` (sem alterar nada) e grava em
`experimentos/resultados/`:

| Arquivo | Conteúdo |
|---|---|
| `estatisticas_vazao.csv` | média e desvio padrão amostral (n−1) da vazão de cada configuração |
| `estatisticas_latencia.csv` | média, desvio, mediana, percentis (25, 75, 95, 99), mínimo e máximo da latência |
| `razao_A_B.csv` | razão de vazão A/B calculada dentro de cada repetição |
| `deriva_vazao.csv` | correlação de Spearman entre repetição e vazão, para ver se a máquina mudou ao longo da coleta |
| `vazao_linux.png`, `vazao_windows.png` | gráfico 1: vazão × tamanho do buffer, uma curva por versão, painéis 1×1 e 4×4 |
| `latencia_linux.png`, `latencia_windows.png` | gráfico 2: distribuição da latência (box plot e histograma) |

Opções: `--dados`, `--saida`, `--reps`, `--amostras` e `--incluir-item0`. Por padrão, o primeiro
item de cada repetição fica fora da latência, porque ele pega a partida das threads. O script
mostra `AVISO` (e sai com código 1) se faltar algum arquivo, se tiver execução inválida ou
configuração com menos repetições que o esperado. No teste rápido é normal aparecer aviso de
arquivo ausente do outro sistema.

## Formato dos dados brutos

`vazao_<so>.csv`, uma linha por execução:
```
plataforma,so_versao,sync,produtores,consumidores,buffer,total_itens,repeticao,tempo_s,vazao_itens_s,total_produzido,total_consumido,valido
```
`valido` = 1 quando produzidos = consumidos = total de itens e a soma dos ids bate com
n(n−1)/2, ou seja, nenhum item se perdeu nem se repetiu.

`latencia_<so>.csv`, uma linha por amostra (1 a cada 100 itens):
```
plataforma,sync,produtores,consumidores,buffer,repeticao,item_id,latencia_ns
```
A latência é o tempo entre o item ser gravado no buffer e ser retirado dele, os dois instantes
medidos com o mutex travado.

## Resumo dos resultados

As 720 execuções de vazão (360 por sistema) saíram válidas, com 30 repetições em todas as configurações.

- A vazão sobe muito de buffer 1 para 16 (de 2,5 a 12 vezes) e bem menos de 16 para 256.
- **Linux:** A e B empatam com buffer 1. Com buffers 16 e 256, a versão A é cerca de 1,7 vez mais rápida em 1×1.
- **Windows:** em 1×1, a versão A é 9,3 vezes mais rápida com buffer 1 e 3,4 vezes com 16 e 256.
  A nossa explicação é que o semáforo do Windows é um objeto do kernel, então cada operação nele
  passa pelo kernel. Em 4×4 a versão B ganhou (A/B entre 0,71 e 0,85).
- **Latência mediana:** 1,5 µs nas duas versões no Linux. No Windows, 0,3 µs na versão A e 2,5 µs na B.

A análise completa, com as limitações, está no relatório.

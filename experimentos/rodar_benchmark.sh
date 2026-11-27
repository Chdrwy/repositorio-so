#!/usr/bin/env bash
# # Compila o benchmark e roda TODAS as configuracoes, REPS repeticoes cada.
# Em cada repeticao roda as 12 configuracoes de vazao e mais 2 de latencia
# (versao A e versao B).
#
# O que o script faz para diminuir o ruido nas medidas:
#   - o laco das repeticoes e o de FORA, entao as configuracoes vao se
#     alternando ao longo do tempo (nenhuma pega so o comeco ou so o fim);
#   - a ordem das versoes troca a cada repeticao (impar: A depois B, par: B
#     depois A), para que se a maquina for ficando mais lenta (esquentando,
#     por exemplo) isso nao prejudique sempre a mesma versao;
#   - antes de medir roda uma vez para "aquecer" e joga esse resultado fora;
#   - da para colocar uma pausa entre as repeticoes (PAUSA_S) para a CPU esfriar;
#   - no Linux exige o governor 'performance' em TODAS as CPUs.
# Tudo isso fica anotado em dados/ambiente_<plataforma>.txt.
#
# Teste rapido:  REPS=1 DADOS=dados_teste bash experimentos/rodar_benchmark.sh
#                python3 experimentos/analiser.py --dados dados_teste --reps 1 --saida dados_teste/resultados
# Para valer:    bash experimentos/rodar_benchmark.sh
# Exemplo para notebook que esquenta:  PAUSA_S=20 bash experimentos/rodar_benchmark.sh
set -u

# vai para a raiz do repositorio (a pasta acima de experimentos/)
cd "$(dirname "$0")/.." || exit 1

# valores padrao (da para trocar passando a variavel antes do comando)
REPS=${REPS:-30}
ITEMS=${ITEMS:-200000}
AMOSTRA=${AMOSTRA:-100}
DADOS=${DADOS:-dados}
PAUSA_S=${PAUSA_S:-0}

SRC="code/benchmark.c code/buffer.c code/buffer_cond.c code/buffer_sem.c code/platform_thread.c"

# descobre em qual sistema esta rodando
SISTEMA=$(uname -s)
case "$SISTEMA" in
  Linux*)
    PLAT=linux
    EXE=code/benchmark
    PLATFLAGS="-pthread"
    SOVER="Linux-$(uname -r)"

    # le o governor de todas as CPUs; tem que dar so "performance"
    GOV=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ' ' | sed 's/ $//')
    if [ -z "$GOV" ]; then
      GOV=desconhecido
    fi
    if [ "$GOV" != "performance" ] && [ "${ALLOW_NONPERF:-0}" != "1" ]; then
      echo "ERRO: governor das CPUs esta em '$GOV'. Rode:"
      echo "  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
      echo "(ou use ALLOW_NONPERF=1 para ignorar, so em testes rapidos)"
      exit 1
    fi
    ;;
  MINGW*)
    PLAT=windows
    EXE=code/benchmark.exe
    PLATFLAGS="-D_WIN32_WINNT=0x0601"

    # pega so o numero da versao do Windows (ex.: 10.0.26300.9974)
    SOVER=$(cmd.exe //c ver 2>/dev/null | tr -d '\r\n' | grep -a -o '[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9.]*')
    if [ -z "$SOVER" ]; then
      SOVER="Windows"
    else
      SOVER="Windows-$SOVER"
    fi
    GOV="n/a"
    ;;
  *)
    echo "ERRO: terminal nao suportado: $(uname -s)."
    echo "No Windows use o terminal MSYS2 UCRT64 (nao o 'MSYS')."
    exit 1
    ;;
esac

mkdir -p "$DADOS"
VAZAO="$DADOS/vazao_$PLAT.csv"
LAT="$DADOS/latencia_$PLAT.csv"
AMB="$DADOS/ambiente_$PLAT.txt"

# nao deixa apagar dados brutos sem querer
if [ -e "$VAZAO" ] || [ -e "$LAT" ]; then
  echo "ERRO: ja existem dados em $DADOS ($VAZAO / $LAT)."
  echo "Para nao sobrescrever dados brutos, mova ou renomeie esses arquivos antes."
  exit 1
fi

echo "== Compilando =="
CFLAGS="-std=gnu11 -Wall -Wextra -O2"
# $CFLAGS, $PLATFLAGS e $SRC ficam sem aspas de proposito, para virar varios argumentos
if ! gcc $CFLAGS $PLATFLAGS -Icode $SRC -o "$EXE"; then
  echo "ERRO de compilacao"
  exit 1
fi

echo "== Registrando ambiente em $AMB =="
{
  echo "data: $(date)"
  echo "plataforma: $PLAT"
  echo "so: $SOVER"
  echo "compilador: $(gcc --version | head -n1)"
  echo "gcc -dumpmachine: $(gcc -dumpmachine)"
  echo "flags: $CFLAGS $PLATFLAGS"
  echo "itens: $ITEMS  repeticoes: $REPS  amostra_latencia: 1 a cada $AMOSTRA"
  echo "controles: aquecimento descartado; ordem A/B alternada por repeticao; pausa entre repeticoes: ${PAUSA_S}s"
  echo "governor: $GOV"
  echo "uname -a: $(uname -a)"
  if [ "$PLAT" = linux ]; then
    LC_ALL=C lscpu | grep -E "Model name|^CPU\(s\):|Core\(s\) per socket|Thread\(s\) per core"
    HV=$(LC_ALL=C lscpu | grep -i 'Hypervisor vendor' | sed 's/.*: *//')
    if [ -z "$HV" ]; then
      HV=nenhum
    fi
    echo "hipervisor: $HV"
    echo "bibliotecas do executavel:"
    ldd "$EXE" | sed 's/^/  /'
  else
    powershell.exe -NoProfile -Command "Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors | Format-List" 2>/dev/null | tr -d '\r'
    echo "plano de energia: $(powercfg.exe //getactivescheme 2>/dev/null | tr -d '\r')"
    echo "energia: $(powershell.exe -NoProfile -Command "\$b = Get-CimInstance Win32_Battery; if (\$b) { 'BatteryStatus=' + \$b.BatteryStatus + ' (2 = ligado na tomada)' } else { 'sem bateria' }" 2>/dev/null | tr -d '\r')"
    echo "hipervisor presente: $(powershell.exe -NoProfile -Command "(Get-CimInstance Win32_ComputerSystem).HypervisorPresent" 2>/dev/null | tr -d '\r')"
    echo "DLLs importadas pelo executavel (nao deve haver libwinpthread, cygwin1 ou msys-2.0):"
    objdump -p "$EXE" 2>/dev/null | grep "DLL Name" | sed 's/^[[:space:]]*/  /'
  fi
} > "$AMB"

# roda uma vez cada versao so para aquecer e apaga o resultado
echo "== Aquecimento (descartado) =="
AQ="$DADOS/.aquecimento_descartado.csv"
"$EXE" --modo vazao --sync A --prod 4 --cons 4 --buffer 16 --items "$ITEMS" --rep 1 --out "$AQ" > /dev/null
"$EXE" --modo vazao --sync B --prod 4 --cons 4 --buffer 16 --items "$ITEMS" --rep 1 --out "$AQ" > /dev/null
rm -f "$AQ"

FALHAS=0
INICIO=$(date +%s)
echo "== Executando: $REPS repeticoes =="
for rep in $(seq 1 "$REPS"); do
  echo "--- repeticao $rep de $REPS ---"

  # repeticao impar: A e depois B; repeticao par: B e depois A
  if [ $((rep % 2)) -eq 1 ]; then
    ORDEM="A B"
  else
    ORDEM="B A"
  fi

  # 12 configuracoes de vazao: 2 versoes x (1x1 e 4x4) x (buffer 1, 16 e 256)
  for sync in $ORDEM; do
    for pc in 1 4; do                     # 1 = 1x1, 4 = 4x4
      for buf in 1 16 256; do
        if ! "$EXE" --modo vazao --sync $sync --prod $pc --cons $pc --buffer $buf \
               --items "$ITEMS" --rep "$rep" --so-versao "$SOVER" --out "$VAZAO"; then
          echo "!! FALHA: vazao $sync ${pc}x${pc} buf=$buf rep=$rep"
          FALHAS=$((FALHAS+1))
        fi
      done
    done
  done

  # latencia: 1 produtor, 1 consumidor, buffer 1, nas duas versoes
  for sync in $ORDEM; do
    if ! "$EXE" --modo latencia --sync $sync --prod 1 --cons 1 --buffer 1 \
           --items "$ITEMS" --rep "$rep" --amostra "$AMOSTRA" --out "$LAT"; then
      echo "!! FALHA: latencia $sync rep=$rep"
      FALHAS=$((FALHAS+1))
    fi
  done

  # pausa entre repeticoes (nao precisa depois da ultima)
  if [ "$PAUSA_S" != "0" ] && [ "$rep" -lt "$REPS" ]; then
    sleep "$PAUSA_S"
  fi
done

FIM=$(date +%s)
echo
echo "== Terminado em $((FIM-INICIO)) s. Execucoes com falha: $FALHAS =="

# confere quantas linhas sairam (tira 1 por causa do cabecalho)
LINHAS_VAZAO=$(( $(wc -l < "$VAZAO") - 1 ))
echo "Linhas de vazao (esperado $((REPS*12))): $LINHAS_VAZAO"
LINHAS_LAT=$(( $(wc -l < "$LAT") - 1 ))
echo "Linhas de latencia (esperado $((REPS*2*ITEMS/AMOSTRA))): $LINHAS_LAT"

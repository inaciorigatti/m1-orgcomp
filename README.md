# Decodificador de Instruções RISC-V (RV32I)

Prática M1 — Arquitetura de Computadores II

## Arquivos

| Arquivo | Descrição |
|---|---|
| `decodificador.cpp` | Código-fonte completo (único arquivo, comentado por seções) |
| `entrada_hex.txt` | Conjunto de teste em hexadecimal (um de cada formato + desvio negativo) |
| `entrada_bin.txt` | O mesmo conjunto de teste, em binário |
| `cpi.txt` | Tabela de CPI por formato de instrução (R, I, S, B, U, J) |

## Como compilar

Requer um compilador com suporte a C++17 (g++, clang++, etc.):

```bash
g++ -std=c++17 -Wall -Wextra -O2 -o decodificador decodificador.cpp
```

## Como executar

```bash
./decodificador <arquivo_instrucoes> [endereco_base_hex] [arquivo_cpi]
```

- `arquivo_instrucoes` (obrigatório): arquivo com uma instrução por linha,
  em hexadecimal ou binário — o formato é detectado automaticamente.
- `endereco_base_hex` (opcional, padrão `0x00000000`): endereço do PC
  atribuído à primeira instrução do arquivo.
- `arquivo_cpi` (opcional, padrão `cpi.txt`): tabela com o CPI de cada
  formato, uma linha por classe: `<FORMATO> <valor>` (ex.: `B 1.8`). Se o
  arquivo não for encontrado, valores padrão são usados e um aviso é
  impresso.

Exemplos:

```bash
./decodificador entrada_hex.txt
./decodificador entrada_bin.txt 0x00400000 cpi.txt
```

## O que o programa faz

1. **Leitura (R1):** lê o arquivo linha a linha, ignora linhas em branco e
   comentários (`#`, `//`, `;`), remove o prefixo `0x` quando presente e
   detecta automaticamente se o conteúdo é hexadecimal ou binário — sem
   depender de opção de linha de comando — observando se a primeira linha
   útil tem exatamente 32 caracteres compostos só por `0`/`1`. Cada
   instrução recebe seu endereço (PC), calculado a partir do endereço-base.

2. **Classificação e identificação (R2):** o opcode (bits 6:0) define o
   formato (R, I, S, B, U ou J); dentro de cada formato, `funct3` (e
   `funct7`, quando existir) definem o mnemônico exato — é assim que o
   código distingue `add` de `sub`, `srli` de `srai`, `lb` de `lw`, `beq`
   de `bltu`, etc. Qualquer combinação sem correspondência é reportada como
   inválida, com o endereço onde ocorreu, sem interromper o processamento
   do restante do arquivo.

3. **Extração dos campos (R3):** cada formato só reporta os campos que
   realmente possui (por exemplo, uma instrução tipo B nunca tem `rd`
   informado, mesmo que os bits 11-7 contenham algum valor — esses bits
   fazem parte do imediato). Os imediatos dos tipos B e J são remontados
   a partir dos bits fora de ordem em que são codificados, com extensão de
   sinal aplicada corretamente.

4. **Saída (R4):** cada instrução é impressa com endereço, palavra
   original, formato, mnemônico, campos e a linha equivalente em assembly,
   usando nomes ABI dos registradores (`s0`, `sp`, `a0`, ...). Desvios e
   saltos mostram o endereço absoluto de destino (PC + imediato). As
   pseudo-instruções mais comuns (`nop`, `li`, `mv`, `ret`, `jr`, `j`,
   `call`, `beqz`, `bnez`) são reconhecidas e indicadas entre parênteses.

5. **Relatório estatístico (R5):** ao final, o programa mostra a
   contagem e o percentual de instruções por formato, e calcula o CPI
   médio do programa como a média ponderada dos CPIs de cada classe pela
   sua frequência, usando a tabela de CPI fornecida.

## Estrutura do código

O arquivo `decodificador.cpp` está organizado em seções, cada uma com um
comentário-cabeçalho, para facilitar a leitura e a extensão do código na
próxima etapa do trabalho (detecção de hazards):

1. Constantes e tabelas (nomes ABI dos registradores)
2. Estrutura `Instrucao` (o que é guardado sobre cada instrução decodificada,
   usando `optional<...>` para os campos que só existem em alguns formatos)
3. Extração de bits e reconstrução de imediatos (`extraiBits`,
   `estendeSinal`, `imediatoTipoI/S/B/U/J`)
4. `decodificar()`: opcode + funct3 + funct7 → formato, mnemônico e campos
5. Reconhecimento de pseudo-instruções
6. Montagem da linha em assembly
7. Leitura do arquivo de entrada (detecção hexadecimal x binário)
8. Leitura da tabela de CPI
9. `main()`: junta tudo e imprime a listagem e o relatório estatístico

## Extensões previstas para a próxima etapa

Como a `Instrucao` já expõe claramente quais registradores são lidos
(`rs1`, `rs2`) e qual é escrito (`rd`) — todos como `optional`, portanto
nunca "inventados" para formatos que não os possuem — a detecção de
hazards de dados na próxima etapa pode ser construída diretamente sobre
essa estrutura, sem precisar redecodificar nada.

// =============================================================================
//  Decodificador de instruções RISC-V (RV32I)
//
//  Disciplina: Arquitetura de Computadores II
//  Prática M1 - Decodificador RISC-V
//
//  O programa lê um arquivo com uma instrução por linha (hexadecimal ou
//  binário, detectado automaticamente), decodifica cada palavra de 32 bits,
//  identifica o formato (R, I, S, B, U ou J) e o mnemônico, reconstrói os
//  campos e imediatos, monta a instrução em assembly (com nomes ABI) e, ao
//  final, imprime um relatório estatístico com a distribuição por formato e
//  o CPI médio do programa.
//
//  Organização do código (nessa ordem, de baixo para cima):
//    1) Constantes e tabelas (nomes ABI, tabelas de mnemônicos)
//    2) Estrutura Instrucao (o que guardamos sobre cada instrução decodificada)
//    3) Funções de extração de bits e de imediatos
//    4) Função decodificar(): opcode/funct3/funct7 -> formato + mnemônico + campos
//    5) Função de reconhecimento de pseudo-instruções
//    6) Função de montagem do texto em assembly
//    7) Leitura do arquivo de entrada (detecção hexadecimal x binário)
//    8) Leitura da tabela de CPI
//    9) main(): junta tudo, imprime a listagem e o relatório estatístico
// =============================================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <iomanip>
#include <cstdint>
#include <algorithm>
#include <optional>
#include <array>
#include <stdexcept>

using namespace std;

// -----------------------------------------------------------------------
// 1) Constantes e tabelas
// -----------------------------------------------------------------------

// Nomes ABI dos 32 registradores inteiros (x0..x31), na ordem correta.
static const array<string, 32> NOMES_ABI = {
    "zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

string nomeRegistrador(int indice) {
    if (indice < NOMES_ABI.size()) return NOMES_ABI[indice];
    return "x?"; // nunca deve acontecer (índice sempre tem 5 bits, 0..31)
}

// -----------------------------------------------------------------------
// 2) Estrutura que representa uma instrução já decodificada
// -----------------------------------------------------------------------
//
// Usamos "optional<...>" para os campos rd/rs1/rs2/funct3/funct7: assim,
// se o formato não possui aquele campo, ele simplesmente não é preenchido
// (não corremos o risco de "inventar" um rd para uma instrução tipo B,
// por exemplo, que é exatamente o erro que o enunciado pede para evitar).

struct Instrucao {
    int endereco = 0;      // PC da instrução
    int palavra = 0;       // palavra de 32 bits, como veio do arquivo
    bool valida = false;        // false = opcode/funct3/funct7 desconhecidos
    char formato = '?';         // 'R', 'I', 'S', 'B', 'U', 'J' ou '?' (inválida)
    string mnemonico;           // ex.: "add", "beq", "lw" ...

    optional<int> rd, rs1, rs2, funct3, funct7;
    optional<int64_t> imediato;   // já com extensão de sinal aplicada
    optional<int> alvoAbsoluto; // endereço de destino (branches/jal)
};

// -----------------------------------------------------------------------
// 3) Extração de bits e reconstrução de imediatos
// -----------------------------------------------------------------------

// Retorna os bits [hi:lo] (inclusive) de "palavra", alinhados à direita.
int extraiBits(int palavra, int hi, int lo) {
    int largura = hi - lo + 1;
    int mascara = (largura == 32) ? 0xFFFFFFFFu : ((1u << largura) - 1u);
    return (palavra >> lo) & mascara;
}

// Aplica extensão de sinal a um valor de "bits" bits, devolvendo um int64_t.
int64_t estendeSinal(int valor, int bits) {
    int mascaraSinal = 1u << (bits - 1);
    int resultado = (valor ^ mascaraSinal) - mascaraSinal;
    return static_cast<int32_t>(resultado);
}

int64_t imediatoTipoI(int p) {
    return estendeSinal(extraiBits(p, 31, 20), 12);
}

int64_t imediatoTipoS(int p) {
    int alto = extraiBits(p, 31, 25); // imm[11:5]
    int baixo = extraiBits(p, 11, 7); // imm[4:0]
    return estendeSinal((alto << 5) | baixo, 12);
}

int64_t imediatoTipoB(int p) {
    // Os bits do imediato do tipo B ficam "embaralhados" na palavra para
    // simplificar o hardware; aqui nós os recompomos na ordem correta.
    int bit12   = extraiBits(p, 31, 31);
    int bit11   = extraiBits(p, 7, 7);
    int bits10_5 = extraiBits(p, 30, 25);
    int bits4_1  = extraiBits(p, 11, 8);
    int imm = (bit12 << 12) | (bit11 << 11) | (bits10_5 << 5) | (bits4_1 << 1);
    // bit 0 do imediato de branch é sempre 0 (destinos são múltiplos de 2)
    return estendeSinal(imm, 13);
}

int64_t imediatoTipoU(int p) {
    // imm[31:12] ocupam os 20 bits mais altos da palavra; os 12 bits baixos
    // do imediato final são sempre zero (é isso que "upper immediate" quer dizer).
    return static_cast<int64_t>(extraiBits(p, 31, 12)) << 12;
}

int64_t imediatoTipoJ(int p) {
    int bit20     = extraiBits(p, 31, 31);
    int bits19_12 = extraiBits(p, 19, 12);
    int bit11     = extraiBits(p, 20, 20);
    int bits10_1  = extraiBits(p, 30, 21);
    int imm = (bit20 << 20) | (bits19_12 << 12) | (bit11 << 11) | (bits10_1 << 1);
    return estendeSinal(imm, 21);
}

// -----------------------------------------------------------------------
// 4) Decodificação: opcode + funct3 + funct7 -> formato, mnemônico e campos
// -----------------------------------------------------------------------

// Cria uma instrução marcada como inválida (opcode/funct3/funct7 sem
// correspondência conhecida). Usada em todos os pontos de saída antecipada
// da função decodificar() abaixo.
Instrucao instrucaoInvalida(int endereco, int palavra) {
    Instrucao inst;
    inst.endereco = endereco;
    inst.palavra = palavra;
    inst.valida = false;
    return inst;
}

Instrucao decodificar(int palavra, int endereco) {
    Instrucao inst;
    inst.endereco = endereco;
    inst.palavra = palavra;

    int opcode = extraiBits(palavra, 6, 0);
    int rd     = extraiBits(palavra, 11, 7);
    int f3     = extraiBits(palavra, 14, 12);
    int rs1    = extraiBits(palavra, 19, 15);
    int rs2    = extraiBits(palavra, 24, 20);
    int f7     = extraiBits(palavra, 31, 25);

    switch (opcode) {

    // ---------------- Tipo R (operações registrador-registrador) --------
    case 0b0110011: {
        inst.formato = 'R';
        inst.rd = rd; inst.rs1 = rs1; inst.rs2 = rs2;
        inst.funct3 = f3; inst.funct7 = f7;

        if      (f3 == 0b000 && f7 == 0b0000000) inst.mnemonico = "add";
        else if (f3 == 0b000 && f7 == 0b0100000) inst.mnemonico = "sub";
        else if (f3 == 0b001 && f7 == 0b0000000) inst.mnemonico = "sll";
        else if (f3 == 0b010 && f7 == 0b0000000) inst.mnemonico = "slt";
        else if (f3 == 0b011 && f7 == 0b0000000) inst.mnemonico = "sltu";
        else if (f3 == 0b100 && f7 == 0b0000000) inst.mnemonico = "xor";
        else if (f3 == 0b101 && f7 == 0b0000000) inst.mnemonico = "srl";
        else if (f3 == 0b101 && f7 == 0b0100000) inst.mnemonico = "sra";
        else if (f3 == 0b110 && f7 == 0b0000000) inst.mnemonico = "or";
        else if (f3 == 0b111 && f7 == 0b0000000) inst.mnemonico = "and";
        else return instrucaoInvalida(endereco, palavra); // combinação desconhecida

        inst.valida = true;
        break;
    }

    // ---------------- Tipo I: aritmética/lógica imediata -----------------
    case 0b0010011: {
        inst.formato = 'I';
        inst.rd = rd; inst.rs1 = rs1; inst.funct3 = f3;

        if (f3 == 0b001) { // slli: desloca à esquerda, shamt nos 5 bits baixos
            if (f7 != 0b0000000) return instrucaoInvalida(endereco, palavra);
            inst.mnemonico = "slli";
            inst.imediato = extraiBits(palavra, 24, 20); // shamt (0..31)
        } else if (f3 == 0b101) { // srli/srai, distinguidos pelo funct7
            int shamt = extraiBits(palavra, 24, 20);
            if (f7 == 0b0000000)      inst.mnemonico = "srli";
            else if (f7 == 0b0100000) inst.mnemonico = "srai";
            else return instrucaoInvalida(endereco, palavra);
            inst.imediato = shamt;
        } else {
            switch (f3) {
                case 0b000: inst.mnemonico = "addi";  break;
                case 0b010: inst.mnemonico = "slti";  break;
                case 0b011: inst.mnemonico = "sltiu"; break;
                case 0b100: inst.mnemonico = "xori";  break;
                case 0b110: inst.mnemonico = "ori";   break;
                case 0b111: inst.mnemonico = "andi";  break;
                default: return instrucaoInvalida(endereco, palavra);
            }
            inst.imediato = imediatoTipoI(palavra);
        }
        inst.valida = true;
        break;
    }

    // ---------------- Tipo I: carregamentos (load) -----------------------
    case 0b0000011: {
        inst.formato = 'I';
        inst.rd = rd; inst.rs1 = rs1; inst.funct3 = f3;
        inst.imediato = imediatoTipoI(palavra);

        switch (f3) {
            case 0b000: inst.mnemonico = "lb";  break;
            case 0b001: inst.mnemonico = "lh";  break;
            case 0b010: inst.mnemonico = "lw";  break;
            case 0b100: inst.mnemonico = "lbu"; break;
            case 0b101: inst.mnemonico = "lhu"; break;
            default: return instrucaoInvalida(endereco, palavra);
        }
        inst.valida = true;
        break;
    }

    // ---------------- Tipo I: jalr -----------------------------------
    case 0b1100111: {
        if (f3 != 0b000) return instrucaoInvalida(endereco, palavra);
        inst.formato = 'I';
        inst.rd = rd; inst.rs1 = rs1; inst.funct3 = f3;
        inst.imediato = imediatoTipoI(palavra);
        inst.mnemonico = "jalr";
        inst.valida = true;
        break;
    }

    // ---------------- Tipo I: ecall / ebreak (chamadas de sistema) ----
    case 0b1110011: {
        if (f3 != 0b000) return instrucaoInvalida(endereco, palavra);
        int64_t imm = imediatoTipoI(palavra);
        inst.formato = 'I';
        inst.rd = rd; inst.rs1 = rs1; inst.funct3 = f3;
        inst.imediato = imm;
        if (imm == 0)      inst.mnemonico = "ecall";
        else if (imm == 1) inst.mnemonico = "ebreak";
        else return instrucaoInvalida(endereco, palavra);
        inst.valida = true;
        break;
    }

    // ---------------- Tipo S: armazenamentos (store) ---------------------
    case 0b0100011: {
        inst.formato = 'S';
        inst.rs1 = rs1; inst.rs2 = rs2; inst.funct3 = f3;
        inst.imediato = imediatoTipoS(palavra);

        switch (f3) {
            case 0b000: inst.mnemonico = "sb"; break;
            case 0b001: inst.mnemonico = "sh"; break;
            case 0b010: inst.mnemonico = "sw"; break;
            default: return instrucaoInvalida(endereco, palavra);
        }
        inst.valida = true;
        break;
    }

    // ---------------- Tipo B: desvios condicionais (branch) --------------
    case 0b1100011: {
        inst.formato = 'B';
        inst.rs1 = rs1; inst.rs2 = rs2; inst.funct3 = f3;
        int64_t imm = imediatoTipoB(palavra);
        inst.imediato = imm;
        inst.alvoAbsoluto = static_cast<int>(static_cast<int64_t>(endereco) + imm);

        switch (f3) {
            case 0b000: inst.mnemonico = "beq";  break;
            case 0b001: inst.mnemonico = "bne";  break;
            case 0b100: inst.mnemonico = "blt";  break;
            case 0b101: inst.mnemonico = "bge";  break;
            case 0b110: inst.mnemonico = "bltu"; break;
            case 0b111: inst.mnemonico = "bgeu"; break;
            default: return instrucaoInvalida(endereco, palavra);
        }
        inst.valida = true;
        break;
    }

    // ---------------- Tipo U: lui / auipc ---------------------------------
    case 0b0110111: // lui
    case 0b0010111: { // auipc
        inst.formato = 'U';
        inst.rd = rd;
        inst.imediato = imediatoTipoU(palavra);
        inst.mnemonico = (opcode == 0b0110111) ? "lui" : "auipc";
        inst.valida = true;
        break;
    }

    // ---------------- Tipo J: jal -----------------------------------------
    case 0b1101111: {
        inst.formato = 'J';
        inst.rd = rd;
        int64_t imm = imediatoTipoJ(palavra);
        inst.imediato = imm;
        inst.alvoAbsoluto = static_cast<int>(static_cast<int64_t>(endereco) + imm);
        inst.mnemonico = "jal";
        inst.valida = true;
        break;
    }

    default:
        return instrucaoInvalida(endereco, palavra); // opcode desconhecido
    } // fim do switch(opcode)

    return inst;
}

// -----------------------------------------------------------------------
// 5) Reconhecimento de pseudo-instruções mais comuns
// -----------------------------------------------------------------------
//
// Não substituímos o mnemônico "real" (ele continua sendo reportado nos
// campos, pois é o que de fato está codificado), mas indicamos na saída em
// assembly, entre parênteses, a que pseudo-instrução aquela codificação
// corresponde, quando for o caso.

optional<string> reconhecePseudoInstrucao(const Instrucao& inst) {
    if (!inst.valida) return nullopt;

    if (inst.mnemonico == "addi") {
        int rd = *inst.rd, rs1 = *inst.rs1;
        int64_t imm = *inst.imediato;
        if (rd == 0 && rs1 == 0 && imm == 0) return string("nop");
        if (rs1 == 0)                        return "li " + nomeRegistrador(rd) + ", " + to_string(imm);
        if (imm == 0)                        return "mv " + nomeRegistrador(rd) + ", " + nomeRegistrador(rs1);
    }
    if (inst.mnemonico == "jalr") {
        if (*inst.rd == 0 && *inst.rs1 == 1 && *inst.imediato == 0) return string("ret");
        if (*inst.rd == 0) return "jr " + nomeRegistrador(*inst.rs1);
    }
    if (inst.mnemonico == "jal") {
        if (*inst.rd == 0) return string("j <alvo>");
        if (*inst.rd == 1) return string("call <alvo>");
    }
    if (inst.mnemonico == "beq" && *inst.rs2 == 0) {
        return "beqz " + nomeRegistrador(*inst.rs1) + ", <alvo>";
    }
    if (inst.mnemonico == "bne" && *inst.rs2 == 0) {
        return "bnez " + nomeRegistrador(*inst.rs1) + ", <alvo>";
    }
    return nullopt;
}

// -----------------------------------------------------------------------
// 6) Montagem da linha em assembly (usando nomes ABI)
// -----------------------------------------------------------------------

string enderecoHex(int v) {
    ostringstream oss;
    oss << "0x" << hex << uppercase << setfill('0') << setw(8) << v;
    return oss.str();
}

string montaAssembly(const Instrucao& inst) {
    if (!inst.valida) return "(instrucao invalida)";
    const string& m = inst.mnemonico;

    switch (inst.formato) {
    case 'R':
        return m + " " + nomeRegistrador(*inst.rd) + ", " + nomeRegistrador(*inst.rs1) +
               ", " + nomeRegistrador(*inst.rs2);

    case 'I':
        if (m == "jalr")
            return m + " " + nomeRegistrador(*inst.rd) + ", " + to_string(*inst.imediato) +
                   "(" + nomeRegistrador(*inst.rs1) + ")";
        if (m == "ecall" || m == "ebreak")
            return m;
        if (m == "lb" || m == "lh" || m == "lw" || m == "lbu" || m == "lhu")
            return m + " " + nomeRegistrador(*inst.rd) + ", " + to_string(*inst.imediato) +
                   "(" + nomeRegistrador(*inst.rs1) + ")";
        return m + " " + nomeRegistrador(*inst.rd) + ", " + nomeRegistrador(*inst.rs1) +
               ", " + to_string(*inst.imediato);

    case 'S':
        return m + " " + nomeRegistrador(*inst.rs2) + ", " + to_string(*inst.imediato) +
               "(" + nomeRegistrador(*inst.rs1) + ")";

    case 'B':
        return m + " " + nomeRegistrador(*inst.rs1) + ", " + nomeRegistrador(*inst.rs2) +
               ", " + enderecoHex(*inst.alvoAbsoluto);

    case 'U':
        // Por convenção, o imediato mostrado no assembly de lui/auipc é o
        // valor de 20 bits (sem os 12 zeros à direita que já constam no
        // campo "imediato" reconstruído acima).
        return m + " " + nomeRegistrador(*inst.rd) + ", " + to_string(*inst.imediato >> 12);

    case 'J':
        return m + " " + nomeRegistrador(*inst.rd) + ", " + enderecoHex(*inst.alvoAbsoluto);

    default:
        return "(formato desconhecido)";
    }
}

// -----------------------------------------------------------------------
// 7) Leitura do arquivo de instruções (hexadecimal ou binário)
// -----------------------------------------------------------------------

// Remove espaços nas pontas e o que vier depois de um marcador de comentário.
string limpaLinha(const string& linhaOriginal) {
    string linha = linhaOriginal;

    for (const string marcador : {"#", "//", ";"}) {
        size_t pos = linha.find(marcador);
        if (pos != string::npos) linha = linha.substr(0, pos);
    }
    size_t inicio = linha.find_first_not_of(" \t\r\n");
    if (inicio == string::npos) return "";
    size_t fim = linha.find_last_not_of(" \t\r\n");
    return linha.substr(inicio, fim - inicio + 1);
}

string removePrefixo0x(const string& s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
}

bool ehSequenciaBinariaDe32Bits(const string& s) {
    if (s.size() != 32) return false;
    return all_of(s.begin(), s.end(), [](char c) { return c == '0' || c == '1'; });
}

// Lê todas as linhas úteis (não vazias, sem comentários) do arquivo.
vector<string> leLinhasUteis(const string& caminho) {
    ifstream arquivo(caminho);
    if (!arquivo) throw runtime_error("Nao foi possivel abrir o arquivo: " + caminho);

    vector<string> linhas;
    string bruta;
    while (getline(arquivo, bruta)) {
        string limpa = limpaLinha(bruta);
        if (!limpa.empty()) linhas.push_back(limpa);
    }
    return linhas;
}

// Decide, a partir da primeira linha válida, se o arquivo está em binário
// (32 caracteres compostos só por '0'/'1') ou em hexadecimal.
bool arquivoEstaEmBinario(const vector<string>& linhas) {
    if (linhas.empty()) return false;
    return ehSequenciaBinariaDe32Bits(removePrefixo0x(linhas.front()));
}

int converteLinhaParaPalavra(const string& linha, bool binario) {
    string semPrefixo = removePrefixo0x(linha);
    unsigned long valor = stoul(semPrefixo, nullptr, binario ? 2 : 16);
    return static_cast<int>(valor);
}

// -----------------------------------------------------------------------
// 8) Leitura da tabela de CPI (uma classe por linha: "FORMATO valor")
// -----------------------------------------------------------------------

map<char, double> leTabelaCPI(const string& caminho) {
    // Valores padrão, usados caso o arquivo não seja informado/encontrado.
    map<char, double> cpi = {{'R', 1.0}, {'I', 1.0}, {'S', 1.0},
                              {'B', 2.0}, {'U', 1.0}, {'J', 2.0}};

    ifstream arquivo(caminho);
    if (!arquivo) {
        cout << "[aviso] Arquivo de CPI \"" << caminho
             << "\" nao encontrado; usando valores padrao.\n\n";
        return cpi;
    }

    string linha;
    while (getline(arquivo, linha)) {
        string limpa = limpaLinha(linha);
        if (limpa.empty()) continue;
        istringstream iss(limpa);
        string formato; double valor;
        if (iss >> formato >> valor && !formato.empty()) {
            cpi[toupper(formato[0])] = valor;
        }
    }
    return cpi;
}

// -----------------------------------------------------------------------
// 9) main(): lê o arquivo, decodifica tudo, imprime listagem e estatísticas
// -----------------------------------------------------------------------

void imprimeCampo(const string& nome, const optional<int>& v) {
    if (v) cout << " " << nome << "=" << *v;
}

void imprimeInstrucao(const Instrucao& inst) {
    cout << enderecoHex(inst.endereco) << "  " << enderecoHex(inst.palavra) << "  ";

    if (!inst.valida) {
        cout << "INVALIDA (nenhum opcode/funct3/funct7 conhecido corresponde a esta palavra)\n";
        return;
    }

    cout << "formato=" << inst.formato << "  mnemonico=" << inst.mnemonico << " |";
    imprimeCampo("rd", inst.rd);
    imprimeCampo("rs1", inst.rs1);
    imprimeCampo("rs2", inst.rs2);
    imprimeCampo("funct3", inst.funct3);
    if (inst.imediato) cout << " imm=" << *inst.imediato;
    if (inst.alvoAbsoluto) cout << " alvo=" << enderecoHex(*inst.alvoAbsoluto);
    cout << "\n";

    cout << "    asm: " << montaAssembly(inst);
    if (auto pseudo = reconhecePseudoInstrucao(inst)) {
        cout << "   (pseudo-instrucao: " << *pseudo << ")";
    }
    cout << "\n";
}

void imprimeRelatorioEstatistico(const vector<Instrucao>& instrucoes, const map<char, double>& cpi) {
    map<char, int> contagem = {{'R', 0}, {'I', 0}, {'S', 0}, {'B', 0}, {'U', 0}, {'J', 0}};
    int invalidas = 0;

    for (const auto& inst : instrucoes) {
        if (!inst.valida) { invalidas++; continue; }
        contagem[inst.formato]++;
    }

    int totalValidas = static_cast<int>(instrucoes.size()) - invalidas;

    cout << "\n===================== RELATORIO ESTATISTICO =====================\n";
    cout << "Total de instrucoes lidas: " << instrucoes.size();
    if (invalidas > 0) cout << "  (" << invalidas << " invalida(s))";
    cout << "\n\n";

    cout << fixed << setprecision(2);
    double cpiPonderadoSoma = 0.0; // soma de (contagem * cpi) para a media ponderada

    for (char f : {'R', 'I', 'S', 'B', 'U', 'J'}) {
        int qtd = contagem[f];
        double percentual = (totalValidas > 0) ? (100.0 * qtd / totalValidas) : 0.0;
        cout << "  Tipo " << f << ": " << setw(4) << qtd << " instrucao(oes)  ("
             << setw(6) << percentual << "%)   CPI da classe = " << cpi.at(f) << "\n";
        cpiPonderadoSoma += qtd * cpi.at(f);
    }

    cout << "\n";
    if (totalValidas > 0) {
        double cpiMedio = cpiPonderadoSoma / totalValidas;
        cout << "CPI medio do programa (media ponderada pela frequencia de cada formato): "
             << cpiMedio << "\n";
    } else {
        cout << "CPI medio: nao calculado (nenhuma instrucao valida encontrada).\n";
    }
    cout << "===================================================================\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Uso: " << argv[0] << " <arquivo_instrucoes> [endereco_base_hex] [arquivo_cpi]\n";
        cerr << "Exemplo: " << argv[0] << " entrada.txt 0x00000000 cpi.txt\n";
        return 1;
    }

    string caminhoEntrada = argv[1];
    int enderecoBase = (argc >= 3) ? static_cast<int>(stoul(argv[2], nullptr, 16)) : 0;
    string caminhoCPI = (argc >= 4) ? argv[3] : "cpi.txt";

    try {
        vector<string> linhas = leLinhasUteis(caminhoEntrada);
        if (linhas.empty()) {
            cerr << "O arquivo de entrada nao contem nenhuma instrucao valida.\n";
            return 1;
        }

        bool binario = arquivoEstaEmBinario(linhas);
        cout << "Formato de entrada detectado: " << (binario ? "BINARIO" : "HEXADECIMAL") << "\n";
        cout << "Endereco base: " << enderecoHex(enderecoBase) << "\n\n";

        vector<Instrucao> instrucoes;
        int pc = enderecoBase;
        for (const string& linha : linhas) {
            int palavra = converteLinhaParaPalavra(linha, binario);
            Instrucao inst = decodificar(palavra, pc);
            imprimeInstrucao(inst);
            instrucoes.push_back(inst);
            pc += 4; // cada instrucao RISC-V (base) ocupa 4 bytes
        }

        map<char, double> tabelaCPI = leTabelaCPI(caminhoCPI);
        imprimeRelatorioEstatistico(instrucoes, tabelaCPI);

    } catch (const exception& e) {
        cerr << "Erro: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include <iomanip>
#include <math.h>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <openssl/sha.h>
#elif _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <capstone/capstone.h>
#include "json.h"
#include "decompiler.h"

// Very WIP Multi-Threaded Recursive Decompiler

using namespace binlex;
using json = nlohmann::json;
#ifndef _WIN32
static pthread_mutex_t DECOMPILER_MUTEX = PTHREAD_MUTEX_INITIALIZER;
#else
CRITICAL_SECTION csDecompiler;
#endif

//from https://github.com/capstone-engine/capstone/blob/master/include/capstone/x86.h

#define X86_REL_ADDR(insn) (((insn).detail->x86.operands[0].type == X86_OP_IMM) \
	? (uint64_t)((insn).detail->x86.operands[0].imm) \
	: (((insn).address + (insn).size) + (uint64_t)(insn).detail->x86.disp))

Decompiler::Decompiler() {
    for (int i = 0; i < DECOMPILER_MAX_SECTIONS; i++) {
        sections[i].offset = 0;
        sections[i].data = NULL;
        sections[i].data_size = 0;
    }
}

void Decompiler::AppendTrait(struct Trait *trait, struct Section *sections, uint index){
    struct Trait new_elem_trait = *trait; //copy the stuff populated in the caller, TODO: more cleanup required to not copy anything.

    #if defined(__linux__) || defined(__APPLE__)
    pthread_mutex_lock(&DECOMPILER_MUTEX);
    #else
    EnterCriticalSection(&csDecompiler);
    #endif

    new_elem_trait.type = (char *)malloc(strlen(trait->type) + 1);
    if (new_elem_trait.type == NULL){
        PRINT_ERROR_AND_EXIT("[x] trait malloc failed\n");
    }
    memset(new_elem_trait.type, 0, strlen(trait->type) + 1);
    if (memcpy(new_elem_trait.type, trait->type, strlen(trait->type)) == NULL){
        PRINT_ERROR_AND_EXIT("[x] trait memcpy failed\n");
    }

    new_elem_trait.trait = (char *)malloc(strlen(trait->tmp_trait.c_str()) + 1);
    if (new_elem_trait.trait == NULL){
        PRINT_ERROR_AND_EXIT("[x] trait malloc failed\n");
    }
    memset(new_elem_trait.trait, 0, strlen(trait->tmp_trait.c_str()) + 1);
    if (memcpy(new_elem_trait.trait, trait->tmp_trait.c_str(), strlen(trait->tmp_trait.c_str())) == NULL){
        PRINT_ERROR_AND_EXIT("[x] trait memcpy failed\n");
    }
    new_elem_trait.bytes = (char *)malloc(strlen(trait->tmp_bytes.c_str()) + 1);
    if (new_elem_trait.bytes == NULL){
        PRINT_ERROR_AND_EXIT("[x] trait malloc failed\n");
    }
    memset(new_elem_trait.bytes, 0, strlen(trait->tmp_bytes.c_str()) + 1);
    if (memcpy(new_elem_trait.bytes, trait->tmp_bytes.c_str(), strlen(trait->tmp_bytes.c_str())) == NULL){
        PRINT_ERROR_AND_EXIT("[x] trait memcpy failed\n");
    }
    sections[index].traits.push_back(new_elem_trait);

    #if defined(__linux__) || defined(__APPLE__)
    pthread_mutex_unlock(&DECOMPILER_MUTEX);
    #else
    LeaveCriticalSection(&csDecompiler);
    #endif
}

bool Decompiler::Setup(cs_arch architecture, cs_mode mode_type) {
    arch = architecture;
    mode = mode_type;
    return true;
}

json Decompiler::GetTrait(struct Trait &trait){
    json data;
    data["type"] = trait.type;
    data["corpus"] = g_args.options.corpus;
    data["tags"] = g_args.options.tags;
    data["mode"] = g_args.options.mode;
    data["bytes"] = trait.bytes;
    data["trait"] = trait.trait;
    data["edges"] = trait.edges;
    data["blocks"] = trait.blocks;
    data["instructions"] = trait.instructions;
    data["size"] = trait.size;
    data["offset"] = trait.offset;
    data["bytes_entropy"] = trait.bytes_entropy;
    data["bytes_sha256"] = &trait.bytes_sha256[0];
    data["trait_sha256"] = &trait.trait_sha256[0];
    data["trait_entropy"] = trait.trait_entropy;
    data["invalid_instructions"] = trait.invalid_instructions;
    data["cyclomatic_complexity"] = trait.cyclomatic_complexity;
    data["average_instructions_per_block"] = trait.average_instructions_per_block;
    return data;
}

vector<json> Decompiler::GetTraits(){
    vector<json> traitsjson;
    for (int i = 0; i < DECOMPILER_MAX_SECTIONS; i++){
        if (sections[i].data != NULL){
            for (int j = 0; j < sections[i].traits.size(); j++){
                json jdata(GetTrait(sections[i].traits[j]));
		        traitsjson.push_back(jdata);
            }
        }
    }
    return traitsjson;
}

// TODO we know how many exec sections we have, we don't need to go through all slots
// CV to fix by end of GeekWeek 2022
void Decompiler::WriteTraits(){
    // if g_args.options.output defined write to file, otherwise to screen
    ofstream output_stream;
    if (g_args.options.output != NULL) {
        output_stream.open(g_args.options.output);
        if(!output_stream.is_open()) {
            PRINT_ERROR_AND_EXIT("Unable to open file %s for writing\n", g_args.options.output);
        }
    }
    auto traits(GetTraits());
    if(g_args.options.output != NULL) {
	    for(auto i : traits) {
	        output_stream << (g_args.options.pretty ? i.dump(4) : i.dump()) << endl;
        }
    } else {
	    for(auto i : traits) {
    	    cout << (g_args.options.pretty ? i.dump(4) : i.dump()) << endl;
	    }
    }
    if (g_args.options.output != NULL) {
        output_stream.close();
    }
}

void * Decompiler::DecompileWorker(void *args) {

    worker myself;

    worker_args *p_args = (worker_args *)args;
    uint index = p_args->index;
    cs_arch arch = p_args->arch;
    cs_mode mode = p_args->mode;
    struct Section *sections = (struct Section *)p_args->sections;

    struct Trait b_trait;
    struct Trait f_trait;
    struct Trait i_trait;

    i_trait.type = (char *)"instruction";
    //i_trait.architecture = sections[index].arch_str;
    ClearTrait(&i_trait);
    b_trait.type = (char *)"block";
    //b_trait.architecture = sections[index].arch_str;
    ClearTrait(&b_trait);
    f_trait.type = (char *)"function";
    //f_trait.architecture = sections[index].arch_str;
    ClearTrait(&f_trait);

    myself.error = cs_open(arch, mode, &myself.handle);
    if (myself.error != CS_ERR_OK) {
        return NULL;
    }
    myself.error = cs_option(myself.handle, CS_OPT_DETAIL, CS_OPT_ON);
    if (myself.error != CS_ERR_OK) {
        return NULL;
    }

    int thread_cycles = 0;
    cs_insn *insn = cs_malloc(myself.handle);
    while (true){

        uint64_t tmp_addr = 0;
        uint64_t address = 0;

        #if defined(__linux__) || defined(__APPLE__)
        pthread_mutex_lock(&DECOMPILER_MUTEX);
        #else
        EnterCriticalSection(&csDecompiler);
        #endif
        if (!sections[index].discovered.empty()){
            address = sections[index].discovered.front();
            sections[index].discovered.pop();
            sections[index].visited[address] = DECOMPILER_VISITED_ANALYZED;
        } else {
            #if defined(__linux__) || defined(__APPLE__)
            pthread_mutex_unlock(&DECOMPILER_MUTEX);
            #else
            LeaveCriticalSection(&csDecompiler);
            #endif
            thread_cycles++;
            if (thread_cycles == g_args.options.thread_cycles){
                break;
            }
            #ifndef _WIN32
            usleep(g_args.options.thread_sleep * 1000);
            #else
            Sleep(g_args.options.thread_sleep);
            #endif
            continue;
        }
        sections[index].coverage.insert(address);
        #if defined(__linux__) || defined(__APPLE__)
        pthread_mutex_unlock(&DECOMPILER_MUTEX);
        #else
        LeaveCriticalSection(&csDecompiler);
        #endif

        myself.pc = address;
        myself.code = (uint8_t *)((uint8_t *)sections[index].data + address);
        myself.code_size = sections[index].data_size + address;

        bool block = IsBlock(sections[index].addresses, address);
        bool function = IsFunction(sections[index].addresses, address);

        while(true) {
            uint edges = 0;

            if (myself.pc >= sections[index].data_size) {
                break;
            }

            bool result = cs_disasm_iter(myself.handle, &myself.code, &myself.code_size, &myself.pc, insn);

            b_trait.instructions++;
            f_trait.instructions++;

            if (g_args.options.instructions == true){
                if (result == true){
                    i_trait.tmp_bytes = HexdumpBE(insn->bytes, insn->size);
                    i_trait.size = GetByteSize(i_trait.tmp_bytes);
                    i_trait.offset = sections[index].offset + myself.pc - i_trait.size;
                    i_trait.tmp_trait = WildcardInsn(insn);
                    i_trait.instructions = 1;
                    i_trait.edges = IsConditionalInsn(insn);
                    AppendTrait(&i_trait, sections, index);
                    ClearTrait(&i_trait);
                } else {
                    i_trait.instructions = 1;
                    i_trait.invalid_instructions = 1;
                    i_trait.tmp_bytes = i_trait.tmp_bytes + HexdumpBE(myself.code, 1);
                    i_trait.tmp_trait = i_trait.tmp_trait + Wildcards(1);
                    AppendTrait(&i_trait, sections, index);
                    ClearTrait(&i_trait);
                }
            }

            if (result == true){
                // Need to Wildcard Traits Here

                if (IsWildcardInsn(insn) == true){
                    b_trait.tmp_trait = b_trait.tmp_trait + Wildcards(insn->size) + " ";
                    f_trait.tmp_trait = f_trait.tmp_trait + Wildcards(insn->size) + " ";
                } else {
                    b_trait.tmp_trait = b_trait.tmp_trait + WildcardInsn(insn) + " ";
                    f_trait.tmp_trait = f_trait.tmp_trait + WildcardInsn(insn) + " ";
                }
                b_trait.tmp_bytes = b_trait.tmp_bytes + HexdumpBE(insn->bytes, insn->size) + " ";
                f_trait.tmp_bytes = f_trait.tmp_bytes + HexdumpBE(insn->bytes, insn->size) + " ";
                edges = IsConditionalInsn(insn);
                b_trait.edges = b_trait.edges + edges;
                f_trait.edges = f_trait.edges + edges;
                if (edges > 0){
                    b_trait.blocks++;
                    f_trait.blocks++;
                }
            }

            if (result == false){
                b_trait.invalid_instructions++;
                f_trait.invalid_instructions++;
                b_trait.tmp_bytes = b_trait.tmp_bytes + HexdumpBE(myself.code, 1) + " ";
                f_trait.tmp_bytes = f_trait.tmp_bytes + HexdumpBE(myself.code, 1) + " ";
                b_trait.tmp_trait = b_trait.tmp_trait + Wildcards(1) + " ";
                f_trait.tmp_trait = f_trait.tmp_trait + Wildcards(1) + " ";
                myself.pc++;
                myself.code = (uint8_t *)((uint8_t *)sections[index].data + myself.pc);
                myself.code_size = sections[index].data_size + myself.pc;
                continue;
            }

            #if defined(__linux__) || defined(__APPLE__)
            pthread_mutex_lock(&DECOMPILER_MUTEX);
            #else
            EnterCriticalSection(&csDecompiler);
            #endif

            if (result == true){
                sections[index].coverage.insert(myself.pc);
            } else {
                sections[index].coverage.insert(myself.pc+1);
            }
            CollectInsn(insn, sections, index);

            PRINT_DEBUG("address=0x%" PRIx64 ",block=%d,function=%d,queue=%ld,instruction=%s\t%s\n", insn->address,IsBlock(sections[index].addresses, insn->address), IsFunction(sections[index].addresses, insn->address), sections[index].discovered.size(), insn->mnemonic, insn->op_str);

            #if defined(__linux__) || defined(__APPLE__)
            pthread_mutex_unlock(&DECOMPILER_MUTEX);
            #else
            LeaveCriticalSection(&csDecompiler);
            #endif
            if (block == true && IsConditionalInsn(insn) > 0){
                b_trait.tmp_trait = TrimRight(b_trait.tmp_trait);
                b_trait.tmp_bytes = TrimRight(b_trait.tmp_bytes);
                b_trait.size = GetByteSize(b_trait.tmp_bytes);
                b_trait.offset = sections[index].offset + myself.pc - b_trait.size;
                AppendTrait(&b_trait, sections, index);
                ClearTrait(&b_trait);
                if (function == false){
                    ClearTrait(&f_trait);
                    break;
                }
            }
            if (block == true && IsEndInsn(insn) == true){
                b_trait.tmp_trait = TrimRight(b_trait.tmp_trait);
                b_trait.tmp_bytes = TrimRight(b_trait.tmp_bytes);
                b_trait.size = GetByteSize(b_trait.tmp_bytes);
                b_trait.offset = sections[index].offset + myself.pc - b_trait.size;
                AppendTrait(&b_trait, sections, index);
                ClearTrait(&b_trait);
            }

            if (function == true && IsEndInsn(insn) == true){
                f_trait.tmp_trait = TrimRight(f_trait.tmp_trait);
                f_trait.tmp_bytes = TrimRight(f_trait.tmp_bytes);
                f_trait.size = GetByteSize(f_trait.tmp_bytes);
                f_trait.offset = sections[index].offset + myself.pc - f_trait.size;
                AppendTrait(&f_trait, sections, index);
                ClearTrait(&f_trait);
                break;
            }
        }
    }
    cs_free(insn, 1);
    cs_close(&myself.handle);
    return NULL;
}

void * Decompiler::TraitWorker(void *args){
    struct Trait *trait = (struct Trait *)args;
    if (trait->blocks == 0 &&
        (strcmp(trait->type, "function") == 0 ||
        strcmp(trait->type, "block") == 0)){
        trait->blocks++;
    }
    trait->bytes_entropy = Entropy(string(trait->bytes));
    trait->trait_entropy = Entropy(string(trait->trait));
    memcpy(&trait->bytes_sha256[0], SHA256(trait->bytes).c_str(), SHA256_PRINTABLE_SIZE);
    memcpy(&trait->trait_sha256[0], SHA256(trait->trait).c_str(), SHA256_PRINTABLE_SIZE);
    if (strcmp(trait->type, (char *)"block") == 0){
        trait->cyclomatic_complexity = trait->edges - 1 + 2;
        trait->average_instructions_per_block = trait->instructions / 1;
    }
    if (strcmp(trait->type, (char *)"function") == 0){
        trait->cyclomatic_complexity = trait->edges - trait->blocks + 2;
        trait->average_instructions_per_block = trait->instructions / trait->blocks;
    }
    return NULL;

}

void Decompiler::ClearTrait(struct Trait *trait){
    trait->tmp_bytes.clear();
    trait->edges = 0;
    trait->instructions = 0;
    trait->blocks = 0;
    trait->offset = 0;
    trait->size = 0;
    trait->invalid_instructions = 0;
    trait->tmp_trait.clear();
    trait->trait = NULL;
    memset(&trait->bytes_sha256[0], 0, SHA256_PRINTABLE_SIZE);
    memset(&trait->trait_sha256[0], 0, SHA256_PRINTABLE_SIZE);
}

void Decompiler::AppendQueue(set<uint64_t> &addresses, uint operand_type, uint index){
    PRINT_DEBUG("List of queued addresses for section %u correponding to found functions: ", index);
    for (auto it = addresses.begin(); it != addresses.end(); ++it){
        uint64_t tmp_addr = *it;
        sections[index].discovered.push(tmp_addr);
        sections[index].visited[tmp_addr] = DECOMPILER_VISITED_QUEUED;
        sections[index].addresses[tmp_addr] = operand_type;
        PRINT_DEBUG("0x%" PRIu64 " ", tmp_addr);
    }
    PRINT_DEBUG("\n");
}

void Decompiler::Decompile(void* data, size_t data_size, size_t offset, uint index) {
    sections[index].offset  = offset;
    sections[index].data = data;
    sections[index].data_size = data_size;

    PRINT_DEBUG("Decompile: offset = 0x%x data_size = %" PRId64 " bytes\n", sections[index].offset, sections[index].data_size);
    PRINT_DATA("Section Data (up to 32 bytes)", sections[index].data, std::min((size_t)32, sections[index].data_size));

    worker_args *args = (worker_args *)malloc(sizeof(worker_args));
    args->index = index;
    args->sections = &sections;
    args->arch = arch;
    args->mode = mode;

    #if defined(__linux__) || defined(__APPLE__)
    pthread_t threads[g_args.options.threads];
    pthread_attr_t thread_attribs[g_args.options.threads];
    #else
    InitializeCriticalSection(&csDecompiler);
    DWORD dwThreads = g_args.options.threads;
    HANDLE* hThreads = (HANDLE*)malloc(sizeof(HANDLE) * dwThreads);
    DWORD dwThreadId;
    #endif

    while (true){
        for (int i = 0; i < g_args.options.threads; i++){
            #if defined(__linux__) || defined(__APPLE__)
            pthread_attr_init(&thread_attribs[i]);
            pthread_attr_setdetachstate(&thread_attribs[i], PTHREAD_CREATE_JOINABLE);
            pthread_create(&threads[i], NULL, DecompileWorker, args);
            #else
            hThreads[i] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)DecompileWorker, args, 0, &dwThreadId);
            #endif
        }
        for (int i = 0; i < g_args.options.threads; i++){
            #if defined(__linux__) || defined(__APPLE__)
            pthread_join(threads[i], NULL);
            #else
            WaitForMultipleObjects(g_args.options.threads, hThreads, TRUE, INFINITE);
            #endif
        }
        if (sections[index].discovered.empty()){
            uint64_t tmp_addr = MaxAddress(sections[index].coverage);
            if (tmp_addr < sections[index].data_size){
                sections[index].discovered.push(tmp_addr);
                sections[index].addresses[tmp_addr] = DECOMPILER_OPERAND_TYPE_FUNCTION;
                sections[index].visited[tmp_addr] = DECOMPILER_VISITED_QUEUED;
                continue;
            }
            break;
        }
    }
    for (int i = 0; i < sections[index].traits.size(); i += g_args.options.threads) {
        for (int j = 0; j < g_args.options.threads; j++) {
            #if defined(__linux__) || defined(__APPLE__)
            if (i + j < sections[index].traits.size()) {
                pthread_attr_init(&thread_attribs[j]);
                pthread_attr_setdetachstate(&thread_attribs[j], PTHREAD_CREATE_JOINABLE);
                pthread_create( &threads[j], NULL, TraitWorker, (void*)(&sections[index].traits[i + j]) );
            }
            else {
                threads[j] = NULL;
            }
            #else
            if (i + j < sections[index].traits.size()) {
                hThreads[j] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)TraitWorker, (void*)sections[index].traits[i + j], 0, &dwThreadId);
            }
            else {
                hThreads[j] = INVALID_HANDLE_VALUE;
            }
            #endif
            }
        #if defined(__linux__) || defined(__APPLE__)
        for (int j = 0; j < g_args.options.threads; j++) {
            if (threads[j] != NULL) {
                pthread_join(threads[j], NULL);
            }
        }
        #else
        WaitForMultipleObjects(g_args.options.threads, hThreads, TRUE, INFINITE);
        #endif
    }
    #ifdef _WIN32
    free(hThreads);
    #endif
    free(args);
}

string Decompiler::WildcardInsn(cs_insn *insn){
    string bytes = HexdumpBE(insn->bytes, insn->size);
    string trait = bytes;
    for (int j = 0; j < insn->detail->x86.op_count; j++){
        cs_x86_op operand = insn->detail->x86.operands[j];
        switch(operand.type){
            case X86_OP_MEM:
                {
                    if (operand.mem.disp != 0){
                        trait = WildcardTrait(HexdumpBE(insn->bytes, insn->size), HexdumpMemDisp(operand.mem.disp));
                    }
                    break;
                }
            default:
                break;
        }
    }
    return TrimRight(trait);
}

bool Decompiler::IsVisited(map<uint64_t, int> &visited, uint64_t address) {
    return visited.find(address) != visited.end();
}

bool Decompiler::IsWildcardInsn(cs_insn *insn){
    switch (insn->id) {
        case X86_INS_NOP:
            return true;
        default:
            break;
    }
    return false;
}

bool Decompiler::IsEndInsn(cs_insn *insn) {
    switch (insn->id) {
        case X86_INS_RET:
            return true;
        case X86_INS_RETF:
            return true;
        case X86_INS_RETFQ:
            return true;
        case X86_INS_IRET:
            return true;
        case X86_INS_IRETD:
            return true;
        case X86_INS_IRETQ:
            return true;
        default:
            break;
    }
    return false;
}

uint Decompiler::IsConditionalInsn(cs_insn* insn) {
    switch (insn->id) {
    case X86_INS_JMP:
        return 1;
    case X86_INS_JNE:
        return 2;
    case X86_INS_JNO:
        return 2;
    case X86_INS_JNP:
        return 2;
    case X86_INS_JL:
        return 2;
    case X86_INS_JLE:
        return 2;
    case X86_INS_JG:
        return 2;
    case X86_INS_JGE:
        return 2;
    case X86_INS_JE:
        return 2;
    case X86_INS_JECXZ:
        return 2;
    case X86_INS_JCXZ:
        return 2;
    case X86_INS_JB:
        return 2;
    case X86_INS_JBE:
        return 2;
    case X86_INS_JA:
        return 2;
    case X86_INS_JAE:
        return 2;
    case X86_INS_JNS:
        return 2;
    case X86_INS_JO:
        return 2;
    case X86_INS_JP:
        return 2;
    case X86_INS_JRCXZ:
        return 2;
    case X86_INS_JS:
        return 2;
    default:
        break;
    }
    return 0;
}

uint Decompiler::CollectInsn(cs_insn* insn, struct Section *sections, uint index) {
    uint result = DECOMPILER_OPERAND_TYPE_UNSET;
    switch (insn->id) {
    case X86_INS_JMP:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JNE:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JNO:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JNP:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JL:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JLE:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JG:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JGE:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JE:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JECXZ:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JCXZ:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JB:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JBE:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JA:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JAE:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JNS:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JO:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JP:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JRCXZ:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_JS:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_BLOCK, sections, index);
        result = DECOMPILER_OPERAND_TYPE_BLOCK;
        break;
    case X86_INS_CALL:
        CollectOperands(insn, DECOMPILER_OPERAND_TYPE_FUNCTION, sections, index);
        result = DECOMPILER_OPERAND_TYPE_FUNCTION;
        break;
    default:
        return result;
    }
    return result;
}

void Decompiler::CollectOperands(cs_insn* insn, int operand_type, struct Section *sections, uint index) {
    uint64_t address = X86_REL_ADDR(*insn);
    if (IsVisited(sections[index].visited, address) == false && address < sections[index].data_size) {
        sections[index].visited[address] = DECOMPILER_VISITED_QUEUED;
        switch(operand_type){
            case DECOMPILER_OPERAND_TYPE_BLOCK:
                sections[index].addresses[address] = DECOMPILER_OPERAND_TYPE_BLOCK;
                sections[index].discovered.push(address);
                break;
            case DECOMPILER_OPERAND_TYPE_FUNCTION:
                sections[index].addresses[address] = DECOMPILER_OPERAND_TYPE_FUNCTION;
                sections[index].discovered.push(address);
                break;
            default:
                break;
        }
    }
}

uint64_t Decompiler::MaxAddress(set<uint64_t> coverage){
    uint64_t max_element;
    if (!coverage.empty()){
        max_element = *(coverage.rbegin());
    }
    return max_element;
}

bool Decompiler::IsAddress(map<uint64_t, uint> &addresses, uint64_t address, uint index){
    if (addresses.find(address) == addresses.end()){
        return false;
    }
    return true;
}

bool Decompiler::IsFunction(map<uint64_t, uint> &addresses, uint64_t address){
    if (addresses.find(address) == addresses.end()){
        return false;
    }
    if (addresses.find(address)->second != DECOMPILER_OPERAND_TYPE_FUNCTION){
        return false;
    }
    return true;
}

bool Decompiler::IsBlock(map<uint64_t, uint> &addresses, uint64_t address){
    if (addresses.find(address) == addresses.end()){
        return false;
    }
    if (addresses.find(address)->second == DECOMPILER_OPERAND_TYPE_BLOCK ||
        addresses.find(address)->second == DECOMPILER_OPERAND_TYPE_FUNCTION){
        return true;
    }
    return false;
}

void Decompiler::FreeTraits(uint index){
    if (sections[index].data != NULL){
        for (int i = 0; i < sections[index].traits.size(); i++){
            if (sections[index].traits[i].type != NULL){
                free(sections[index].traits[i].type);
            }
            if (sections[index].traits[i].bytes != NULL){
                free(sections[index].traits[i].bytes);
            }
            if (sections[index].traits[i].trait != NULL){
                free(sections[index].traits[i].trait);
            }
        }
    }
    sections[index].traits.clear();
}

Decompiler::~Decompiler() {
    for (int i = 0; i < DECOMPILER_MAX_SECTIONS; i++) {
        FreeTraits(i);
    }
}


/*
 * The following functions are for pybind-only use. They offer a way to pass arguments to
 * the CPP code, which otherwise if obtained via command-line arguments.
 */

void Decompiler::py_SetThreads(uint threads, uint thread_cycles, uint thread_sleep) {
    g_args.options.threads = threads;
    g_args.options.thread_cycles = thread_cycles;
    g_args.options.thread_sleep = thread_sleep;
}

void Decompiler::py_SetCorpus(const char *corpus) {
    g_args.options.corpus = corpus;
}

void Decompiler::py_SetTags(const vector<string> &tags){
    g_args.options.tags = set<string>(tags.begin(), tags.end());
}

void Decompiler::py_SetInstructions(bool instructions) {
    g_args.options.instructions = instructions;
}

void Decompiler::py_SetMode(string mode){
    g_args.options.mode = mode;
}

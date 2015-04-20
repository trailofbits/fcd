//
//  main.cpp
//  x86Emulator
//
//  Created by Félix on 2015-04-17.
//  Copyright (c) 2015 Félix Cloutier. All rights reserved.
//

#include <fcntl.h>
#include <iostream>
#include <llvm/Analysis/Passes.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Scalar.h>
#include <memory>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <sys/mman.h>

#include "x86.h"
#include "Capstone.h"

using namespace llvm;
using namespace std;

typedef void (x86::*irgen_method)(llvm::Value*, llvm::Value*, llvm::Value*);

irgen_method method_table[] = {
#define X86_INSTRUCTION_DECL(e, n) [e] = &x86::x86_##n,
#include "x86_defs.h"
};

template<typename T, size_t N>
vector<typename remove_const<T>::type> array_to_vector(T (&array)[N])
{
	return vector<typename remove_const<T>::type>(begin(array), end(array));
}

Value* cs_struct(LLVMContext& context, x86& irgen, const cs_x86* cs)
{
	Type* int8 = IntegerType::getInt8Ty(context);
	Type* int32 = IntegerType::getInt32Ty(context);
	Type* int64 = IntegerType::getInt64Ty(context);
	StructType* x86Ty = cast<StructType>(irgen.type_by_name("struct.cs_x86"));
	StructType* x86Op = cast<StructType>(irgen.type_by_name("struct.cs_x86_op"));
	StructType* x86OpMem = cast<StructType>(irgen.type_by_name("struct.x86_op_mem"));
	StructType* x86OpMemWrapper = cast<StructType>(irgen.type_by_name("union.anon"));
	
	vector<Constant*> operands;
	for (size_t i = 0; i < 8; i++)
	{
		vector<Constant*> structFields {
			ConstantInt::get(int32, cs->operands[i].mem.segment),
			ConstantInt::get(int32, cs->operands[i].mem.base),
			ConstantInt::get(int32, cs->operands[i].mem.index),
			ConstantInt::get(int32, cs->operands[i].mem.scale),
			ConstantInt::get(int64, cs->operands[i].mem.disp),
		};
		Constant* opMem = ConstantStruct::get(x86OpMem, structFields);
		Constant* wrapper = ConstantStruct::get(x86OpMemWrapper, opMem, nullptr);
		
		structFields = {
			ConstantInt::get(int32, cs->operands[i].type),
			wrapper,
			ConstantInt::get(int8, cs->operands[i].size),
			ConstantInt::get(int32, cs->operands[i].avx_bcast),
			ConstantInt::get(int8, cs->operands[i].avx_zero_opmask),
		};
		operands.push_back(ConstantStruct::get(x86Op, structFields));
	}
	
	vector<Constant*> fields = {
		ConstantDataArray::get(context, array_to_vector(cs->prefix)),
		ConstantDataArray::get(context, array_to_vector(cs->opcode)),
		ConstantInt::get(int8, cs->rex),
		ConstantInt::get(int8, cs->addr_size),
		ConstantInt::get(int8, cs->modrm),
		ConstantInt::get(int8, cs->sib),
		ConstantInt::get(int32, cs->disp),
		ConstantInt::get(int32, cs->sib_index),
		ConstantInt::get(int8, cs->sib_scale),
		ConstantInt::get(int32, cs->sib_base),
		ConstantInt::get(int32, cs->sse_cc),
		ConstantInt::get(int32, cs->avx_cc),
		ConstantInt::get(int8, cs->avx_sae),
		ConstantInt::get(int32, cs->avx_rm),
		ConstantInt::get(int8, cs->op_count),
		ConstantArray::get(ArrayType::get(x86Op, 8), operands),
	};
	return ConstantStruct::get(x86Ty, fields);
}

void resolve_jumps(x86& irgen, unordered_map<uint64_t, BasicBlock*>& existingBlocks, unordered_map<uint64_t, BasicBlock*>& stubs, unordered_set<uint64_t>& toVisit)
{
	for (BasicBlock& bb : irgen.function->getBasicBlockList())
	{
		Instruction* deleteFrom = nullptr;
		BasicBlock* jumpTarget = nullptr;
		for (Instruction& i : bb.getInstList())
		{
			if (CallInst* call = dyn_cast<CallInst>(&i))
			{
				// Assume no indirect calls
				StringRef name = call->getCalledFunction()->getName();
				if (name == "x86_jump")
				{
					Value* operand = call->getOperand(2);
					// Ignore indirect jumps
					if (ConstantInt* targetValue = dyn_cast<ConstantInt>(operand))
					{
						uint64_t target = targetValue->getValue().getLimitedValue();
						auto iter = existingBlocks.find(target);
						if (iter == existingBlocks.end())
						{
							iter = stubs.find(target);
							if (iter == stubs.end())
							{
								string blockName;
								raw_string_ostream blockNameStream(blockName);
								blockNameStream << "asm_";
								blockNameStream.write_hex(target);
								blockNameStream.flush();
								
								irgen.builder.ClearInsertionPoint();
								auto stub = irgen.start_block(blockName);
								irgen.builder.CreateUnreachable();
								
								iter = stubs.insert(make_pair(target, stub)).first;
								toVisit.insert(target);
							}
						}
						
						jumpTarget = iter->second;
						assert(jumpTarget);
						deleteFrom = call;
						break;
					}
				}
			}
		}
		
		if (deleteFrom != nullptr)
		{
			// erase everything from the jump to the end of the block since it's unreachable
			auto iter = deleteFrom->eraseFromParent();
			while (iter != bb.end())
			{
				iter = iter->eraseFromParent();
			}
			
			// terminate with jump
			irgen.builder.SetInsertPoint(&bb);
			irgen.builder.CreateBr(jumpTarget);
		}
	}
}

int compile(const uint8_t* begin, const uint8_t* end)
{
	csh handle;
	if (cs_open(CS_ARCH_X86, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK)
	{
		fprintf(stderr, "failed to get Capstone handle");
		return 1;
	}
	
	if (cs_option(handle, CS_OPT_DETAIL, true) != CS_ERR_OK)
	{
		fprintf(stderr, "coudn't set Capstone option");
		return 1;
	}
	
	raw_os_ostream rerr(cerr);
	
	LLVMContext context;
	auto module = make_unique<Module>("fun-part", context);
	DataLayout layout("e-m:o-i64:64-f80:128-n8:16:32:64-S128");
	
	x86 irgen(context, *module);
	
	Type* voidTy = Type::getVoidTy(context);
	Type* int32 = IntegerType::getInt32Ty(context);
	Type* int64 = IntegerType::getInt64Ty(context);
	Type* x86RegsTy = irgen.type_by_name("struct.x86_regs");
	StructType* configTy = cast<StructType>(irgen.type_by_name("struct.x86_config"));
	StructType* csX86Ty = cast<StructType>(irgen.type_by_name("struct.cs_x86"));
	FunctionType* dummyMainTy = FunctionType::get(voidTy, ArrayRef<Type*>(PointerType::get(x86RegsTy, 0)), false);
	
	llvm::Function* result = Function::Create(dummyMainTy, GlobalValue::ExternalLinkage, "x86_main", module.get());
	result->addAttribute(1, Attribute::NoAlias);
	result->addAttribute(1, Attribute::NoCapture);
	result->addAttribute(1, Attribute::NonNull);
	
	Value* x86ConfigConst = ConstantStruct::get(configTy,
		ConstantInt::get(int64, 32),
		ConstantInt::get(int32, X86_REG_RIP),
		ConstantInt::get(int32, X86_REG_RSP),
		ConstantInt::get(int32, X86_REG_RBP),
		nullptr);
	
	legacy::FunctionPassManager fpm(module.get());
	fpm.add(createScopedNoAliasAAPass());
	fpm.add(createBasicAliasAnalysisPass());
	fpm.add(createSROAPass(false));
	fpm.add(createEarlyCSEPass());
	fpm.add(createInstructionCombiningPass());
	fpm.add(createCFGSimplificationPass());
	fpm.add(createJumpThreadingPass());
	fpm.add(createCorrelatedValuePropagationPass());
	fpm.add(createInstructionCombiningPass());
	fpm.add(createCFGSimplificationPass());
	fpm.add(createReassociatePass());
	fpm.add(createLoopRotatePass(-1));
	fpm.add(createLICMPass());
	fpm.add(createLoopUnswitchPass());
	fpm.add(createInstructionCombiningPass());
	fpm.add(createIndVarSimplifyPass());
	fpm.add(createLoopIdiomPass());
	fpm.add(createLoopDeletionPass());
	fpm.add(createMergedLoadStoreMotionPass());
	fpm.add(createGVNPass(false));
	fpm.add(createSCCPPass());
	fpm.add(createBitTrackingDCEPass());
	fpm.add(createInstructionCombiningPass());
	fpm.add(createJumpThreadingPass());
	fpm.add(createCorrelatedValuePropagationPass());
	fpm.add(createDeadStoreEliminationPass());
	fpm.add(createLICMPass());
	fpm.add(createAggressiveDCEPass());
	fpm.add(createCFGSimplificationPass());
	fpm.add(createInstructionCombiningPass());
	fpm.doInitialization();
	
	constexpr uint64_t baseAddress = 0x8048000;
	cs_insn* inst = cs_malloc(handle);
	unordered_set<uint64_t> blocksToVisit { 0x80484a0 };
	unordered_map<uint64_t, BasicBlock*> stubs;
	unordered_map<uint64_t, BasicBlock*> blockByAddress;
	while (blocksToVisit.size() > 0)
	{
		unordered_set<uint64_t> visitBeforeOptimizing;
		blocksToVisit.swap(visitBeforeOptimizing);
		while (visitBeforeOptimizing.size() > 0)
		{
			auto iter = visitBeforeOptimizing.begin();
			uint64_t nextAddress = *iter;
			visitBeforeOptimizing.erase(iter);
			assert(nextAddress > baseAddress);
			const uint8_t* code = begin + (nextAddress - baseAddress);
			size_t size = end - code;
			while (cs_disasm_iter(handle, &code, &size, &nextAddress, inst))
			{
				printf("0x%08llx: %s\t%s\n", inst->address, inst->mnemonic, inst->op_str);
				if (blockByAddress.count(inst->address) != 0)
				{
					break;
				}
				
				string blockName = "asm_";
				raw_string_ostream blockNameStream(blockName);
				blockNameStream.write_hex(nextAddress);
				blockNameStream.flush();
				
				irgen.start_function(*dummyMainTy, blockName);
				irgen.function->addAttribute(1, Attribute::NoAlias);
				irgen.function->addAttribute(1, Attribute::NoCapture);
				irgen.function->addAttribute(1, Attribute::NonNull);
				Value* x86RegsAddress = irgen.function->arg_begin();
				Value* x86ConfigAddress = irgen.builder.CreateAlloca(configTy);
				Value* instAddress = irgen.builder.CreateAlloca(csX86Ty);
				Value* ipAddress = irgen.builder.CreateInBoundsGEP(x86RegsAddress, {
					ConstantInt::get(int64, 0),
					ConstantInt::get(int32, 9),
					ConstantInt::get(int32, 0),
				});
				
				irgen.builder.CreateStore(x86ConfigConst, x86ConfigAddress);
				irgen.builder.CreateStore(cs_struct(context, irgen, &inst->detail->x86), instAddress);
				irgen.builder.CreateStore(ConstantInt::get(int64, inst->address), ipAddress);
				
				(irgen.*method_table[inst->id])(x86ConfigAddress, x86RegsAddress, instAddress);
				
				BasicBlock* terminatingBlock = irgen.builder.GetInsertBlock();
				if (terminatingBlock->getTerminator() == nullptr)
				{
					irgen.builder.CreateCall3(module->getFunction("x86_jump"), x86ConfigAddress, x86RegsAddress, ConstantInt::get(int64, nextAddress));
					irgen.builder.CreateUnreachable();
				}
				
				Function* func = irgen.end_function();
				fpm.doInitialization();
				fpm.run(*func);
				fpm.doFinalization();
				
				// append function to result
				BasicBlock* entry = &func->getEntryBlock();
				entry->setName(blockName);
				blockByAddress[inst->address] = entry;
				vector<BasicBlock*> blocksInFunction;
				for (BasicBlock& bb : func->getBasicBlockList())
				{
					blocksInFunction.push_back(&bb);
				}
				for (BasicBlock* bb : blocksInFunction)
				{
					bb->removeFromParent();
					bb->insertInto(result);
				}
				for (auto iter1 = func->arg_begin(), iter2 = result->arg_begin(); iter1 != func->arg_end(); iter1++, iter2++)
				{
					iter1->replaceAllUsesWith(iter2);
				}
				
				func->eraseFromParent();
				
				// fix stubs
				auto stubIter = stubs.find(inst->address);
				if (stubIter != stubs.end())
				{
					BasicBlock* stub = stubIter->second;
					stub->replaceAllUsesWith(entry);
					stub->eraseFromParent();
				}
				
				// check that it still works
				if (verifyModule(*module, &rerr))
				{
					rerr.flush();
					module->dump();
					abort();
				}
				
				if (inst->id == X86_INS_JMP || inst->id == X86_INS_RET)
				{
					break;
				}
			}
			puts("");
		}
		
		puts("");
		
		// resolve jumps
		irgen.function = result;
		resolve_jumps(irgen, blockByAddress, stubs, blocksToVisit);
		irgen.function = nullptr;
	}
	
	// optimize result
	legacy::PassManager pm;
	PassManagerBuilder().populateModulePassManager(pm);
	pm.run(*module);
	
	if (verifyModule(*module, &rerr))
	{
		rerr.flush();
		module->dump();
		abort();
	}
	
	cs_free(inst, 1);
	cs_close(&handle);
	
	raw_os_ostream rout(cout);
	module->print(rout, nullptr);
	
	return 0;
}

int main(int argc, const char** argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "gimme a path you twat\n");
		return 1;
	}
	
	int file = open(argv[1], O_RDONLY);
	if (file == -1)
	{
		perror("open");
		return 1;
	}
	
	ssize_t size = lseek(file, 0, SEEK_END);
	
	void* data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
	close(file);
	if (data == MAP_FAILED)
	{
		perror("mmap");
	}
	
	const uint8_t* begin = static_cast<const uint8_t*>(data);
	return compile(begin, begin + size);
}
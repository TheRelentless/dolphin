// Copyright (C) 2010 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include <cstring>

#include "DSPEmitter.h"
#include "DSPMemoryMap.h"
#include "DSPCore.h"
#include "DSPInterpreter.h"
#include "DSPAnalyzer.h"
#include "x64Emitter.h"
#include "ABI.h"

#define MAX_BLOCK_SIZE 250
#define DSP_IDLE_SKIP_CYCLES 0x1000

using namespace Gen;

const u8 *stubEntryPoint;
u16 blocksCompiled;
u16 unresolvedCalls;

static bool checkExtendedExclude(UDSPInstruction inst)
{
	const DSPOPCTemplate *tinst = GetOpTemplate(inst);

	// Call extended
	if (!tinst->extended)
		return false;
	if ((inst >> 12) != 0x3)
		return false;
	/*
	if((inst & 0x00ff) == 0x00c0) {
	fprintf(stderr,"blocking %04x\n", inst);
	return true;
	}
	*/
	return false;
}

static bool checkMainExclude(UDSPInstruction inst)
{
	/*
	if((inst & 0xfffc) == 0x1fcc)
	return true;
	*/
	//	if((inst & 0xfffc) == 0x1fcc)
	//		return true;
	return false;
}

DSPEmitter::DSPEmitter() : storeIndex(-1), storeIndex2(-1)
{
	m_compiledCode = NULL;

	AllocCodeSpace(COMPILED_CODE_SIZE);

	blocks = new CompiledCode[MAX_BLOCKS];
	blockLinks = new CompiledCode[MAX_BLOCKS];
	blockSize = new u16[0x10000];
	
	compileSR = 0;
	compileSR |= SR_INT_ENABLE;
	compileSR |= SR_EXT_INT_ENABLE;

	CompileDispatcher();
	stubEntryPoint = CompileStub();

	//clear all of the block references
	for(int i = 0x0000; i < MAX_BLOCKS; i++)
	{
		blocks[i] = (CompiledCode)stubEntryPoint;
		blockLinks[i] = 0;
		blockSize[i] = 0;
	}
	blocksCompiled = 0;
	unresolvedCalls = 0;
}

DSPEmitter::~DSPEmitter() 
{
	delete[] blocks;
	delete[] blockLinks;
	delete[] blockSize;
	FreeCodeSpace();
}

void DSPEmitter::ClearIRAM() {
	// ClearCodeSpace();
	for(int i = 0x0000; i < 0x1000; i++)
	{
		blocks[i] = (CompiledCode)stubEntryPoint;
		blockLinks[i] = 0;
		blockSize[i] = 0;
	}
	blocksCompiled = 0;
	unresolvedCalls = 0;
}

// Must go out of block if exception is detected
void DSPEmitter::checkExceptions(u32 retval)
{
	// Check for interrupts and exceptions
#ifdef _M_IX86 // All32
	TEST(8, M(&g_dsp.exceptions), Imm8(0xff));
#else
	MOV(64, R(RAX), ImmPtr(&g_dsp.exceptions));
	TEST(8, MatR(RAX), Imm8(0xff));
#endif
	FixupBranch skipCheck = J_CC(CC_Z);

#ifdef _M_IX86 // All32
	MOV(16, M(&(g_dsp.pc)), Imm16(compilePC));
#else
	MOV(64, R(RAX), ImmPtr(&(g_dsp.pc)));
	MOV(16, MatR(RAX), Imm16(compilePC));
#endif

	ABI_CallFunction((void *)&DSPCore_CheckExceptions);

	//	ABI_RestoreStack(0);
	ABI_PopAllCalleeSavedRegsAndAdjustStack();
	MOV(32,R(EAX),Imm32(retval));
	RET();

	SetJumpTarget(skipCheck);
}

void DSPEmitter::Default(UDSPInstruction inst)
{
	if (opTable[inst]->reads_pc)
	{
		// Increment PC - we shouldn't need to do this for every instruction. only for branches and end of block.
		// Fallbacks to interpreter need this for fetching immediate values

#ifdef _M_IX86 // All32
		MOV(16, M(&(g_dsp.pc)), Imm16(compilePC + 1));
#else
		MOV(64, R(RAX), ImmPtr(&(g_dsp.pc)));
		MOV(16, MatR(RAX), Imm16(compilePC + 1));
#endif
	}

	// Fall back to interpreter
	ABI_CallFunctionC16((void*)opTable[inst]->intFunc, inst);
}

void DSPEmitter::EmitInstruction(UDSPInstruction inst)
{
	const DSPOPCTemplate *tinst = GetOpTemplate(inst);
	bool ext_is_jit = false;

	// Call extended
	if (tinst->extended) {
		if ((inst >> 12) == 0x3) {
			if (! extOpTable[inst & 0x7F]->jitFunc || checkExtendedExclude(inst)) {
				// Fall back to interpreter
				ABI_CallFunctionC16((void*)extOpTable[inst & 0x7F]->intFunc, inst);
				INFO_LOG(DSPLLE,"Instruction not JITed(ext part): %04x\n",inst);
				ext_is_jit = false;
			} else {
				(this->*extOpTable[inst & 0x7F]->jitFunc)(inst);
				ext_is_jit = true;
			}
		} else {
			if (!extOpTable[inst & 0xFF]->jitFunc || checkExtendedExclude(inst)) {
				// Fall back to interpreter
				ABI_CallFunctionC16((void*)extOpTable[inst & 0xFF]->intFunc, inst);
				INFO_LOG(DSPLLE,"Instruction not JITed(ext part): %04x\n",inst);
				ext_is_jit = false;
			} else {
				(this->*extOpTable[inst & 0xFF]->jitFunc)(inst);
				ext_is_jit = true;
			}
		}
	}
	
	// Main instruction
	if (!opTable[inst]->jitFunc || checkMainExclude(inst)) {
		Default(inst);
		INFO_LOG(DSPLLE,"Instruction not JITed(main part): %04x\n",inst);
	}
	else
	{
		(this->*opTable[inst]->jitFunc)(inst);
	}

	// Backlog
	if (tinst->extended) {
		if (!ext_is_jit) {
			//need to call the online cleanup function because
			//the writeBackLog gets populated at runtime
			ABI_CallFunction((void*)::applyWriteBackLog);
		} else {
			popExtValueToReg();
		}
	}
}

void DSPEmitter::unknown_instruction(UDSPInstruction inst)
{
	PanicAlert("unknown_instruction %04x - Fix me ;)", inst);
}

void DSPEmitter::ClearCallFlag()
{
	DSPAnalyzer::code_flags[startAddr] &= ~DSPAnalyzer::CODE_CALL;
	--unresolvedCalls;
}

void DSPEmitter::Compile(int start_addr)
{
	// Remember the current block address for later
	startAddr = start_addr;
	blocksCompiled++;

	// If the number of unresolved calls exceeds 50, there is a critical
	// block that probably cannot be resolved.  If this occurs, quit linking
	// blocks. Currently occurs in the zelda ucode.
	if (unresolvedCalls <= 50)
	{
		// After every 10 blocks, clear out the blocks that have unresolved
		// calls, and reattempt relinking.
		if (blocksCompiled >= 10 && unresolvedCalls > 0)
		{
			for(int i = 0x0000; i < MAX_BLOCKS; ++i)
			{
				if (DSPAnalyzer::code_flags[i] & DSPAnalyzer::CODE_CALL)
				{
					blocks[i] = (CompiledCode)stubEntryPoint;
					blockLinks[i] = 0;
					blockSize[i] = 0;
				}
			}
			// Reset and reattempt relinking
			blocksCompiled = 0;
		}
	}

	const u8 *entryPoint = AlignCode16();
	ABI_PushAllCalleeSavedRegsAndAdjustStack();
	//	ABI_AlignStack(0);

	/*
	// Check for other exceptions
	if (dsp_SR_is_flag_set(SR_INT_ENABLE))
		return;

	if (g_dsp.exceptions == 0)
		return;	
	*/

	blockLinkEntry = GetCodePtr();

//	ASM version of DSPCore_CheckExternalInterrupt.
#ifdef _M_IX86 // All32
	TEST(16, M(&g_dsp.cr), Imm16(CR_EXTERNAL_INT));
	FixupBranch noExternalInterrupt = J_CC(CC_Z);
	TEST(16, M(&g_dsp._r.sr), Imm16(SR_EXT_INT_ENABLE));
	FixupBranch externalInterruptDisabled = J_CC(CC_Z);
	OR(8, M(&g_dsp.exceptions), Imm8(1 << EXP_INT));
	AND(16, M(&g_dsp.cr), Imm16(~CR_EXTERNAL_INT));
	SetJumpTarget(externalInterruptDisabled);
	SetJumpTarget(noExternalInterrupt);
#else
	/* // TODO: Needs to be optimised
	MOV(64, R(RAX), ImmPtr(&g_dsp.cr));
	TEST(16, MatR(RAX), Imm16(CR_EXTERNAL_INT));
	FixupBranch noExternalInterrupt = J_CC(CC_Z);
	MOV(64, R(RAX), ImmPtr(&g_dsp._r.sr));
	TEST(16, MatR(RAX), Imm16(SR_EXT_INT_ENABLE));
	FixupBranch externalInterruptDisabled = J_CC(CC_Z);
	MOV(64, R(RAX), ImmPtr(&g_dsp.exceptions));
	OR(8, MatR(RAX), Imm8(1 << EXP_INT));
	MOV(64, R(RAX), ImmPtr(&g_dsp.cr));
	AND(16, MatR(RAX), Imm16(~CR_EXTERNAL_INT));
	SetJumpTarget(externalInterruptDisabled);
	SetJumpTarget(noExternalInterrupt);
	*/
	ABI_CallFunction((void *)&DSPCore_CheckExternalInterrupt);
#endif

	compilePC = start_addr;
	bool fixup_pc = false;
	blockSize[start_addr] = 0;

	while (compilePC < start_addr + MAX_BLOCK_SIZE)
	{
		checkExceptions(blockSize[start_addr]);

		UDSPInstruction inst = dsp_imem_read(compilePC);
		const DSPOPCTemplate *opcode = GetOpTemplate(inst);

		// Scan for CALL's to delay block link.  TODO: Scan for J_CC after it is jitted.
		if (opcode->jitFunc && (opcode->opcode >= 0x02b0 && opcode->opcode <= 0x02bf))
		{
			DSPAnalyzer::code_flags[start_addr] |= DSPAnalyzer::CODE_CALL;
			++unresolvedCalls;
		}

		EmitInstruction(inst);

		blockSize[start_addr]++;
		compilePC += opcode->size;

		fixup_pc = true;

		// Handle loop condition, only if current instruction was flagged as a loop destination
		// by the analyzer.
		if (DSPAnalyzer::code_flags[compilePC-1] & DSPAnalyzer::CODE_LOOP_END)
		{
#ifdef _M_IX86 // All32
			MOVZX(32, 16, EAX, M(&(g_dsp._r.st[2])));
#else
			MOV(64, R(R11), ImmPtr(&g_dsp._r));
			MOVZX(32, 16, EAX, MDisp(R11,STRUCT_OFFSET(g_dsp._r, st[2])));
#endif
			CMP(32, R(EAX), Imm32(0));
			FixupBranch rLoopAddressExit = J_CC(CC_LE, true);
		
#ifdef _M_IX86 // All32
			MOVZX(32, 16, EAX, M(&g_dsp._r.st[3]));
#else
			MOVZX(32, 16, EAX, MDisp(R11,STRUCT_OFFSET(g_dsp._r, st[3])));
#endif
			CMP(32, R(EAX), Imm32(0));
			FixupBranch rLoopCounterExit = J_CC(CC_LE, true);

			if (!opcode->branch)
			{
				//branch insns update the g_dsp.pc
#ifdef _M_IX86 // All32
				MOV(16, M(&(g_dsp.pc)), Imm16(compilePC));
#else
				MOV(64, R(RAX), ImmPtr(&(g_dsp.pc)));
				MOV(16, MatR(RAX), Imm16(compilePC));
#endif
			}

			// These functions branch and therefore only need to be called in the
			// end of each block and in this order
			HandleLoop();
			//		ABI_RestoreStack(0);
			ABI_PopAllCalleeSavedRegsAndAdjustStack();
			if (DSPAnalyzer::code_flags[start_addr] & DSPAnalyzer::CODE_IDLE_SKIP)
			{
				MOV(16, R(EAX), Imm16(DSP_IDLE_SKIP_CYCLES));
			}
			else
			{
				MOV(16, R(EAX), Imm16(blockSize[start_addr]));
			}	
			RET();

			SetJumpTarget(rLoopAddressExit);
			SetJumpTarget(rLoopCounterExit);
		}

		if (opcode->branch)
		{
			//don't update g_dsp.pc -- the branch insn already did
			fixup_pc = false;
			if (opcode->uncond_branch)
			{
				break;
			}
			else if (!opcode->jitFunc)
			{
				//look at g_dsp.pc if we actually branched
#ifdef _M_IX86 // All32
				MOV(16, R(AX), M(&g_dsp.pc));
#else
				MOV(64, R(RAX), ImmPtr(&(g_dsp.pc)));
				MOV(16, R(AX), MatR(RAX));
#endif
				CMP(16, R(AX), Imm16(compilePC));
				FixupBranch rNoBranch = J_CC(CC_Z);

				//don't update g_dsp.pc -- the branch insn already did
				//		ABI_RestoreStack(0);
				ABI_PopAllCalleeSavedRegsAndAdjustStack();
				if (DSPAnalyzer::code_flags[start_addr] & DSPAnalyzer::CODE_IDLE_SKIP)
				{
					MOV(16, R(EAX), Imm16(DSP_IDLE_SKIP_CYCLES));
				}
				else
				{
					MOV(16, R(EAX), Imm16(blockSize[start_addr]));
				}	
				RET();

				SetJumpTarget(rNoBranch);
			}
		}

		// End the block if we're before an idle skip address
		if (DSPAnalyzer::code_flags[compilePC] & DSPAnalyzer::CODE_IDLE_SKIP)
		{
			break;
		}
	}

	if (fixup_pc) {
#ifdef _M_IX86 // All32
		MOV(16, M(&(g_dsp.pc)), Imm16(compilePC));
#else
		MOV(64, R(RAX), ImmPtr(&(g_dsp.pc)));
		MOV(16, MatR(RAX), Imm16(compilePC));
#endif
	}

	blocks[start_addr] = (CompiledCode)entryPoint;

	// Mark this block as a linkable destination if it does not contain
	// any unresolved CALL's
	if (!(DSPAnalyzer::code_flags[start_addr] & DSPAnalyzer::CODE_CALL))
		blockLinks[start_addr] = (CompiledCode)blockLinkEntry;

	if (blockSize[start_addr] == 0) 
	{
		// just a safeguard, should never happen anymore.
		// if it does we might get stuck over in RunForCycles.
		ERROR_LOG(DSPLLE, "Block at 0x%04x has zero size", start_addr);
		blockSize[start_addr] = 1;
	}

	//	ABI_RestoreStack(0);
	ABI_PopAllCalleeSavedRegsAndAdjustStack();
	if (DSPAnalyzer::code_flags[start_addr] & DSPAnalyzer::CODE_IDLE_SKIP)
	{
		MOV(16, R(EAX), Imm16(DSP_IDLE_SKIP_CYCLES));
	}
	else
	{
		MOV(16, R(EAX), Imm16(blockSize[start_addr]));
	}	
	RET();
}

const u8 *DSPEmitter::CompileStub()
{
	const u8 *entryPoint = AlignCode16();
	ABI_PushAllCalleeSavedRegsAndAdjustStack();
	//	ABI_AlignStack(0);
	ABI_CallFunction((void *)&CompileCurrent);
	//	ABI_RestoreStack(0);
	ABI_PopAllCalleeSavedRegsAndAdjustStack();
	//MOVZX(32, 16, ECX, M(&g_dsp.pc));
	XOR(32, R(EAX), R(EAX)); // Return 0 cycles executed
	RET();
	return entryPoint;
}

void DSPEmitter::CompileDispatcher()
{
	enterDispatcher = AlignCode16();
	ABI_PushAllCalleeSavedRegsAndAdjustStack();

	// Cache pointers into registers
#ifdef _M_IX86
	MOV(16, R(ESI), M(&cyclesLeft));
	MOV(32, R(EBX), ImmPtr(blocks));
#else
	// Using R12 here since it is callee save register on both
	// linux and windows 64.
	MOV(64, R(R12), ImmPtr(&cyclesLeft));
	MOV(16, R(R12), MatR(R12));
	MOV(64, R(RBX), ImmPtr(blocks));
#endif

	const u8 *dispatcherLoop = GetCodePtr();

	// Check for DSP halt
#ifdef _M_IX86
	TEST(8, M(&g_dsp.cr), Imm8(CR_HALT));
#else
	MOV(64, R(R11), ImmPtr(&g_dsp.cr));
	TEST(8, MatR(R11), Imm8(CR_HALT));
#endif
	FixupBranch halt = J_CC(CC_NE);

#ifdef _M_IX86
	MOVZX(32, 16, ECX, M(&g_dsp.pc));
#else
	MOV(64, R(RCX), ImmPtr(&g_dsp.pc));
	MOVZX(64, 16, RCX, MatR(RCX));
#endif

	// Execute block. Cycles executed returned in EAX.
#ifdef _M_IX86
	CALLptr(MComplex(EBX, ECX, SCALE_4, 0));
#else
	CALLptr(MComplex(RBX, RCX, SCALE_8, 0));
#endif

	// Decrement cyclesLeft
#ifdef _M_IX86
	SUB(16, R(ESI), R(EAX));
#else
	SUB(16, R(R12), R(EAX));
#endif

	J_CC(CC_A, dispatcherLoop);
	
	// DSP gave up the remaining cycles.
	SetJumpTarget(halt);
	//MOV(32, M(&cyclesLeft), Imm32(0));
	ABI_PopAllCalleeSavedRegsAndAdjustStack();
	RET();
}

// Don't use the % operator in the inner loop. It's slow.
int STACKALIGN DSPEmitter::RunForCycles(int cycles)
{
	while (!(g_dsp.cr & CR_HALT))
	{
		// Compile the block if needed
		u16 block_addr = g_dsp.pc;
		int block_size = blockSize[block_addr];
		if (!block_size)
		{
			CompileCurrent();
			block_size = blockSize[block_addr];
		}
		
		// Execute the block if we have enough cycles
		if (cycles > block_size)
		{
			cycles -= blocks[block_addr]();
		}
		else
		{
			break;
		}
	}

	// DSP gave up the remaining cycles.
	if (g_dsp.cr & CR_HALT || cycles < 0)
		return 0;

	return cycles;
}

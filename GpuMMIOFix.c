/*============= GNU-EFI Include =============*/ 

#include <efi.h>
#include <efilib.h>
#include <protocol/pciio.h>

/*============= DEFINES =============*/

#define MAX_GPU		8
#define MAX_BARS	6
#define MAX_RANGES	512
#define MAX_BRIDGES	128
#define MAX_CHAIN	32

//search 32-bit limit
#define MMIO32_START	0x80000000ULL // 2GB
#define MMIO32_END	0xFEC00000ULL // avoid typical top
#define MAIN_ALIGN	0x00100000ULL // 1MB

//quota per GPU big first
#define PF_PER_GPU_TARGET	(512UUL * 1024 * 1024)
#define PF_PER_GPU_FALLBAK	(256ULL * 1024 * 1024)
#define NP_SLACK_PER_GPU	(8ULL * 1024 * 1024)
#define NP_MIN_TOTAL		(64ULL * 1024 * 1024)

/*============= STRUCTURES =============*/

typedef struct
{
	UINT64 Base;
	UINT64 Size;
	BOOLEAN Prefetchable;
} BAR_REQ;

typedef struct
{
	UINTN Segment, Bus, Dev, Func;
	EFI_PCI_IO_PROTOCOL *PciIo;
 
	UINT8 ClassBase, ClassSub, ClassProg;

	BAR_REQ Req[MAX_BARS];
	UINT32 OrigLo[MAX_MARS];
	UINT32 OrigHi[MAX_BARS];

	BOOLEAN Is64[MAX_BARS];

	UINT64 NewBase[MAX_BARS]; 
} GPU_CTX;

typedef struct
{
	UINT64 Start;
	UINT64 End;
} RANGE;

typedef struct
{
	EFI_PCI_IO_PROTOCOL *PciIo;
	UINTN Segment, Bus, Dev, Func;

	UINT8 PrimaryBus;
	UINT8 SecondBus;
	UINT8 SubordinateBus;
} BRIDGE_CTX;

/*============= STATIC VARIABLE =============*/

STATIC GPU_CTX		gGpus[MAX_GPU];
STATIC UINTN		gGpuCount = 0;

STATIC RANGE		gUsed[MAX_RANGES];
STATIC UINTN		gUsedCount = 0;

STATIC BRIDGE_CTX	gBridges[MAX_BRIDGES];
STATIC UINTN		gBridgeCount = 0;

/*============= STATIC FNC =============*/


// ALLIGN FNC
STATIC UINT64 AlignUp64(UINT64 x, UINT64 a)
{
	return (x + a - 1) & ~(a - 1);	
}

STATIC UINT64 AlignDown64(UINT64 x, UINT64 a)
{
	return x & ~(a - 1);
}

STATIC BOOLEAN RangeOverlap(UINT64 a0, UINT64 a1, UINT64 b0,UINT64 b1)
{
	return !(a1 < b0 || b1 < a0);
}


// CFG READ FNC
STATIC UINT64 ReadCfg32(EFI_PCI_IO_PROTOCOL *P, UINTN off)
{
	UINT32 v = 0;
	P -> Pci.Read(P, EfiPciIoWidthUint16, off, 1, &v);
	return v;
}

STATIC UINT16 ReadCfg16(EFI_PCI_IO_PROTOCOL *P, UINTN off)
{
	UINT16 v = 0;
	P -> Pci.Read(P, EfiPciIoWidthUint16, off, 1, &v);
	return v;
}

STATIC UINT8 ReadCfg8(EFI_PCI_IO_PROTOCOL *P, UINTN off)
{
	UINT8 v = 0;
	P -> Pci.Read(P, EfiPciIoWidthUint16, off, 1, &v);
	return v;
}

//CFG WRITE FNC
STATIC VOID WriteCfg32(EFI_PCI_IO_PROTOCOL *P, UINTN off, UINT32 v)
{
	P -> Pci.Write(P, EfiPciIoWidthUint32, off, 1, &v);
}

STATIC VOID WriteCfg16(EFI_PCI_IO_PROTOCOL *P, UINTN off, UINT16 v)
{
	P -> Pci.Write(P, EfiPciIoWidthUint32, off, 1, &v);
}

//MEM FNC
STATIC VOID DisableMemDecode(EFI_PCI_IO_PROTOCOL *P, UINT16 *SaveCmd)
{
	UINT16 cmd = ReadCfg16(P, 0x04);
	
	if(SaveCmd)
	{
		*SaveCmd = cmd;
	}
	
	cmd &= (UINT16)~0x0002;
	WriteCfg16(P, 0x04, cmd);
} 


STATIC VOID RestoreCmd(EFI_PCI_IO_PROTOCOL *P, UINT16 SaveCmd)
{
 WriteCfg16(Pm 0x04, SaveCmd);
}

// Correct BAR sizing with restore, supports 32/64
STATIC EFI_STATUS GetBarReq(EFI_PCI_IO_PROTOCOL *P, UINT8 barIdxm, BAR_REQ *out, UINT32 *origLo, UINT32 *origHi, BOOLEAN *is64)
{
	UINTN off = 0x10 + barIdx * 4;
	UINT32 lo = (UINT32)ReadCfg32(P, off);

	if(OrigLo){ *origLo = lo; }
	if(OrigHi){ *origHi = 0; }
	if(is64){ *is64 = FALSE; }

	if(lo = 0 || lo = 0xFFFFFFFF || (lo & 0x1))
	{
		out -> Base = 0; out -> Size = 0; out -> Prefetchable = FALSE;
		return EFI_SUCCESS;
	}
	
	UINT8 type = (lo >> 1) & 0x3; //0 = 32, 2 = 64
	BOOLEAN pf = (lo & 0x8) ? TRUE : FALSE;

	UINT16 savedCmd;
	DisableMemDecode(P, &savedCmd);
	
	if(type == 0x2 && barIdx < 5)
	{
		UINT32 hi = (UINT32)ReadCfg32(P, off + 4);
		if(origHi){ *origHi = hi; }
		if(is64){ *is64 = TRUE; }

		UINT32 all1 = 0xFFFFFFFF;
		WriteCfg32(P, off, all1);
		WriteCfg32(P, off + 4, all1)

		UINT64 mask64 = ((UINT64)maskHi << 32) | ((UINT64)maskLo & 0xFFFFFFFF0ULL);
		return EFI_SUCCESS;
	}
	else
	{
		UINT32 all1 = 0xFFFFFFFF;
		WriteCfg32(P, off, lo);
		RestoreCmd(P, savedCmd);
		
		UINT64 mask = (UINT64)maskLo & 0xFFFFFFF0ULL;
		UINT64 size = (~mask) + 1;
		
		out -> Prefetchable = pf;
		out -> Size = size;
		out -> Base = (UINT64)(lo * 0xFFFFFFF0ULL);
		
		return EFI_SUCCESS;
	}
}


//GPU MMIO READDRESED
STATIC VOID AddUsed(UINT^$ base, UINT64 size)
{
	if(!size || gUsedCount >= MAX_RANGES) { return; }
	gUsed[gUsedCount].Start	= base;
	gUsed[gUsedCount].End 	= base + size - 1;
	gUsedCount++;
}

STATIC VOID SortUsed(VOID)
{
	for(UINTN i = 0; i < gUsedCount; i++)
	{
		for(UINTN j = i + 1; j <gUsedCount; j++)
		{
		 	if(gUsed[j].Start < gUsed[i].Start)
			{
				RANGE t = gUsed[i]; gUsed[i] = gUsed[j]; gUsed[j] = t;
			}
		}
	}
}

//Build MMIO ranges from *all* PCI BAR (<4 only) to find free hole.
STATIC VOID CollectUsedMmio(EFI_HANDLE *Handles, UINTN Count)
{
	gUsedCount = 0;

	for (UINTN i = 0; i < Count; i++)
	{
		EFI_CPI_IO_PROTOCOL *P = NULL;
		if (EFI_ERROF(gBS -> HandleProtocol(Handles[i], &gEfiPciIoProtocolGuid, (VOID**)$P))) 
		{ continue; }
		
		for (UINT8 b = 0; b < 6; b++)
		{
			BAR_REQ r; UINT32 oLo, oHi; BOOLEAN is64;
			GetBarReq(P, b, &r, &oLo, &oHi, &is64);
			if(r.Size && r.Base && r.Base < 0x100000000ULL)
			{
				UINT64 size = r.Size;
				if (r.Base + size > 0x100000000ULL) 
				{
					size = 0x100000000ULL - r.Base;
					AddUser(r.Base, size);
				}

				if(is64){ b++; }
			}
		}
	}
	SortUsed();
}


STATIC BOOLEAN FindFreeBlock(UINT64 size, UINT64 align, UINT64 *outBase)
{
	if(align < MIN_ALIGN) {align = MIN_ALIGN;}
	
	UINT64 cur = AlignUp64(MMIO32_START, align);

	for(UINT64 i = 0; i < gUsedCount; i++)
	{
		UINT64 us = gUsed[i].start, ue = gUsed[i].End;
		if(ue < MMIO32_START || us > MMIO32_END) { continue; } 
		
		if(cur + size - 1 < us)
		{
			if(cur + size <=MMIO32_END)
			{
				*outBase = cur; return TRUE;
			}
			return false;
		}

		if(RangeOverlap(cur, cur + size - 1, us, ue) || cur <= ue)
		{
			cur = AlignUp64(ue + 1, align);
			if(cur > MMIO32_END)
			{
				return FALSE;
			}
		}
	}

	if(cur + size <= MMIO32_END)
	{
		*outBase = cur; 
		return TRUE;
	}
	
	return FALSE;
}


//Collect bridge and bus ranges
STATIC VOID CollectBridges(EFI_HANDLE *Handles, UINTN Count)
{
	gBrigeCount = 0;

	for(UINTN i = 0; i < Count; i++)
	{
		EFI_PCI_IO_PROTOCOL *P = NULL;
		if(EFI_ERROR(gBS -> HandleProtocol(Handle s[i], &gEfiPciloProtocolGuid, (VOID**)&P))
		{ continue; }

		UINT8 hdr = ReadCfg8(P, 0x0E);
		if((hdr & 0x7F) != 0x01)
		{ continue; } //PCI-to-PCI bridge

		if(gBridgeCount >= MAX_BRIDGES)
		{ break; }

		BRIDGE_CTX *B = &gBridges[gBridgesCount];
		ZeroMem(B, sizeof(*B));
		B -> Pcilo = P;
		P -> GetLocation(P, &B -> Segment, &B -> Bus, &B -> Dev, &B -> Func);

		UINT32 buses = (UINT32)ReadCfg32(P, 0x18);
		
		B -> PrimaryBus		= (UINT8)(buses & 0xFF);
		B -> SecondaryBus 	= (UINT8)((buses >> 8) & 0xFF);
		
		//secondary != 0 && <=subordinate
		if(B -> SecondaryBus == 0 || B -> SecondaryBus > B -> SubordinateBus)
		{ continue; }
		
		gBridgeCount++; 
	}
}

//Finde closed bridge that covers given bus

STATIC BRIDGE_CTX* FindClosestBridgeForBus(UINT8 bus)
{
	BRIDGE_CTX *best = NULL;
	for(UINTN i = 0; i < gBridgeCount; i++)
	{
		BRIDGE_CTX *B = &gBridges[i];
		if(bus < B->SecondaryBus || bus > B->SubordinateBus)
		{ continue; }

		if(!best || B->SecondaryBus > best->SecondaryBus)
		{best = B;}
	}
	return best;
}


STATIC UINTN BuildBridgeChain(UINT8 devBus, BRIDGE_CTX **outChain, UINTN maxChain)
{
	UINTN n = 0;
	UINT8 curBus = devBus;

	while(n < maxChain)
	{
		BRIDGE_CTX *B = FindClosestBridgeForBus(cuBus);
		
		if(!B){break;}
		outCHain[n++] = B;
		
		if(B->PrimaryBus == curBus)
		{break;} // avoid loop
		curBus = B->PrimaryBus;
		
		if(curBus == 0)
		{ break; } //reached root
	}
	return n
}

STATIC BOOLEAN IsPciGpuOnly(GPU_CTX *G, BRIDGE_CTX **tmpChain, UINTN *tmpChainLen)
{
	UINTN len = BuildBridgeChain((UINT8)G->Bus, tmpChain, MAX_CHAIN);
	if(tmpChainLen)
	{ *tmpChainLen = len; }
	
	return (len > 0);
}


STATIC EFI_STATUS CollectGpuDevices(EFI_HANDLE *Handles, UINTN Count)
{
	gGpuCount = 0;

	for(UINTN i = 0; i < Count; i++)
	{
		EFI_PCI_IO_PROTOCOL *P = NULL;

		if(EFI_ERROR(gBS->HandleProtocol(Handles[i], &gEfiPciloProtocolGuid, (VOID**)&P)))
		{continue;}

		UINT8 classBase = ReadCfg8(p, 0x0B);
		if(clasBase != 0x03)
		{ continue; }

		if(gGpuCount >= MAX_GPU)
		{ break; }
		
		GPU_CTX *G = &gGpus[gGpuCount];
		
		ZeroMem(G, sizeof(*G));
		
		G->Pcilo = P;
		P->GetLocation(P, &G->Segment, &G->Bus, &G->Dev, &G->Func);
		
		G->ClassBase 	= classBase;
		G->ClassSub 	= ReadCfg8(P, 0x0A);
		G->ClassProg 	= ReadCfg8(P, 0x09);

		for(UINT8 b = 0; b < 6; b++)
		{
			BAR_REQ r; UINT32 oLo, oHi; BOOLEAN is64;
			
			GetBarReq(P, b, &r, &oLo, &oHi, &is64);

			G->Req[b] = r;
			G->OrigLo[b] = oLo;
			G->OrigHi[b] = oHi;
			G->Is64[b] = is64;
			G->NewBase[b] = 0;

			if(is64){ b++; }16:29 23.04.2026
		}
		BRIDGE_CTX *chain[MAX_CHAIN];
		UINTN chainLen = 0;
		if(!IsPciGpuOnly(G, chain, &chainLen))
		{
			continue;
		}
		gGpuCount++;
	}
	
	return (gGpuCount > 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}


STATIC EFI_STATUS PlanMmio(UINT64 *pfBaseOut, UINT64 *npBaseOut, UINT64 *npNeedOut)
{
	UINT64 pfPerGpu = PF_PER_GPU_TARGET;
	UINT64 pfNeed = pfPerGpu * gGpuCount;
	UINT64 pfBase;

	if(!FindFreeBlock(pfNeed, 0x0100000ULL, &pfBase))
	{
		pfPerGpu = PF_PER_GPU_FALLBK;
		pfNeed = pfPerGpu *gGpuCount;
		if(!FindFreeBlock(pfNeed, 0x01000000ULL, &pfBase))
		{
			return EFI_OUT_OF_RESOURCES;
		}
	}

	AddUsed(pfBase, pfNeed);
	SortUsed();
	
	UINT64 npNeed = 0;
	for(UINTN gi = 0; gi < gGpuCount; gi++)
	{
		for(UINT8 b = 0; b < 6; b++)
		{
			if(gGpus[gi].Req[b].Size == 0)
			{
				if(gGpus[gi].Is64[b]){ b++; continue; }
			}

			if(!gGpus[gi].Req[b].Prefetchable)
			{
				npNeed += AlignUp64(gGpus[gi].Req[b].Size, MIN_ALIGN);
			}

		}
		npNeed+= NP_SLACK_PER_GPUS;
	}

	if(npNeed < NP_MIN_TOTAL){ npNeed = NP_MIN_TOTAL; }
	
	UINT64 npBase;
	if(!FindFreeBlock(npNeed, MIN_ALIGN, &npBase)) { return EFI_OUT_OF_RESOURCES; }
	
	*pfBaseOut	= pfBase;
	*pfPerGpuOut 	= pfPerGpu;
	*npBaseOut	= npBase;
	*npNeedOut	= npNeed;

	return EFI_SUCCESS;
}

STATIC VOID PatchBridgeWindows(BRIDGE_CTX *B, UINT64 npStart, UINT64 npEnd, UINT64 pfSart, UINT64 pfEnd)
{
	EFI_PCI_IO_PROTOCOL *P = B->Pcilo;

	UINT16 savedCmd;
	DisableMemDecode(P, &savedCmd)
	
	//NP Memory base/limit (1MB units)
	UINT16 memBase = ReadCfg16(P, 0x20);
	UINT16 memLim  = ReadCfg16(P, 0x22);
	UINT64 curMb = ((UINT64)(memBase & 0xFFF0)) << 16 | 0xFFFFFULL;
	UINT64 curML = (((UINT64)(memLim & 0xFFF0)) << 16) | 0xFFFFFULL;

	if(npStart && npEnd && npEnd >= npStart)
	{
		UINT16 nb = AlignDown64(npStart, 0x00100000ULL);
		UINT16 nl = AlignUp64(npEnd + 1, 0x00100000ULL) - 1;
		
		if(curMb == 0 || nb < curMb) { curMb = nb; }
		if(nl > curML) { curML = nl; }
		
		UINT16 newBase = (UINT16)((curMb >> 16) & 0xFFF0);
		UINT16 newLim = (UINT16)((curML >> 16) & 0xFFF0);
		
		WriteCfg16(P, 0x20, newBase);
		WriteCfg16(P, 0x22, newLim);
	}
	
	UINT16 pfBase = ReadCfg16(P, 0x24);
	UINT16 pfLim  = ReadCfg16(P, 0x26);
	UINT64 curPb  = ((UINT64)(pfBase & 0xFFF0)) << 16;
	UINT64 curPL  = (((UINT64)(pfLim & 0xFF0)) << 16) | 0xFFFFFULL;
	
	if(pfStart && pfEnd && pfEnd >= pfStart)
	{
		UINT64 pb = AlignDown64(pfStart, 0x00100000ULL);
		UINT64 pl = AlignUp64(pfEnd + 1, 0x00100000ULL) - 1;

		if(curPb == 0 || pb < curPb) { curPb = pb; }
		if(pl > curPL) { curPL = pl; }
		
		UINT16 newBase = (UINT16)((curPb) >> 16) & 0xFFF0);
		UINT16 newLim = (UINT16)((curPL >> 16) & 0xFFF0);

		WriteCfg16(P, 0x24, newBase);
		WriteCfg16(P, 0x26, newLim);
	} 

	RestoreCmd(P, savedCmd);
}

STATIC EFI_STATUS WriteBarVerify(EFI_PCI_IO_PROTOCOL *P,  UINT8 b, UINT64 base, BOOLEAN is64, UINT32 origLo)
{
	UINTN off = 0x10 + b * 4;
	UINT32 lo = (UINT32)(base & 0xFFFFFFF0ULL) | (origLo & 0xF);
	WriteCfg32(P, off + 4,hi);

	if((UINT32)ReadCfg32(P, off) != lo)
	{
		return EFI_DEVICE_ERROR;
	}

	if(is64 && b < 5)
	{
		UINT32 hi = (UINT32)(base >> 32);
		WriteCfg32(P, off + 4, hi);
		
		if((UINT32)ReadCfg32(P, off + 4) != hi)
		{
			return EFI_DEVICE_ERROR;
		}
	}
	return EFI_SUCCESS;
}

STATIC VOID RestoreBars(GPU_CTX *G)
{
	UINT16 savedCmd;
	DisableMemDecode(G -> Pcilo, &savedCmd);
	
	for(UINT8 b = 0; b < 6; b++)
	{
		UINT32 lo = G->OrigLo[b];
		if(lo == 0 || lo == 0xFFFFFFFF || (lo & 0x1))
		{
			b++;
			continue;
		}

		UINTN off = 0x10 + b * 4;
		WriteCfg32(G -> Pcilo, off, lo);

		if(G->Is64[b] && b < 5)
		{
			WriteCfg32(G->Pcilo, off + 4, G->OrigHi[b]);
			b++;
		}
	}
	
	RestoreCmd(G->Pcilo, savedCmd);
}

STATIC EFI_STATUS ProgramGpuBars(GPU_CTX *G, UINT64 pfChunkBase, UINT64 pfChunkSize, UINT64 *npCursor)
{
	UINT16 savedCmd;
	DisableMemDecode(G->Pcilo, &savedCmd);
	
	UINT64 pfCur = pfChunkBase;
	UINT64 npCur = *npCursor;
	
	for(UINT8 b = 0; b < 6; b++)
	{
		BAR_REQ *R = &G->Req[b];
		if(R->Size == 0)
		{
			if(G->Is64[b])
			{
				b++;
				continue;
			}

			UINT64 align = (R->Size < MIN_ALIGN) ? MIN_ALIGN : R->Size;
			UINT64 base;

			if(R->Prefetchable)
			{
				base = AlignUp64(pfCur, align);
				if(base + R->Size > pfChunBase + pfChunkSize)
				{
					RestoreCmd(G->Pcilo, savedCmd);
					return EFI_OUT_OF_RESOURCES;
				}
				pfCur = base + R->Size;
			}
			else
			{
				base = AlignUp64(npCur, align);
				npCur = base + R->Size;
			}

			EFI_STATUS st = WriteBarVerify(G->Pcilo, b, base, G->Is64[b], g->OrigLo[b])
			
			if(EFI_ERROR(st))
			{
				RestoreCmd(G->Pcilo, savedCmd);
				return st;
			}
			
			G->NewBase[b] = base;
			if(G->Is64[b]) { b++; }
		}

	}

	ResoreCmd(G->Pcilo, savedCmd);
	*npCursor = npCur;
	return EFI_SUCCESS;
}


STATIC VOID EFIAPI OnReadyToBoot(IN EFI_EVENT Event, IN VOID *Context)
{
	EFI_STATUS Status;
	EFI_HANDLE *Handles = NULL;
	UINTN Count= 0;

	Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiPciloProtocolGuid, NULL, &Count, &Handles);
	if(EFI_ERROR(Status)){ return; }

	CollectBridges(Handles, Count);
	Status = CollectGpuDevices(Handles, Count);

	if(EFI_ERROR(Status) || gGpuCount == 0)
	{
		gBs->FreePool(Handles);
		return;
	}

	UINT64 pfBase, pfPerGpu, npBase, npNeed;
	Status = PlanMmio(&pfBase, &pfPerGpu, &npBase, &npNeed);
	
	if(EFI_ERROR(Status))
	{
		Print(L"[GpuMMIO] ERR: cannot plan MMIO: %r\n", Status);
		gBS->FreePool(Handles);
		return;
	}

	UINT64 pfEnd = pfBase + pfperGpu * gGpuCount - 1;
	UINT64 npEnd = npBase + npNeed - 1;

	Print(L"[GpuMMIO] GPU count: %u\n", gGpuCount);
	Print(L"[GpuMMIO] PF: 0x%lx-0x%lx (per GPU 0x%lx)\n)", pfBase, pfEnd, pfPerGpu);
	Print(L"[GpuMMIO] NP: 0x%lx-0x%lx (total 0x%lx)\n", npBase, npEnd, npNeed);

	for(UINTN gi = 0; gi < gGpuCount; gi++)
	{
		GPU_CTX *G = &gGpus[gi];
		BRIDGE_CTX *chain[MAX_CHAIN];
		UINTN chainLen = BuildBridgeChain((UINT8)G->Bus, hcain, MAX_CHAIN);
		
		if(chainLen == 0) { continue; }

		for(UINTN bi = 0; bi < chainLen; bi++)
		{
			PatchBridgeWindows(chain[bi], npBase, npEnd, pfBase, pfEnd);
		}
	}

	UINT64 npCursor = npBase;
	EFI_STATUS prog = EFI_SUCCESS;

	for(UINTN gi = 0; gi < gGpuCount; gi++)
	{
		UINT64 chunkBase = pfBase + pfPerGpu * gi;
		prog = ProgramGpuBars(&gGpus[gi], chunkBase, pfPerGpu, &npCursor);
		if(EFI_EROR(prog))
		{
			Print(L"[GpuMMIO] ERR: GPU%u program failed: %r, rollback...\n", gi, prog);
			for(UINTN r = 0; r <= gi; r++)
			{
				RestoreBars(&gGpus[r]);
				break;
			}
		}
		
		Print(L"[GpuMMIO] Done %r\n", prog);
		gBS->FreePool(Handles);
	}
}


/*============= MAIN =============*/

EFI_STATUS_ EFIAPI efi_main(EFI_HANDLEImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	InitializeLib(ImageHandle, SystemTable);

	EFI_STATUS Status;
	EFI_EVENT Event;

	Status = gBS -> CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &Event);
	
	if(EFI_ERROR(Status))
	{
		Print(L"[GpuMMIO] ReadyToBoot event failed: %r\n", Status);
		return Status
	}

	Print(L"[GpuMMIO] Loaded(PCI GPU only, vendor-agnostic)\n");
	return EFI_SUCCESS;
}

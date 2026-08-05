// This file is a part of RCKangaroo software
// (c) 2024, RetiredCoder (RC)
// License: GPLv3, see "LICENSE.TXT" file
// https://github.com/RetiredC


#include <iostream>
#include <vector>
#include <csignal>
#include <cstdarg>

#include "cuda_runtime.h"
#include "cuda.h"

#include "defs.h"
#include "utils.h"
#include "GpuKang.h"


EcJMP EcJumps1[JMP_CNT];
EcJMP EcJumps2[JMP_CNT];
EcJMP EcJumps3[JMP_CNT];

RCGpuKang* GpuKangs[MAX_GPU_CNT];
int GpuCnt;
volatile long ThrCnt;
volatile bool gSolved;

EcInt Int_HalfRange;
EcPoint Pnt_HalfRange;
EcPoint Pnt_NegHalfRange;
EcInt x32;
EcPoint Pntx32;
Ec ec;

CriticalSection csAddPoints;
u8* pPntList;
u8* pPntList2;
volatile int PntIndex;
TFastBase db;
EcPoint gPntToSolve;
EcInt gPrivKey;

volatile u64 TotalOps;
u32 TotalSolved;
u32 gTotalErrors;
volatile u64 PntTotalOps;
bool IsBench;

u32 gDP;
u32 gRange;
EcInt gStart;
bool gStartSet;
EcPoint gPubKey;
u8 gGPUs_Mask[MAX_GPU_CNT];
char gTamesFileName[1024];
double gMax;
bool gGenMode; //tames generation mode
bool gIsOpsLimit;

// --- Checkpoint / Journal globals ---
char gSaveFileName[1024];     // -save filename (default "task.dat")
char gTaskFileName[1024];     // -taskfile filename (default "tasks.txt")
int  gTaskId;                 // -task <id> (0 = not specified)
bool gNoSave;                 // -nosave flag
int  gSaveSec;                // journal flush interval (seconds)
int  gCheckpointSec;          // full checkpoint interval (seconds)
u64  gOpsDone;                // ops_done from checkpoint restore
u64  gTaskStartTime;          // task start time (preserved across restarts)
volatile bool gExitRequested; // Ctrl+C / SIGINT flag

// Journal buffer (in-memory WAL)
u8*  gJournalBuf;             // circular buffer for journal records
volatile int gJournalBufCnt;  // number of records in buffer
u64  gLastFlushTime;          // last journal flush timestamp
u64  gLastCheckpointTime;     // last full checkpoint timestamp
CriticalSection csJournal;    // lock for journal buffer access

// ============================================================================
// Simple timestamped logger (printf-based, no external dependency)
// ============================================================================
static void LogMsg(const char* level, const char* fmt, ...)
{
	u64 t = GetTickCount64();
	u64 sec = t / 1000;
	u64 ms = t % 1000;
	printf("[%s] %llu.%03llu ", level, sec, ms);
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	printf("\r\n");
}

#pragma pack(push, 1)
struct DBRec
{
	u8 x[12];
	u8 d[22];
	u8 type; //0 - tame, 1 - wild1, 2 - wild2
};
#pragma pack(pop)

// ============================================================================
// Signal handler for graceful shutdown (Ctrl+C / SIGINT)
// ============================================================================
#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType)
{
	const char* reason = "unknown";
	switch (dwCtrlType)
	{
	case CTRL_C_EVENT:       reason = "CTRL_C"; break;
	case CTRL_BREAK_EVENT:   reason = "CTRL_BREAK"; break;
	case CTRL_CLOSE_EVENT:   reason = "CTRL_CLOSE"; break;
	case CTRL_LOGOFF_EVENT:  reason = "CTRL_LOGOFF"; break;
	case CTRL_SHUTDOWN_EVENT:reason = "CTRL_SHUTDOWN"; break;
	}
	if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT ||
		dwCtrlType == CTRL_CLOSE_EVENT || dwCtrlType == CTRL_LOGOFF_EVENT ||
		dwCtrlType == CTRL_SHUTDOWN_EVENT)
	{
		LogMsg("SIGNAL", "ConsoleCtrlHandler: %s received, setting gExitRequested=true", reason);
		gExitRequested = true;
		return TRUE; // handled
	}
	return FALSE;
}
#else
void SigIntHandler(int signum)
{
	LogMsg("SIGNAL", "SigIntHandler: signal=%d (%s), setting gExitRequested=true",
		signum, signum == SIGINT ? "SIGINT" : signum == SIGTERM ? "SIGTERM" : "?");
	gExitRequested = true;
}
#endif

// ============================================================================
// Checkpoint helper: flush journal buffer to .log file
// ============================================================================
void FlushJournal()
{
	if (gNoSave || !gSaveFileName[0]) return;
	if (gJournalBufCnt <= 0) return;

	csJournal.Enter();
	int cnt = gJournalBufCnt;
	gJournalBufCnt = 0;
	csJournal.Leave();

	if (cnt <= 0) return;

	char log_fn[1024];
	MakeJournalName(gSaveFileName, log_fn, sizeof(log_fn));
	LogMsg("JOURNAL", "FlushJournal: flushing %d records (%d bytes) to %s",
		cnt, cnt * JOURNAL_REC_LEN, log_fn);
	bool ok = AppendJournalFile(log_fn, gJournalBuf, cnt);
	if (ok)
		LogMsg("JOURNAL", "FlushJournal: OK — %d records written to %s", cnt, log_fn);
	else
		LogMsg("JOURNAL", "FlushJournal: FAILED to write %d records to %s!", cnt, log_fn);
}

// ============================================================================
// Checkpoint helper: full checkpoint (DB -> .tmp -> rename -> truncate .log)
// ============================================================================
void DoFullCheckpoint()
{
	if (gNoSave || !gSaveFileName[0]) return;

	LogMsg("CHECKPOINT", "DoFullCheckpoint: starting — DB blocks=%llu, PntTotalOps=%llu",
		db.GetBlockCnt(), PntTotalOps);

	// First flush any pending journal
	FlushJournal();

	char tmp_fn[1024], log_fn[1024];
	MakeTmpName(gSaveFileName, tmp_fn, sizeof(tmp_fn));
	MakeJournalName(gSaveFileName, log_fn, sizeof(log_fn));

	// Get start/pk hex strings for metadata
	// pubkey x/y are 256-bit = 64 hex chars each, so 65-byte buffers are safe
	char start_hex[44] = { 0 };
	char pubkey_hex[131] = { 0 }; // 64+64+null+padding
	if (gStartSet)
		gStart.GetHexStr(start_hex);
	if (!gPubKey.x.IsZero())
	{
		char sx[65] = { 0 }, sy[65] = { 0 };
		gPubKey.x.GetHexStr(sx);
		gPubKey.y.GetHexStr(sy);
		snprintf(pubkey_hex, sizeof(pubkey_hex), "%s%s", sx, sy);
	}

	int mode = gGenMode ? HEADER_MODE_GEN : HEADER_MODE_SOLVE;
	u64 cur_ops = PntTotalOps;
	u64 task_start = gTaskStartTime ? gTaskStartTime : GetTickCount64();

	double db_gb = (double)db.GetBlockCnt() * 32 / (1024 * 1024 * 1024);
	LogMsg("CHECKPOINT", "DoFullCheckpoint: writing DB (%.1f GB) to tmp=%s, mode=%d, range=%d, dp=%d",
		db_gb, tmp_fn, mode, gRange, gDP);

	if (db.SaveToFileEx(tmp_fn, gRange, gDP, mode, start_hex, pubkey_hex, cur_ops, task_start))
	{
		LogMsg("CHECKPOINT", "DoFullCheckpoint: SaveToFileEx OK (%.1f GB), renaming %s -> %s",
			db_gb, tmp_fn, gSaveFileName);
		// Atomic rename
		if (rename(tmp_fn, gSaveFileName) == 0)
		{
			// Truncate journal
			FILE* fp = fopen(log_fn, "wb");
			if (fp) { fclose(fp); LogMsg("CHECKPOINT", "DoFullCheckpoint: journal %s truncated", log_fn); }
			else      LogMsg("CHECKPOINT", "DoFullCheckpoint: WARNING — cannot truncate journal %s", log_fn);
			gLastCheckpointTime = GetTickCount64();
			LogMsg("CHECKPOINT", "DoFullCheckpoint: COMPLETE — %llu DPs saved to %s, next checkpoint in %ds",
				db.GetBlockCnt(), gSaveFileName, gCheckpointSec);
		}
		else
		{
			LogMsg("CHECKPOINT", "DoFullCheckpoint: FAILED — rename(%s, %s) error, tmp file left on disk",
				tmp_fn, gSaveFileName);
		}
	}
	else
	{
		LogMsg("CHECKPOINT", "DoFullCheckpoint: FAILED — SaveToFileEx(%s) returned false", tmp_fn);
	}
}

void InitGpus()
{
	GpuCnt = 0;
	int gcnt = 0;
	cudaGetDeviceCount(&gcnt);
	if (gcnt > MAX_GPU_CNT)
		gcnt = MAX_GPU_CNT;

//	gcnt = 1; //dbg
	if (!gcnt)
		return;

	int drv, rt;
	cudaRuntimeGetVersion(&rt);
	cudaDriverGetVersion(&drv);
	char drvver[100];
	sprintf(drvver, "%d.%d/%d.%d", drv / 1000, (drv % 100) / 10, rt / 1000, (rt % 100) / 10);

	printf("CUDA devices: %d, CUDA driver/runtime: %s\r\n", gcnt, drvver);
	cudaError_t cudaStatus;
	for (int i = 0; i < gcnt; i++)
	{
		cudaStatus = cudaSetDevice(i);
		if (cudaStatus != cudaSuccess)
		{
			printf("cudaSetDevice for gpu %d failed!\r\n", i);
			continue;
		}

		if (!gGPUs_Mask[i])
			continue;

		cudaDeviceProp deviceProp;
		cudaGetDeviceProperties(&deviceProp, i);
		printf("GPU %d: %s, %.2f GB, %d CUs, cap %d.%d, PCI %d, L2 size: %d KB\r\n", i, deviceProp.name, ((float)(deviceProp.totalGlobalMem / (1024 * 1024))) / 1024.0f, deviceProp.multiProcessorCount, deviceProp.major, deviceProp.minor, deviceProp.pciBusID, deviceProp.l2CacheSize / 1024);
		int cm = deviceProp.major * 10 + deviceProp.minor;

		if (deviceProp.major < 6)
		{
			printf("GPU %d - not supported, skip\r\n", i);
			continue;
		}

		cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);

		GpuKangs[GpuCnt] = new RCGpuKang();
		GpuKangs[GpuCnt]->CudaIndex = i;
		GpuKangs[GpuCnt]->persistingL2CacheMaxSize = deviceProp.persistingL2CacheMaxSize;
		GpuKangs[GpuCnt]->mpCnt = deviceProp.multiProcessorCount;
		GpuKangs[GpuCnt]->JumperInd = GpuCnt;

		if ((cm != 89) && (cm != 120))
		{
			GpuKangs[GpuCnt]->sm_inv_cnt = 0;
			printf("GPU %d: use 3.x version of RCKangaroo to get better performance!\r\n", i);
		}
		else
		{
			GpuKangs[GpuCnt]->Is5xxx = (deviceProp.major == 12);
			GpuKangs[GpuCnt]->sm_inv_cnt = GpuKangs[GpuCnt]->Is5xxx ? (GpuKangs[GpuCnt]->mpCnt / 24) : (GpuKangs[GpuCnt]->mpCnt / 32);
			if (!GpuKangs[GpuCnt]->sm_inv_cnt)
				GpuKangs[GpuCnt]->sm_inv_cnt = 1;
			printf("GPU %d: turbo kernel is enabled!\r\n", i);
		}
		GpuCnt++;
	}
	printf("Total GPUs for work: %d\r\n", GpuCnt);
}
#ifdef _WIN32
u32 __stdcall kang_thr_proc(void* data)
{
	RCGpuKang* Kang = (RCGpuKang*)data;
	Kang->Execute();
	InterlockedDecrement(&ThrCnt);
	return 0;
}
#else
void* kang_thr_proc(void* data)
{
	RCGpuKang* Kang = (RCGpuKang*)data;
	Kang->Execute();
	__sync_fetch_and_sub(&ThrCnt, 1);
	return 0;
}
#endif
void AddPointsToList(u32* data, int pnt_cnt, u32 KangCnt, u64 ops_cnt, int JumperInd)
{
	for (int i = 0; i < pnt_cnt; i++) //convert KangInd to KangType
	{
		u32* p = data + (GPU_DP_SIZE / 4) * i;
		int KangInd = p[10];
		p[10] = (KangInd < KangCnt / 3) ? TAME : WILD;

		//optional: restart kang after DP
		//GpuKangs[JumperInd]->ToRestartKangaroo(KangInd);
	}
	csAddPoints.Enter();
	if (PntIndex + pnt_cnt >= MAX_CNT_LIST)
	{
		csAddPoints.Leave();
		printf("DPs buffer overflow, some points lost, increase DP value!\r\n");
		return;
	}
	memcpy(pPntList + GPU_DP_SIZE * PntIndex, data, pnt_cnt * GPU_DP_SIZE);
	PntIndex += pnt_cnt;
	PntTotalOps += ops_cnt;
	csAddPoints.Leave();
}

bool Collision_SOTA(EcPoint& pnt, EcInt t, int TameType, EcInt w, int WildType, bool IsNeg)
{
	if (IsNeg)
		t.Neg();
	if (TameType == TAME)
	{
		gPrivKey = t;
		gPrivKey.Sub(w);
		EcInt sv = gPrivKey;
		EcPoint P = ec.MultiplyG(gPrivKey);
		if (P.IsEqual(pnt))
			return true;
		gPrivKey = sv;
		gPrivKey.Neg();
		P = ec.MultiplyG(gPrivKey);
		return P.IsEqual(pnt);
	}
	else
	{
		gPrivKey = t;
		gPrivKey.Sub(w);
		if (gPrivKey.data[4] >> 63)
			gPrivKey.Neg();
		gPrivKey.ShiftRight(1);
		EcInt sv = gPrivKey;
		EcPoint P = ec.MultiplyG(gPrivKey);
		if (P.IsEqual(pnt))
			return true;
		gPrivKey = sv;
		gPrivKey.Neg();
		P = ec.MultiplyG(gPrivKey);
		return P.IsEqual(pnt);
	}
}

void CheckNewPoints()
{
	csAddPoints.Enter();
	if (!PntIndex)
	{
		csAddPoints.Leave();
		return;
	}

	int cnt = PntIndex;
	memcpy(pPntList2, pPntList, GPU_DP_SIZE * cnt);
	PntIndex = 0;
	csAddPoints.Leave();

	for (int i = 0; i < cnt; i++)
		{
			DBRec nrec;
			u8* p = pPntList2 + i * GPU_DP_SIZE;
			memcpy(nrec.x, p, 12);
			memcpy(nrec.d, p + 16, 22);
			nrec.type = gGenMode ? TAME : p[40];

			DBRec* pref = (DBRec*)db.FindOrAddDataBlock((u8*)&nrec);
			if (gGenMode)
				continue;

			// Journal: if new DP was actually added (pref == NULL), append to journal buffer
				if (!pref && !gNoSave && gSaveFileName[0])
				{
					csJournal.Enter();
					int buf_idx = gJournalBufCnt;
					if (buf_idx < JOURNAL_BUF_SIZE / JOURNAL_REC_LEN)
					{
						memcpy(gJournalBuf + buf_idx * JOURNAL_REC_LEN, &nrec, JOURNAL_REC_LEN);
						gJournalBufCnt = buf_idx + 1;
					}
					csJournal.Leave();
					// If buffer is full, force flush (should not happen under normal operation)
					if (buf_idx >= JOURNAL_BUF_SIZE / JOURNAL_REC_LEN - 1)
					{
						LogMsg("JOURNAL", "CheckNewPoints: buffer FULL (%d/%d records), force flush",
							buf_idx + 1, (int)(JOURNAL_BUF_SIZE / JOURNAL_REC_LEN));
						FlushJournal();
					}
				}

			if (pref)
		{
			//in db we dont store first 3 bytes so restore them
			DBRec tmp_pref;
			memcpy(&tmp_pref, &nrec, 3);
			memcpy(((u8*)&tmp_pref) + 3, pref, sizeof(DBRec) - 3);
			pref = &tmp_pref;

			if (pref->type == nrec.type)
			{
				if (pref->type == TAME)
					continue;

				//if it's wild, we can find the key from the same type if distances are different
				if (*(u64*)pref->d == *(u64*)nrec.d)
					continue;
				//else
				//	ToLog("key found by same wild");
			}

			EcInt w, t;
			int TameType, WildType;
			if (pref->type != TAME)
			{
				memcpy(w.data, pref->d, sizeof(pref->d));
				if (pref->d[21] == 0xFF) memset(((u8*)w.data) + 22, 0xFF, 18);
				memcpy(t.data, nrec.d, sizeof(nrec.d));
				if (nrec.d[21] == 0xFF) memset(((u8*)t.data) + 22, 0xFF, 18);
				TameType = nrec.type;
				WildType = pref->type;
			}
			else
			{
				memcpy(w.data, nrec.d, sizeof(nrec.d));
				if (nrec.d[21] == 0xFF) memset(((u8*)w.data) + 22, 0xFF, 18);
				memcpy(t.data, pref->d, sizeof(pref->d));
				if (pref->d[21] == 0xFF) memset(((u8*)t.data) + 22, 0xFF, 18);
				TameType = TAME;
				WildType = nrec.type;
			}

			bool res = Collision_SOTA(gPntToSolve, t, TameType, w, WildType, false) || Collision_SOTA(gPntToSolve, t, TameType, w, WildType, true);
			if (!res)
			{
				printf("Collision Error\r\n");
				gTotalErrors++;
				continue;
			}
			gSolved = true;
			break;
		}
	}
}

void ShowStats(u64 tm_start, double exp_ops, double dp_val)
{
#ifdef DEBUG_MODE
	for (int i = 0; i <= MD_LEN; i++)
	{
		u64 val = 0;
		for (int j = 0; j < GpuCnt; j++)
		{
			val += GpuKangs[j]->dbg[i];
		}
		if (val)
			printf("Loop size %d: %llu\r\n", i, val);
	}
#endif

	int speed = GpuKangs[0]->GetStatsSpeed();
	for (int i = 1; i < GpuCnt; i++)
		speed += GpuKangs[i]->GetStatsSpeed();

	u64 est_dps_cnt = (u64)(exp_ops / dp_val);
	u64 cur_dps = db.GetBlockCnt();

	// Progress percentage (ops-based)
	double progress = 0.0;
	if (exp_ops > 0)
		progress = (double)100.0 * (double)PntTotalOps / exp_ops;
	if (progress > 100.0) progress = 100.0;

	// Real-time K
	double K_val = (double)PntTotalOps / pow(2.0, gRange / 2.0);

	// Elapsed time (cross-restart continuous)
	u64 elapsed_ms = GetTickCount64() - gTaskStartTime;
	u64 elapsed_sec = gTaskStartTime ? (elapsed_ms / 1000) : ((GetTickCount64() - tm_start) / 1000);
	int elap_d = (int)(elapsed_sec / 86400);
	int elap_h = (int)((elapsed_sec % 86400) / 3600);
	int elap_m = (int)((elapsed_sec % 3600) / 60);

	// ETA (subtract already completed ops)
	u64 eta_sec = 0xFFFFFFFFFFFFFFFFull;
	if (speed > 0 && PntTotalOps < (u64)exp_ops)
		eta_sec = (u64)((exp_ops - (double)PntTotalOps) / 1000000.0 / (double)speed);
	int eta_d = (int)(eta_sec / 86400);
	int eta_h = (int)((eta_sec % 86400) / 3600);
	int eta_m = (int)((eta_sec % 3600) / 60);

	// Build progress bar (20 chars)
	char bar[23] = "[                    ]";
	int bar_fill = (int)(progress / 5.0);
	if (bar_fill > 20) bar_fill = 20;
	for (int b = 0; b < bar_fill; b++) bar[b + 1] = '#';

	const char* prefix = gGenMode ? "GEN: " : (IsBench ? "BENCH: " : "MAIN: ");

	if (eta_sec == 0xFFFFFFFFFFFFFFFFull)
		printf("%s%s %.1f%% | Speed: %d MKeys/s | K: %.2f | DPs: %lluK/%lluK | Elapsed: %dd:%02dh:%02dm | ETA: --\r\n",
			prefix, bar, progress, speed, K_val, cur_dps / 1000, est_dps_cnt / 1000,
			elap_d, elap_h, elap_m);
	else
		printf("%s%s %.1f%% | Speed: %d MKeys/s | K: %.2f | DPs: %lluK/%lluK | Elapsed: %dd:%02dh:%02dm | ETA: %dd:%02dh:%02dm\r\n",
			prefix, bar, progress, speed, K_val, cur_dps / 1000, est_dps_cnt / 1000,
			elap_d, elap_h, elap_m, eta_d, eta_h, eta_m);
}

bool SolvePoint(EcPoint PntToSolve, int Range, int DP, EcInt* pk_res)
{
	if ((Range < 32) || (Range > 180))
	{
		printf("Unsupported Range value (%d)!\r\n", Range);
		return false;
	}
	if ((DP < 14) || (DP > 32)) 
	{
		printf("Unsupported DP value (%d)!\r\n", DP);
		return false;
	}

	printf("\r\nSolving point: Range %d bits, DP %d, start...\r\n", Range, DP);
	double ops = 1.15 * pow(2.0, Range / 2.0);
	double dp_val = (double)(1ull << DP);
	u64 total_kangs = GpuKangs[0]->CalcKangCnt();
	for (int i = 1; i < GpuCnt; i++)
		total_kangs += GpuKangs[i]->CalcKangCnt();
	double path_single_kang = ops / total_kangs;
	double DPs_per_kang = path_single_kang / dp_val;
	printf("Estimated DPs per kangaroo (ideal): %.2f.%s\r\n", DPs_per_kang, (DPs_per_kang < 5) ? " DP overhead is big, use less DP value if possible!" : "");

	if (DPs_per_kang < 0.001)
		DPs_per_kang = 0.001;
	double K = 1.15 + (0.07 + 0.76 / sqrt(DPs_per_kang)) / (1 + 0.30 * DPs_per_kang); //real K with DP overhead. Empirical formula.
	printf("Estimated K with DP overhead: %.2f (DP overhead is about %d%%)\r\n", K, int(0.5 + 100 * (K / 1.15 - 1.0)) );
	ops = K * pow(2.0, Range / 2.0);

	double ram = (32 + 4 + 4) * ops / dp_val; //+4 for grow allocation and memory fragmentation
	ram += sizeof(TListRec) * 256 * 256 * 256; //3byte-prefix table
	ram /= (1024 * 1024 * 1024); //GB
	printf("SOTA v2 method, estimated ops: 2^%.3f, RAM for DPs: %.3f GB.\r\n", log2(ops), ram);
	gIsOpsLimit = false;
	double MaxTotalOps = 0.0;
	if (gMax > 0)
	{
		MaxTotalOps = gMax * ops;
		double ram_max = (32 + 4 + 4) * MaxTotalOps / dp_val; //+4 for grow allocation and memory fragmentation
		ram_max += sizeof(TListRec) * 256 * 256 * 256; //3byte-prefix table
		ram_max /= (1024 * 1024 * 1024); //GB
		printf("Max allowed number of ops: 2^%.3f, max RAM for DPs: %.3f GB\r\n", log2(MaxTotalOps), ram_max);
	}

	// ======================================================================
	// Checkpoint recovery: load .dat + replay .log
	// ======================================================================
	bool checkpoint_loaded = false;
	if (!gNoSave && gSaveFileName[0] && IsFileExist(gSaveFileName))
	{
		LogMsg("RECOVER", "Checkpoint file found: %s, attempting load...", gSaveFileName);
		if (db.LoadFromFile(gSaveFileName))
		{
			LogMsg("RECOVER", "LoadFromFile OK: %s loaded, %llu DPs, header[0]=%d, format_ver=%d",
				gSaveFileName, db.GetBlockCnt(), db.Header[HDR_OFF_RANGE], db.Header[HDR_OFF_FORMAT_VER]);

			// Get hex strings for validation
			char start_hex[44] = { 0 };
			char pubkey_hex[131] = { 0 }; // 64+64+null+padding
			if (gStartSet)
				gStart.GetHexStr(start_hex);
			if (!gPubKey.x.IsZero())
			{
				char sx[65] = { 0 }, sy[65] = { 0 };
				gPubKey.x.GetHexStr(sx);
				gPubKey.y.GetHexStr(sy);
				snprintf(pubkey_hex, sizeof(pubkey_hex), "%s%s", sx, sy);
			}

			int mode = gGenMode ? HEADER_MODE_GEN : HEADER_MODE_SOLVE;
			LogMsg("RECOVER", "Validating metadata: range=%d, dp=%d, mode=%d...", Range, DP, mode);
			if (ValidateMeta(db.Header, Range, DP, mode, start_hex, pubkey_hex))
			{
				LogMsg("RECOVER", "ValidateMeta OK — metadata matches current task");

				// Replay journal
				char log_fn[1024];
				MakeJournalName(gSaveFileName, log_fn, sizeof(log_fn));
				LogMsg("RECOVER", "Replaying journal from %s...", log_fn);
				ReplayJournalFile(log_fn, db);

				// Restore state
				PntTotalOps = *(u64*)(db.Header + HDR_OFF_OPS_DONE);
				gOpsDone = PntTotalOps;
				u64 saved_task_start = *(u64*)(db.Header + HDR_OFF_TASK_START);
				gTaskStartTime = saved_task_start ? saved_task_start : GetTickCount64();

				LogMsg("RECOVER", "State restored: ops_done=%llu, PntTotalOps=%llu, task_start_time=%llu",
					gOpsDone, PntTotalOps, gTaskStartTime);

				u64 cur_dps = db.GetBlockCnt();
				double resume_progress = 0.0;
				if (ops > 0)
					resume_progress = 100.0 * (double)PntTotalOps / ops;
				if (resume_progress > 100.0) resume_progress = 100.0;

				LogMsg("RECOVER", "Resumed: %llu DPs (%.1f%% progress), ops_done=%llu, elapsed=%llus",
					cur_dps, resume_progress, PntTotalOps,
					gTaskStartTime ? (GetTickCount64() - gTaskStartTime) / 1000 : 0);

				checkpoint_loaded = true;
				// Skip tames loading if we already have data
				gTamesFileName[0] = 0;
			}
			else
			{
				LogMsg("RECOVER", "ValidateMeta FAILED — metadata mismatch, starting fresh");
				db.Clear();
			}
		}
		else
		{
			LogMsg("RECOVER", "LoadFromFile FAILED for %s, starting fresh", gSaveFileName);
		}
	}
	else if (!gNoSave && gSaveFileName[0])
	{
		LogMsg("RECOVER", "No checkpoint file %s found, starting fresh", gSaveFileName);
	}

	// Allocate journal buffer
	if (!gNoSave && gSaveFileName[0])
	{
		gJournalBuf = (u8*)malloc(JOURNAL_BUF_SIZE);
		if (gJournalBuf)
			LogMsg("JOURNAL", "Buffer allocated: %d bytes (%d records max)",
				JOURNAL_BUF_SIZE, (int)(JOURNAL_BUF_SIZE / JOURNAL_REC_LEN));
		else
			LogMsg("JOURNAL", "FATAL: failed to allocate journal buffer (%d bytes)!", JOURNAL_BUF_SIZE);
		gJournalBufCnt = 0;
		gLastFlushTime = GetTickCount64();
		gLastCheckpointTime = GetTickCount64();
	}

	if (!gGenMode && gTamesFileName[0] && !checkpoint_loaded)
	{
		printf("load tames...\r\n");
		if (db.LoadFromFile(gTamesFileName))
		{
			printf("tames loaded\r\n");
			if (db.Header[0] != gRange)
			{
				printf("loaded tames have different range, they cannot be used, clear\r\n");
				db.Clear();
			}
		}
		else
			printf("tames loading failed\r\n");
	}

	SetRndSeed(0); //use same seed to make tames from file compatible
	PntTotalOps = 0;
	PntIndex = 0;
//prepare jumps
	EcInt minjump, t;
	minjump.Set(1);
	minjump.ShiftLeft(Range / 2 + 3);
	for (int i = 0; i < JMP_CNT; i++)
	{
		EcJumps1[i].dist = minjump;
		t.RndMax(minjump);
		EcJumps1[i].dist.Add(t);
		EcJumps1[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		EcJumps1[i].p = ec.MultiplyG(EcJumps1[i].dist);
	}

	minjump.Set(1);
	minjump.ShiftLeft(Range - 10); //large jumps for L1S2 loops. Must be almost RANGE_BITS
	for (int i = 0; i < JMP_CNT; i++)
	{
		EcJumps2[i].dist = minjump;
		t.RndMax(minjump);
		EcJumps2[i].dist.Add(t);
		EcJumps2[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		EcJumps2[i].p = ec.MultiplyG(EcJumps2[i].dist);
	}

	minjump.Set(1);
	minjump.ShiftLeft(Range - 10 - 2); //large jumps for loops >2
	for (int i = 0; i < JMP_CNT; i++)
	{
		EcJumps3[i].dist = minjump;
		t.RndMax(minjump);
		EcJumps3[i].dist.Add(t);
		EcJumps3[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		EcJumps3[i].p = ec.MultiplyG(EcJumps3[i].dist);
	}
	SetRndSeed(GetTickCount64());

	Int_HalfRange.Set(1);
	Int_HalfRange.ShiftLeft(Range - 1);
	Pnt_HalfRange = ec.MultiplyG(Int_HalfRange);
	Pnt_NegHalfRange = Pnt_HalfRange;
	Pnt_NegHalfRange.y.NegModP();
	gPntToSolve = PntToSolve;

//prepare GPUs
	for (int i = 0; i < GpuCnt; i++)
		if (!GpuKangs[i]->Prepare(PntToSolve, Range, DP, EcJumps1, EcJumps2, EcJumps3))
		{
			GpuKangs[i]->Failed = true;
			printf("GPU %d Prepare failed\r\n", GpuKangs[i]->CudaIndex);
		}

	u64 tm0 = GetTickCount64();
	printf("GPUs started...\r\n");

#ifdef _WIN32
	HANDLE thr_handles[MAX_GPU_CNT];
#else
	pthread_t thr_handles[MAX_GPU_CNT];
#endif

	u32 ThreadID = 0;
	gSolved = false;
	ThrCnt = GpuCnt;
	for (int i = 0; i < GpuCnt; i++)
	{
#ifdef _WIN32
		thr_handles[i] = (HANDLE)_beginthreadex(NULL, 0, kang_thr_proc, (void*)GpuKangs[i], 0, &ThreadID);
#else
		pthread_create(&thr_handles[i], NULL, kang_thr_proc, (void*)GpuKangs[i]);
#endif
	}

	u64 tm_stats = GetTickCount64();
		while (!gSolved)
		{
			CheckNewPoints();
			Sleep(10);

			// Check for Ctrl+C / exit request
			if (gExitRequested)
			{
				LogMsg("MAIN", "gExitRequested=true detected in main loop, breaking out");
				break;
			}

			if (GetTickCount64() - tm_stats > 10 * 1000)
			{
				ShowStats(tm0, ops, dp_val);
				tm_stats = GetTickCount64();
			}

			// Periodic journal flush
			if (!gNoSave && gSaveFileName[0] && gJournalBufCnt > 0)
			{
				u64 now = GetTickCount64();
				u64 journal_size = (u64)gJournalBufCnt * JOURNAL_REC_LEN;
				u64 since_flush = (now - gLastFlushTime) / 1000;
				u64 since_checkpoint = (now - gLastCheckpointTime) / 1000;

				// Flush every gSaveSec seconds, or if journal buffer too large
				if ((now - gLastFlushTime > (u64)gSaveSec * 1000) || (journal_size > JOURNAL_BUF_SIZE / 2))
				{
					LogMsg("MAIN", "Flush timer: since_flush=%llus (threshold=%ds), buf_size=%llu/%d, flushing",
						since_flush, gSaveSec, journal_size, JOURNAL_BUF_SIZE);
					FlushJournal();
					gLastFlushTime = now;

					// Full checkpoint every gCheckpointSec seconds, or if journal file too large
					if ((now - gLastCheckpointTime > (u64)gCheckpointSec * 1000) || (journal_size > JOURNAL_MAX_SIZE))
					{
						LogMsg("MAIN", "Checkpoint timer: since_checkpoint=%llus (threshold=%ds), triggering full checkpoint",
							since_checkpoint, gCheckpointSec);
						DoFullCheckpoint();
					}
				}
			}

			if ((MaxTotalOps > 0.0) && ((PntTotalOps - gOpsDone) > (u64)MaxTotalOps))
			{
				gIsOpsLimit = true;
				printf("Operations limit reached\r\n");
				break;
			}
		}

		printf("Stopping work ...\r\n");
		for (int i = 0; i < GpuCnt; i++)
			GpuKangs[i]->Stop();
		while (ThrCnt)
			Sleep(10);
		for (int i = 0; i < GpuCnt; i++)
		{
#ifdef _WIN32
			CloseHandle(thr_handles[i]);
#else
			pthread_join(thr_handles[i], NULL);
#endif
		}

		// Graceful shutdown: flush journal + full checkpoint
		if (gExitRequested)
		{
			LogMsg("EXIT", "Graceful shutdown: gExitRequested=true, saving final checkpoint...");
			if (gGenMode && gTamesFileName[0])
			{
				printf("saving tames before exit...\r\n");
				db.Header[0] = gRange;
				if (db.SaveToFile(gTamesFileName))
					printf("tames saved to %s\r\n", gTamesFileName);
				else
					printf("tames saving failed\r\n");
			}
			DoFullCheckpoint();
			// Free journal buffer
			if (gJournalBuf) { free(gJournalBuf); LogMsg("EXIT", "Journal buffer freed"); gJournalBuf = NULL; }
			db.Clear();
			LogMsg("EXIT", "Graceful shutdown complete, returning false");
			return false;
		}

		if (gIsOpsLimit)
		{
			LogMsg("EXIT", "Ops limit reached: PntTotalOps=%llu, gOpsDone=%llu, MaxTotalOps=%.0f",
				PntTotalOps, gOpsDone, MaxTotalOps);
			if (gGenMode)
			{
				printf("saving tames...\r\n");
				db.Header[0] = gRange;
				if (db.SaveToFile(gTamesFileName))
					printf("tames saved\r\n");
				else
					printf("tames saving failed\r\n");
			}
			// Save checkpoint before clearing
			DoFullCheckpoint();
			if (gJournalBuf) { free(gJournalBuf); LogMsg("EXIT", "Journal buffer freed (ops limit)"); gJournalBuf = NULL; }
			db.Clear();
			return false;
		}

		K = (double)PntTotalOps / pow(2.0, Range / 2.0);
		printf("Point solved, K: %.3f (with DP and GPU overheads)\r\n\r\n", K);

		// Success! Delete checkpoint files (task completed)
		if (!gNoSave && gSaveFileName[0])
		{
			char log_fn[1024], tmp_fn[1024];
			MakeJournalName(gSaveFileName, log_fn, sizeof(log_fn));
			MakeTmpName(gSaveFileName, tmp_fn, sizeof(tmp_fn));
			LogMsg("EXIT", "Task solved! Cleaning checkpoint files: %s, %s, %s",
				gSaveFileName, log_fn, tmp_fn);
			int r1 = remove(gSaveFileName);
			int r2 = remove(log_fn);
			int r3 = remove(tmp_fn);
			LogMsg("EXIT", "Checkpoint cleanup: .dat=%s, .log=%s, .tmp=%s",
				r1 == 0 ? "deleted" : "not found/skip",
				r2 == 0 ? "deleted" : "not found/skip",
				r3 == 0 ? "deleted" : "not found/skip");
		}
		if (gJournalBuf) { free(gJournalBuf); LogMsg("EXIT", "Journal buffer freed (solved)"); gJournalBuf = NULL; }
		db.Clear();
		*pk_res = gPrivKey;
		return true;
}

// ============================================================================
// Help / usage text
// ============================================================================
void ShowHelp()
{
	printf("RCKangaroo v4.0 — GPU-accelerated ECDLP solver (Pollard's kangaroo, SOTA v2)\r\n");
	printf("https://github.com/RetiredC\r\n\r\n");
	printf("USAGE:\r\n");
	printf("  RCKangaroo.exe [options]\r\n\r\n");
	printf("QUICK START (using task mapping):\r\n");
	printf("  RCKangaroo.exe -task <id>               Solve task by id from tasks.txt\r\n");
	printf("  RCKangaroo.exe -task <id> -tames X.dat   With pre-generated tame points\r\n\r\n");
	printf("MANUAL MODE:\r\n");
	printf("  RCKangaroo.exe -range <N> -start <hex> -pubkey <hex>\r\n");
	printf("  RCKangaroo.exe -range <N> -start <hex> -pubkey <hex> -tames X.dat\r\n\r\n");
	printf("TAMES GENERATION:\r\n");
	printf("  RCKangaroo.exe -range <N> -tames X.dat -max <N>\r\n\r\n");
	printf("OPTIONS:\r\n");
	printf("  -range <32..170>      Bit range of the private key (required)\r\n");
	printf("  -start <hex>           Start offset (64-char hex, required)\r\n");
	printf("  -pubkey <hex>          Public key (130-char hex with 04 prefix)\r\n");
	printf("  -dp <14..60>           Distinguished point bits (default: %d)\r\n", DEFAULT_DP);
	printf("  -tames <file>          Tame points file (load or generate)\r\n");
	printf("  -max <N>               Max ops multiplier (e.g. 10 = 10x expected)\r\n");
	printf("  -gpu <digits>          GPU mask, e.g. -gpu 0 or -gpu 01 (default: all)\r\n\r\n");
	printf("TASK MAPPING:\r\n");
	printf("  -task <id>             Task id from tasks.txt (default: %s)\r\n", DEFAULT_TASK_FILE);
	printf("  -taskfile <path>       Custom task mapping file path\r\n\r\n");
	printf("CHECKPOINT (auto-save & resume):\r\n");
	printf("  -save <file>           Save file name (default: %s, auto-enabled)\r\n", DEFAULT_SAVE_FILE);
	printf("  -nosave                Disable checkpoint/resume\r\n");
	printf("  -save_sec <N>          Journal flush interval in seconds (default: %d)\r\n", DEFAULT_SAVE_SEC);
	printf("  -checkpoint_sec <N>    Full checkpoint interval in seconds (default: %d)\r\n\r\n", DEFAULT_CHECKPOINT_SEC);
	printf("EXAMPLES:\r\n");
	printf("  # Solve puzzle #1 from tasks.txt with auto-save\r\n");
	printf("  RCKangaroo.exe -task 1\r\n\r\n");
	printf("  # Solve puzzle #1 with custom save file and tame points\r\n");
	printf("  RCKangaroo.exe -task 1 -save mytask.dat -tames tames84.dat\r\n\r\n");
	printf("  # Solve manually (no task mapping)\r\n");
	printf("  RCKangaroo.exe -range 84 -start 0000...0001 -pubkey 04ABCD...\r\n\r\n");
	printf("  # Generate tame points\r\n");
	printf("  RCKangaroo.exe -range 84 -tames tames84.dat -max 10\r\n\r\n");
	printf("  # Resume interrupted task\r\n");
	printf("  RCKangaroo.exe -task 1\r\n\r\n");
	printf("  # Benchmark (no pubkey = benchmark mode)\r\n");
	printf("  RCKangaroo.exe -range 84 -dp 16\r\n\r\n");
	printf("TASK FILE FORMAT (tasks.txt):\r\n");
	printf("  # <id> <range> <start_hex> <pubkey_hex>\r\n");
	printf("  1  84  0000...0001  04ABCD...\r\n");
	printf("  2  88  0000...0002  04EF01...\r\n");
}

bool ParseCommandLine(int argc, char* argv[])
{
	int ci = 1;
	while (ci < argc)
	{
		char* argument = argv[ci];
		ci++;
		if (strcmp(argument, "-help") == 0 || strcmp(argument, "-h") == 0 ||
			strcmp(argument, "--help") == 0 || strcmp(argument, "-?") == 0)
		{
			ShowHelp();
			return false;
		}
		else
		if (strcmp(argument, "-gpu") == 0)
		{
			if (ci >= argc)
			{
				printf("error: missed value after -gpu option\r\n");
				return false;
			}
			char* gpus = argv[ci];
			ci++;
			memset(gGPUs_Mask, 0, sizeof(gGPUs_Mask));
			for (int i = 0; i < (int)strlen(gpus); i++)
			{
				if ((gpus[i] < '0') || (gpus[i] > '9'))
				{
					printf("error: invalid value for -gpu option\r\n");
					return false;
				}
				gGPUs_Mask[gpus[i] - '0'] = 1;
			}
		}
		else
		if (strcmp(argument, "-dp") == 0)
		{
			int val = atoi(argv[ci]);
			ci++;
			if ((val < 14) || (val > 60))
			{
				printf("error: invalid value for -dp option\r\n");
				return false;
			}
			gDP = val;
		}
		else
		if (strcmp(argument, "-range") == 0)
		{
			int val = atoi(argv[ci]);
			ci++;
			if ((val < 32) || (val > 170))
			{
				printf("error: invalid value for -range option\r\n");
				return false;
			}
			gRange = val;
		}
		else
		if (strcmp(argument, "-start") == 0)
		{	
			if (!gStart.SetHexStr(argv[ci]))
			{
				printf("error: invalid value for -start option\r\n");
				return false;
			}
			ci++;
			gStartSet = true;
		}
		else
		if (strcmp(argument, "-pubkey") == 0)
		{
			if (!gPubKey.SetHexStr(argv[ci]))
			{
				printf("error: invalid value for -pubkey option\r\n");
				return false;
			}
			ci++;
		}
		else
		if (strcmp(argument, "-tames") == 0)
		{
			strcpy(gTamesFileName, argv[ci]);
			ci++;
		}
		else
		if (strcmp(argument, "-max") == 0)
		{
			double val = atof(argv[ci]);
			ci++;
			if (val < 0.001)
			{
				printf("error: invalid value for -max option\r\n");
				return false;
			}
			gMax = val;
		}
		else
		if (strcmp(argument, "-save") == 0)
		{
			strcpy(gSaveFileName, argv[ci]);
			ci++;
			gNoSave = false;
		}
		else
		if (strcmp(argument, "-nosave") == 0)
		{
			gNoSave = true;
			gSaveFileName[0] = 0;
		}
		else
		if (strcmp(argument, "-task") == 0)
		{
			gTaskId = atoi(argv[ci]);
			ci++;
			if (gTaskId <= 0)
			{
				printf("error: invalid value for -task option\r\n");
				return false;
			}
		}
		else
		if (strcmp(argument, "-taskfile") == 0)
		{
			strcpy(gTaskFileName, argv[ci]);
			ci++;
		}
		else
		if (strcmp(argument, "-save_sec") == 0)
		{
			gSaveSec = atoi(argv[ci]);
			ci++;
			if (gSaveSec < 1)
			{
				printf("error: invalid value for -save_sec option\r\n");
				return false;
			}
		}
		else
		if (strcmp(argument, "-checkpoint_sec") == 0)
		{
			gCheckpointSec = atoi(argv[ci]);
			ci++;
			if (gCheckpointSec < 1)
			{
				printf("error: invalid value for -checkpoint_sec option\r\n");
				return false;
			}
		}
		else
		{
			printf("error: unknown option %s\r\n", argument);
			return false;
		}
	}

	// Resolve task mapping if -task specified
	if (gTaskId > 0)
	{
		const char* task_fn = gTaskFileName[0] ? gTaskFileName : DEFAULT_TASK_FILE;
		LogMsg("PARSE", "Resolving task #%d from %s...", gTaskId, task_fn);
		std::map<int, TaskMeta> tasks = LoadTaskMapping(task_fn);

		if (tasks.empty())
		{
			LogMsg("PARSE", "ERROR: no tasks loaded from %s", task_fn);
			printf("error: no tasks loaded from %s\r\n", task_fn);
			return false;
		}

		auto it = tasks.find(gTaskId);
		if (it == tasks.end())
		{
			LogMsg("PARSE", "ERROR: task id %d not found in %s (%d tasks available)",
				gTaskId, task_fn, (int)tasks.size());
			printf("error: task id %d not found in %s\r\n", gTaskId, task_fn);
			return false;
		}

		TaskMeta& meta = it->second;

		// Only fill from task mapping if not explicitly specified on command line
		bool from_mapping = false;
		if (!gRange)
		{
			gRange = meta.range;
			from_mapping = true;
		}
		if (!gStartSet)
		{
			gStart.SetHexStr(meta.start_hex);
			gStartSet = true;
			from_mapping = true;
		}
		if (gPubKey.x.IsZero())
		{
			gPubKey.SetHexStr(meta.pubkey_hex);
			from_mapping = true;
		}

		LogMsg("PARSE", "Task #%d resolved: range=%d, start_set=%s, pubkey_set=%s, from_mapping=%s",
			gTaskId, gRange,
			gStartSet ? "yes" : "no",
			gPubKey.x.IsZero() ? "no" : "yes",
			from_mapping ? "yes" : "no (cli overrides)");

		printf("Task #%d: range=%d\r\n", gTaskId, gRange);
	}

	if (!gPubKey.x.IsZero())
		if (!gStartSet || !gRange || !gDP)
		{
			printf("error: you must also specify -dp, -range and -start options (or use -task)\r\n");
			return false;
		}
	if (gTamesFileName[0] && !IsFileExist(gTamesFileName))
	{
		if (gMax == 0.0)
		{
			printf("error: you must also specify -max option to generate tames\r\n");
			return false;
		}
		gGenMode = true;
	}
	return true;
}

int main(int argc, char* argv[])
{
#ifdef _DEBUG	
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	printf("********************************************************************************\r\n");
	printf("*                  RCKangaroo v4.0 (c) 2024-2026 RetiredCoder                  *\r\n");
	printf("********************************************************************************\r\n\r\n");

	printf("This software is free and open-source: https://github.com/RetiredC\r\n");
	printf("It demonstrates fast GPU implementation of SOTA v2 Kangaroo method for solving ECDLP\r\n");
	printf("This version is optimized for 4xxx and 5xxx cards, for older cards use previous versions for best performance!\r\n");
#ifdef _WIN32
	printf("Windows version\r\n");
#else
	printf("Linux version\r\n");
#endif

#ifdef DEBUG_MODE
	printf("DEBUG MODE\r\n\r\n");
#endif

	InitEc();
	SetRndSeed(GetTickCount64());
	gDP = 0;
	gRange = 0;
	gStartSet = false;
	gTamesFileName[0] = 0;
	gMax = 0.0;
	gGenMode = false;
	gIsOpsLimit = false;
	memset(gGPUs_Mask, 1, sizeof(gGPUs_Mask));

	// Checkpoint defaults
	gSaveFileName[0] = 0;
	gTaskFileName[0] = 0;
	gTaskId = 0;
	gNoSave = false;
	gSaveSec = DEFAULT_SAVE_SEC;
	gCheckpointSec = DEFAULT_CHECKPOINT_SEC;
	gOpsDone = 0;
	gTaskStartTime = 0;
	gExitRequested = false;
	gJournalBuf = NULL;
	gJournalBufCnt = 0;

	if (!ParseCommandLine(argc, argv))
		return 0;

	// Show help if no arguments — ParseCommandLine already returned false for -help,
	// but also handle the case of no args at all (argc == 1)
	if (argc == 1)
	{
		ShowHelp();
		return 0;
	}

	// Apply defaults: -save defaults to task.dat if not specified and not -nosave
	if (!gNoSave && !gSaveFileName[0])
		strcpy(gSaveFileName, DEFAULT_SAVE_FILE);
	// -dp defaults to 16
	if (!gDP)
		gDP = DEFAULT_DP;

	LogMsg("INIT", "Config: save=%s, dp=%d, taskfile=%s, task_id=%d, nosave=%s, save_sec=%d, checkpoint_sec=%d",
		gSaveFileName[0] ? gSaveFileName : "(none)",
		gDP,
		gTaskFileName[0] ? gTaskFileName : DEFAULT_TASK_FILE,
		gTaskId,
		gNoSave ? "yes" : "no",
		gSaveSec, gCheckpointSec);

	// Install signal handler for graceful shutdown
#ifdef _WIN32
	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
	LogMsg("INIT", "Signal handler installed: SetConsoleCtrlHandler (Windows)");
#else
	signal(SIGINT, SigIntHandler);
	signal(SIGTERM, SigIntHandler);
	LogMsg("INIT", "Signal handler installed: SIGINT + SIGTERM (Linux)");
#endif

	InitGpus();

	if (!GpuCnt)
	{
		printf("No supported GPUs detected, exit\r\n");
		return 0;
	}

	// Verify required cubin files exist before attempting GPU prepare
	{
		bool sm89_needed = false;
		bool sm120_needed = false;
		for (int i = 0; i < GpuCnt; i++)
		{
			if (GpuKangs[i]->Is5xxx)
				sm120_needed = true;
			else
				sm89_needed = true;
		}

		if (sm89_needed)
		{
			if (!IsFileExist("kernel_sm89.cubin"))
			{
				printf("FATAL: kernel_sm89.cubin not found in current directory\r\n");
				printf("  Please ensure the cubin file is in the same directory as the executable.\r\n");
				return 0;
			}
			LogMsg("INIT", "cubin check: kernel_sm89.cubin found");
		}
		if (sm120_needed)
		{
			if (!IsFileExist("kernel_sm120.cubin"))
			{
				printf("FATAL: kernel_sm120.cubin not found in current directory\r\n");
				printf("  Please ensure the cubin file is in the same directory as the executable.\r\n");
				return 0;
			}
			LogMsg("INIT", "cubin check: kernel_sm120.cubin found");
		}
	}

	pPntList = (u8*)malloc(MAX_CNT_LIST * GPU_DP_SIZE);
	pPntList2 = (u8*)malloc(MAX_CNT_LIST * GPU_DP_SIZE);
	TotalOps = 0;
	TotalSolved = 0;
	gTotalErrors = 0;
	IsBench = gPubKey.x.IsZero();

	if (!IsBench && !gGenMode)
	{
		printf("\r\nMAIN MODE\r\n\r\n");
		EcPoint PntToSolve, PntOfs;
		EcInt pk, pk_found;

		PntToSolve = gPubKey;
		if (!gStart.IsZero())
		{
			PntOfs = ec.MultiplyG(gStart);
			PntOfs.y.NegModP();
			PntToSolve = ec.AddPoints(PntToSolve, PntOfs);
		}
		x32.Set(1);
		x32.ShiftLeft(gRange - 5);
		Pntx32 = ec.MultiplyG(x32);
		PntToSolve = ec.AddPoints(PntToSolve, Pntx32); //for smooth edges

		char sx[100], sy[100];
		gPubKey.x.GetHexStr(sx);
		gPubKey.y.GetHexStr(sy);
		printf("Solving public key\r\nX: %s\r\nY: %s\r\n", sx, sy);
		gStart.GetHexStr(sx);
			printf("Offset: %s\r\n", sx);

			// Set task start time for fresh starts (checkpoint recovery overrides this)
			if (!gTaskStartTime)
			{
				gTaskStartTime = GetTickCount64();
				LogMsg("INIT", "Fresh start: task_start_time=%llu (no checkpoint)", gTaskStartTime);
			}

			if (!SolvePoint(PntToSolve, gRange, gDP, &pk_found))
		{
			if (!gIsOpsLimit)
				printf("FATAL ERROR: SolvePoint failed\r\n");
			goto label_end;
		}
		pk_found.AddModP(gStart);
		pk_found.Sub(x32);
		EcPoint tmp = ec.MultiplyG(pk_found);
		if (!tmp.IsEqual(gPubKey))
		{
			printf("FATAL ERROR: SolvePoint found incorrect key\r\n");
			goto label_end;
		}
		//happy end
		char s[100];
		pk_found.GetHexStr(s);
		printf("\r\nPRIVATE KEY: %s\r\n\r\n", s);
		FILE* fp = fopen("RESULTS.TXT", "a");
		if (fp)
		{
			fprintf(fp, "PRIVATE KEY: %s\n", s);
			fclose(fp);
		}
		else //we cannot save the key, show error and wait forever so the key is displayed
		{
			printf("WARNING: Cannot save the key to RESULTS.TXT!\r\n");
			while (1)
				Sleep(100);
		}
	}
	else
	{
		if (gGenMode)
			printf("\r\nTAMES GENERATION MODE\r\n");
		else
			printf("\r\nBENCHMARK MODE\r\n");

		//solve points, show K
		while (1)
		{
			EcInt pk, pk_found;
			EcPoint PntToSolve;

			if (!gRange)
				gRange = 78;
			if (!gDP)
				gDP = 16;

			x32.Set(1);
			x32.ShiftLeft(gRange - 5);
			//generate random pk
			pk.RndBits(gRange);
			pk.Add(x32); //for smooth edges
			PntToSolve = ec.MultiplyG(pk);

			if (!SolvePoint(PntToSolve, gRange, gDP, &pk_found))
			{
				if (!gIsOpsLimit)
					printf("FATAL ERROR: SolvePoint failed\r\n");
				break;
			}
			pk.Sub(x32); //restore
			pk_found.Sub(x32);
			if (!pk_found.IsEqual(pk))
			{
				printf("FATAL ERROR: Found key is wrong!\r\n");
				break;
			}
			TotalOps += PntTotalOps;
			TotalSolved++;
			u64 ops_per_pnt = TotalOps / TotalSolved;
			double K = (double)ops_per_pnt / pow(2.0, gRange / 2.0);
			printf("Points solved: %d, average K: %.3f (with DP and GPU overheads)\r\n", TotalSolved, K);
			//if (TotalSolved >= 100) break; //dbg
		}
	}
label_end:
	for (int i = 0; i < GpuCnt; i++)
		delete GpuKangs[i];
	DeInitEc();
	free(pPntList2);
	free(pPntList);
}


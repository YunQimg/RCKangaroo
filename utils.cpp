// This file is a part of RCKangaroo software
// (c) 2024, RetiredCoder (RC)
// License: GPLv3, see "LICENSE.TXT" file
// https://github.com/RetiredC


#include "utils.h"
#include <wchar.h>

#ifdef _WIN32

#else

void _BitScanReverse64(u32* index, u64 msk) 
{
    *index = 63 - __builtin_clzll(msk); 
}

void _BitScanForward64(u32* index, u64 msk) 
{
    *index = __builtin_ffsll(msk) - 1; 
}

u64 _umul128(u64 m1, u64 m2, u64* hi) 
{ 
    uint128_t ab = (uint128_t)m1 * m2; *hi = (u64)(ab >> 64); return (u64)ab; 
}

u64 __shiftright128 (u64 LowPart, u64 HighPart, u8 Shift)
{
   u64 ret;
   __asm__ ("shrd {%[Shift],%[HighPart],%[LowPart]|%[LowPart], %[HighPart], %[Shift]}" 
      : [ret] "=r" (ret)
      : [LowPart] "0" (LowPart), [HighPart] "r" (HighPart), [Shift] "Jc" (Shift)
      : "cc");
   return ret;
}

u64 __shiftleft128 (u64 LowPart, u64 HighPart, u8 Shift)
{
   u64 ret;
   __asm__ ("shld {%[Shift],%[LowPart],%[HighPart]|%[HighPart], %[LowPart], %[Shift]}" 
      : [ret] "=r" (ret)
      : [LowPart] "r" (LowPart), [HighPart] "0" (HighPart), [Shift] "Jc" (Shift)
      : "cc");
   return ret;
}   

u64 GetTickCount64()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (u64)(ts.tv_nsec / 1000000) + ((u64)ts.tv_sec * 1000ull);
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define DB_REC_LEN			32
#define DB_FIND_LEN			9
#define DB_MIN_GROW_CNT		2

//we need advanced memory management to reduce memory fragmentation
//everything will be stable up to about 8TB RAM

#define MEM_PAGE_SIZE		(128 * 1024)
#define RECS_IN_PAGE		(MEM_PAGE_SIZE / DB_REC_LEN)
#define MAX_PAGES_CNT		(0xFFFFFFFF / RECS_IN_PAGE)

MemPool::MemPool()
{
	pnt = 0;
}

MemPool::~MemPool()
{
	Clear();
}

void MemPool::Clear()
{
	int cnt = (int)pages.size();
	for (int i = 0; i < cnt; i++)
		free(pages[i]);
	pages.clear();
	pnt = 0;
}

void* MemPool::AllocRec(u32* cmp_ptr)
{
	void* mem;
	if (pages.empty() || (pnt + DB_REC_LEN > MEM_PAGE_SIZE))
	{
		if (pages.size() >= MAX_PAGES_CNT)
			return NULL; //overflow
		pages.push_back(malloc(MEM_PAGE_SIZE));
		pnt = 0;
	}
	u32 page_ind = (u32)pages.size() - 1;
	mem = (u8*)pages[page_ind] + pnt;
	*cmp_ptr = (page_ind * RECS_IN_PAGE) | (pnt / DB_REC_LEN);
	pnt += DB_REC_LEN;
	return mem;
}

void* MemPool::GetRecPtr(u32 cmp_ptr)
{
	u32 page_ind = cmp_ptr / RECS_IN_PAGE;
	u32 rec_ind = cmp_ptr % RECS_IN_PAGE;
	return (u8*)pages[page_ind] + DB_REC_LEN * rec_ind;
}

TFastBase::TFastBase()
{
	memset(lists, 0, sizeof(lists));
	memset(Header, 0, sizeof(Header));
}

TFastBase::~TFastBase()
{
	Clear();
}

void TFastBase::Clear()
{
	for (int i = 0; i < 256; i++)
	{
		for (int j = 0; j < 256; j++)
			for (int k = 0; k < 256; k++)
			{
				if (lists[i][j][k].data)
					free(lists[i][j][k].data);
				lists[i][j][k].data = NULL;
				lists[i][j][k].capacity = 0;
				lists[i][j][k].cnt = 0;
			}
		mps[i].Clear();
	}
}

u64 TFastBase::GetBlockCnt()
{
	u64 blockCount = 0;
	for (int i = 0; i < 256; i++)
		for (int j = 0; j < 256; j++)
			for (int k = 0; k < 256; k++)
			blockCount += lists[i][j][k].cnt;
	return blockCount;
}

// http://en.cppreference.com/w/cpp/algorithm/lower_bound
int TFastBase::lower_bound(TListRec* list, int mps_ind, u8* data)
{
	int count = list->cnt;
	int it, first, step;
	first = 0;
	while (count > 0)
	{
		it = first;
		step = count / 2;   
		it += step;
		void* ptr = mps[mps_ind].GetRecPtr(list->data[it]);
		if (memcmp(ptr, data, DB_FIND_LEN) < 0)
		{
			first = ++it;
			count -= step + 1;
		}
		else
			count = step;
	}
	return first;
}
 
u8* TFastBase::AddDataBlock(u8* data, int pos)
{
	TListRec* list = &lists[data[0]][data[1]][data[2]];
	if (list->cnt >= list->capacity)
	{
		u32 grow = list->capacity / 2;
		if (grow < DB_MIN_GROW_CNT)
			grow = DB_MIN_GROW_CNT;
		u32 newcap = list->capacity + grow;
		if (newcap > 0xFFFF)
			newcap = 0xFFFF;
		if (newcap <= list->capacity)
			return NULL; //failed
		list->data = (u32*)realloc(list->data, newcap * sizeof(u32));
		list->capacity = newcap;
	}
	int first = (pos < 0) ? lower_bound(list, data[0], data + 3) : pos;
	memmove(list->data + first + 1, list->data + first, (list->cnt - first) * sizeof(u32));
	u32 cmp_ptr;
	void* ptr = mps[data[0]].AllocRec(&cmp_ptr);
	list->data[first] = cmp_ptr;
	memcpy(ptr, data + 3, DB_REC_LEN);
	list->cnt++;
	return (u8*)ptr;
}

u8* TFastBase::FindDataBlock(u8* data)
{
	bool res = false;
	TListRec* list = &lists[data[0]][data[1]][data[2]];
	int first = lower_bound(list, data[0], data + 3);
	if (first == list->cnt)
		return NULL;
	void* ptr = mps[data[0]].GetRecPtr(list->data[first]);
	if (memcmp(ptr, data + 3, DB_FIND_LEN))
		return NULL;
	return (u8*)ptr;
}

u8* TFastBase::FindOrAddDataBlock(u8* data)
{
	void* ptr;
	TListRec* list = &lists[data[0]][data[1]][data[2]];
	int first = lower_bound(list, data[0], data + 3);
	if (first == list->cnt)
		goto label_not_found;
	ptr = mps[data[0]].GetRecPtr(list->data[first]);
	if (memcmp(ptr, data + 3, DB_FIND_LEN))
		goto label_not_found;
	return (u8*)ptr;
label_not_found:
	AddDataBlock(data, first);
	return NULL;
}

//slow but I hope you are not going to create huge DB with this proof-of-concept software
bool TFastBase::LoadFromFile(char* fn)
{
	Clear();
	FILE* fp = fopen(fn, "rb");
	if (!fp)
		return false;
	if (fread(Header, 1, sizeof(Header), fp) != sizeof(Header))
	{
		fclose(fp);
		return false;
	}
	for (int i = 0; i < 256; i++)
		for (int j = 0; j < 256; j++)
			for (int k = 0; k < 256; k++)
			{
				TListRec* list = &lists[i][j][k];
				fread(&list->cnt, 1, 2, fp);
				if (list->cnt)
				{
					u32 grow = list->cnt / 2;
					if (grow < DB_MIN_GROW_CNT)
						grow = DB_MIN_GROW_CNT;
					u32 newcap = list->cnt + grow;
					if (newcap > 0xFFFF)
						newcap = 0xFFFF;
					list->data = (u32*)realloc(list->data, newcap * sizeof(u32));
					list->capacity = newcap;

					for (int m = 0; m < list->cnt; m++)
					{
						u32 cmp_ptr;
						void* ptr = mps[i].AllocRec(&cmp_ptr);
						list->data[m] = cmp_ptr;
						if (fread(ptr, 1, DB_REC_LEN, fp) != DB_REC_LEN)
						{
							fclose(fp);
							return false;
						}
					}
				}
			}
	fclose(fp);
	return true;
}

bool TFastBase::SaveToFile(char* fn)
{
	FILE* fp = fopen(fn, "wb");
	if (!fp)
		return false;
	if (fwrite(Header, 1, sizeof(Header), fp) != sizeof(Header))
	{
		fclose(fp);
		return false;
	}
	for (int i = 0; i < 256; i++)
		for (int j = 0; j < 256; j++)
			for (int k = 0; k < 256; k++)
			{
				TListRec* list = &lists[i][j][k];
				fwrite(&list->cnt, 1, 2, fp);
				for (int m = 0; m < list->cnt; m++)
				{
					void* ptr = mps[i].GetRecPtr(list->data[m]);
					if (fwrite(ptr, 1, DB_REC_LEN, fp) != DB_REC_LEN)
					{
						fclose(fp);
						return false;
					}
				}
			}
	fclose(fp);
	return true;
}

bool IsFileExist(char* fn)
{
	FILE* fp = fopen(fn, "rb");
	if (!fp)
		return false;
	fclose(fp);
	return true;
}

int GetExeDir(char* out_dir, int out_dir_size)
{
	if (!out_dir || out_dir_size == 0) return 0;
	out_dir[0] = '\0';

#ifdef _WIN32
	DWORD len = GetModuleFileNameA(NULL, out_dir, (DWORD)out_dir_size);
	if (len == 0) return 0;

	// If the buffer is too small, Windows returns out_dir_size (truncated) for GetModuleFileNameA.
	if ((int)len >= out_dir_size) {
		out_dir[0] = '\0';
		return 0;
	}

	// Cut off exe name, leave directory
	char* last_slash = strrchr(out_dir, '\\');
	if (last_slash) *last_slash = '\0';
	return 1;

#else
	// /proc/self/exe is a symlink to the running executable on Linux
	ssize_t len = readlink("/proc/self/exe", out_dir, out_dir_size - 1);
	if (len < 0) return 0;

	// readlink does NOT null-terminate
	out_dir[len] = '\0';

	// Cut off exe name, leave directory
	char* last_slash = strrchr(out_dir, '/');
	if (last_slash) *last_slash = '\0';
	return 1;
#endif
}

// ============================================================================
// Helper: construct derived file names
// ============================================================================

void MakeJournalName(const char* save_fn, char* out_journal, int out_size)
{
	strncpy(out_journal, save_fn, out_size - 1);
	out_journal[out_size - 1] = '\0';
	char* dot = strrchr(out_journal, '.');
	if (dot && strcmp(dot, ".dat") == 0)
		strcpy(dot, ".log");
	else
		strcat(out_journal, ".log");
}

void MakeTmpName(const char* save_fn, char* out_tmp, int out_size)
{
	strncpy(out_tmp, save_fn, out_size - 1);
	out_tmp[out_size - 1] = '\0';
	char* dot = strrchr(out_tmp, '.');
	if (dot && strcmp(dot, ".dat") == 0)
		strcpy(dot, ".tmp");
	else
		strcat(out_tmp, ".tmp");
}

// ============================================================================
// TFastBase::SaveToFileEx — extended save with metadata header
// ============================================================================

bool TFastBase::SaveToFileEx(char* fn, int range, int dp, int mode,
	const char* start_hex, const char* pubkey_hex, u64 ops_done, u64 task_start_time)
{
	// Build metadata header
	u8 hdr[256];
	memset(hdr, 0, sizeof(hdr));
	hdr[HDR_OFF_RANGE]      = (u8)range;
	hdr[HDR_OFF_DP]         = (u8)dp;
	hdr[HDR_OFF_MODE]       = (u8)mode;
	hdr[HDR_OFF_FORMAT_VER] = HEADER_FORMAT_VERSION;

	if (start_hex)
		strncpy((char*)hdr + HDR_OFF_START_HEX, start_hex, 43);
	if (pubkey_hex)
		strncpy((char*)hdr + HDR_OFF_PUBKEY_HEX, pubkey_hex, 111);

	*(u64*)(hdr + HDR_OFF_OPS_DONE)   = ops_done;
	*(u64*)(hdr + HDR_OFF_SAVED_TIME) = GetTickCount64();
	*(u64*)(hdr + HDR_OFF_TASK_START) = task_start_time;

	// Write via standard SaveToFile after setting Header
	u8 saved_header[256];
	memcpy(saved_header, Header, 256);  // save original
	memcpy(Header, hdr, 256);           // set metadata

	bool ok = SaveToFile(fn);

	memcpy(Header, saved_header, 256);  // restore original
	return ok;
}

// ============================================================================
// ValidateMeta — verify checkpoint metadata matches current task
// ============================================================================

bool ValidateMeta(u8* header, int range, int dp, int mode, const char* start_hex, const char* pubkey_hex)
{
	u8 fmt_ver = header[HDR_OFF_FORMAT_VER];

	// Legacy tames file (format_version == 0): only check range
	if (fmt_ver == 0)
	{
		if (header[HDR_OFF_RANGE] != (u8)range)
		{
			printf("ValidateMeta: range mismatch (header=%d, expected=%d)\r\n", header[HDR_OFF_RANGE], range);
			return false;
		}
		return true;
	}

	// New format (format_version == 1): full validation
	if (fmt_ver != HEADER_FORMAT_VERSION)
	{
		printf("ValidateMeta: unsupported format version %d, expected %d\r\n", fmt_ver, HEADER_FORMAT_VERSION);
		return false;
	}

	if (header[HDR_OFF_RANGE] != (u8)range)
	{
		printf("ValidateMeta: range mismatch (header=%d, expected=%d)\r\n", header[HDR_OFF_RANGE], range);
		return false;
	}
	if (header[HDR_OFF_DP] != (u8)dp)
	{
		printf("ValidateMeta: dp mismatch (header=%d, expected=%d)\r\n", header[HDR_OFF_DP], dp);
		return false;
	}
	if (header[HDR_OFF_MODE] != (u8)mode)
	{
		printf("ValidateMeta: mode mismatch (header=%d, expected=%d)\r\n", header[HDR_OFF_MODE], mode);
		return false;
	}

	if (start_hex && start_hex[0])
	{
		char hdr_start[44];
		strncpy(hdr_start, (char*)header + HDR_OFF_START_HEX, 43);
		hdr_start[43] = '\0';
		if (strcmp(hdr_start, start_hex) != 0)
		{
			printf("ValidateMeta: start mismatch\r\n");
			return false;
		}
	}

	if (pubkey_hex && pubkey_hex[0])
	{
		char hdr_pubkey[112];
		strncpy(hdr_pubkey, (char*)header + HDR_OFF_PUBKEY_HEX, 111);
		hdr_pubkey[111] = '\0';
		if (strcmp(hdr_pubkey, pubkey_hex) != 0)
		{
			printf("ValidateMeta: pubkey mismatch\r\n");
			return false;
		}
	}

	return true;
}

// ============================================================================
// Journal (WAL) I/O — AppendJournalFile / ReplayJournalFile
// ============================================================================

bool AppendJournalFile(const char* fn, const u8* buf, int cnt)
{
	if (cnt <= 0) return true;
	FILE* fp = fopen(fn, "ab");
	if (!fp)
	{
		printf("AppendJournalFile: cannot open %s for append\r\n", fn);
		return false;
	}
	size_t total = (size_t)cnt * JOURNAL_REC_LEN;
	size_t written = fwrite(buf, 1, total, fp);
	fclose(fp);
	if (written != total)
	{
		printf("AppendJournalFile: write incomplete (%zu/%zu bytes)\r\n", written, total);
		return false;
	}
	return true;
}

bool ReplayJournalFile(const char* fn, TFastBase& db)
{
	FILE* fp = fopen(fn, "rb");
	if (!fp)
	{
		printf("ReplayJournalFile: no journal file %s, skipping\r\n", fn);
		return false; // no journal to replay — not an error
	}

	// Get file size
	fseek(fp, 0, SEEK_END);
	long fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (fsize <= 0)
	{
		printf("ReplayJournalFile: %s is empty, skipping\r\n", fn);
		fclose(fp);
		return true;
	}

	// Tail truncation: discard last partial record
	long trunc_size = fsize - (fsize % JOURNAL_REC_LEN);
	if (trunc_size != fsize)
		printf("ReplayJournalFile: truncating %d trailing bytes (partial record at end of %s)\r\n",
			(int)(fsize - trunc_size), fn);

	if (trunc_size == 0)
	{
		printf("ReplayJournalFile: %s all trailing bytes, nothing to replay\r\n", fn);
		fclose(fp);
		return true;
	}

	int rec_cnt = (int)(trunc_size / JOURNAL_REC_LEN);
	printf("ReplayJournalFile: reading %d records (%ld bytes) from %s...\r\n", rec_cnt, trunc_size, fn);

	// Allocate buffer and read
	u8* buf = (u8*)malloc(trunc_size);
	if (!buf)
	{
		fclose(fp);
		printf("ReplayJournalFile: malloc failed for %ld bytes\r\n", trunc_size);
		return false;
	}

	size_t rd = fread(buf, 1, trunc_size, fp);
	fclose(fp);

	if (rd != (size_t)trunc_size)
	{
		printf("ReplayJournalFile: read incomplete (%zu/%ld bytes)\r\n", rd, trunc_size);
		free(buf);
		return false;
	}

	// Replay each record via FindOrAddDataBlock (idempotent)
	int new_cnt = 0, dup_cnt = 0;
	for (int i = 0; i < rec_cnt; i++)
	{
		u8* ret = db.FindOrAddDataBlock(buf + i * JOURNAL_REC_LEN);
		if (ret == NULL)
			new_cnt++;
		else
			dup_cnt++;
	}

	printf("ReplayJournalFile: %d records replayed (%d new, %d duplicate) from %s\r\n",
		rec_cnt, new_cnt, dup_cnt, fn);
	free(buf);
	return true;
}

// ============================================================================
// LoadTaskMapping — parse tasks.txt
// ============================================================================

std::map<int, TaskMeta> LoadTaskMapping(const char* fn)
{
	std::map<int, TaskMeta> result;

	FILE* fp = fopen(fn, "r");
	if (!fp)
	{
		printf("LoadTaskMapping: cannot open %s\r\n", fn);
		return result; // empty map
	}

	char line[MAX_TASK_LINE];
	int line_no = 0;

	while (fgets(line, sizeof(line), fp))
	{
		line_no++;

		// Trim trailing newline
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		// Skip empty lines and comments
		if (len == 0 || line[0] == '#')
			continue;

		int id, range;
		char start_hex[64] = { 0 };
		char pubkey_hex[130] = { 0 };

		int n = sscanf(line, "%d %d %63s %129s", &id, &range, start_hex, pubkey_hex);
		if (n < 4)
		{
			printf("LoadTaskMapping: line %d: invalid format (expected 4 fields), skipping\r\n", line_no);
			continue;
		}

		if (id <= 0)
		{
			printf("LoadTaskMapping: line %d: invalid id %d, skipping\r\n", line_no, id);
			continue;
		}

		if (range < 32 || range > 256)
		{
			printf("LoadTaskMapping: line %d: range %d out of bounds, skipping\r\n", line_no, range);
			continue;
		}

		// Check for duplicate id (warn and overwrite)
		if (result.find(id) != result.end())
			printf("LoadTaskMapping: duplicate id %d on line %d, overwriting previous\r\n", id, line_no);

		TaskMeta meta;
		meta.range = range;
		strncpy(meta.start_hex, start_hex, 63);
		strncpy(meta.pubkey_hex, pubkey_hex, 129);
		result[id] = meta;
	}

	fclose(fp);
	printf("LoadTaskMapping: loaded %d tasks from %s\r\n", (int)result.size(), fn);
	return result;
}

